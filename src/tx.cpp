#include "vdm_rs/reed_solomon.hpp"
#include "vdm_rs/stream_demo.hpp"
#include <argparse/argparse.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <netdb.h>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options
{
    std::size_t k = 8;
    std::size_t t = 4;
    std::size_t shard_payload_size = 1400;
    std::uint16_t stream_id = 0;
    std::size_t repeat_count = 1;
    std::int64_t stats_ms = 1000;
    std::string host;
    std::string port;
};

struct Destination
{
    int socket_fd = -1;
    int family = AF_UNSPEC;
    sockaddr_storage address {};
    socklen_t address_len = 0;
};

struct Stats
{
    std::uint64_t bytes_read = 0;
    std::uint64_t stripes_sent = 0;
    std::uint64_t packets_sent = 0;
    std::uint64_t output_bytes = 0;
};

// Converts a ratio into a percentage while handling a zero denominator.
[[nodiscard]] auto percent(std::uint64_t numerator, std::uint64_t denominator) -> double
{
    if (denominator == 0) {
        return 0.0;
    }
    return (100.0 * static_cast<double>(numerator))
           / static_cast<double>(denominator);
}

class SocketHandle
{
public:
    // Creates an empty handle that owns no socket.
    SocketHandle() = default;

    // Takes ownership of an already-open file descriptor.
    explicit SocketHandle(int fd)
        : fd_(fd)
    {
    }

    SocketHandle(const SocketHandle&) = delete;
    auto operator=(const SocketHandle&) -> SocketHandle& = delete;

    // Transfers socket ownership from another handle.
    SocketHandle(SocketHandle&& other) noexcept
        : fd_(other.fd_)
    {
        other.fd_ = -1;
    }

    // Replaces this handle with another owned socket.
    auto operator=(SocketHandle&& other) noexcept -> SocketHandle&
    {
        if (this != &other) {
            close_if_needed_();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    // Closes the owned socket on scope exit.
    ~SocketHandle() { close_if_needed_(); }

    // Returns the raw file descriptor without releasing ownership.
    [[nodiscard]] auto get() const noexcept -> int { return fd_; }

private:
    int fd_ = -1;

    // Closes the socket if this handle currently owns one.
    void close_if_needed_() noexcept
    {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
};

// Parses and validates the sender command-line options.
[[nodiscard]] auto parse_args(int argc, char* argv[]) -> Options
{
    argparse::ArgumentParser program("tx");
    program.add_argument("-k").default_value(std::size_t { 8 }).scan<'u', std::size_t>();
    program.add_argument("-t").default_value(std::size_t { 4 }).scan<'u', std::size_t>();
    program.add_argument("-s")
        .default_value(std::size_t { 1400 })
        .scan<'u', std::size_t>();
    program.add_argument("-p")
        .default_value(std::uint16_t { 0 })
        .scan<'u', std::uint16_t>();
    program.add_argument("-x").default_value(std::size_t { 1 }).scan<'u', std::size_t>();
    program.add_argument("--stats-ms")
        .default_value(std::int64_t { 1000 })
        .scan<'i', std::int64_t>();
    program.add_argument("host");
    program.add_argument("port");

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n' << program;
        std::exit(1);
    }

    Options options;
    options.k = program.get<std::size_t>("-k");
    options.t = program.get<std::size_t>("-t");
    options.shard_payload_size = program.get<std::size_t>("-s");
    options.stream_id = program.get<std::uint16_t>("-p");
    options.repeat_count = program.get<std::size_t>("-x");
    options.stats_ms = program.get<std::int64_t>("--stats-ms");
    options.host = program.get<std::string>("host");
    options.port = program.get<std::string>("port");

    if (options.repeat_count == 0) {
        throw std::invalid_argument("-x must be positive");
    }
    if (options.stats_ms <= 0) {
        throw std::invalid_argument("--stats-ms must be positive");
    }
    if (options.shard_payload_size <= stream_demo::data_shard_header_size) {
        throw std::invalid_argument("shard payload size too small for protected header");
    }
    if (stream_demo::packet_header_size + options.shard_payload_size > 65507U) {
        throw std::invalid_argument("UDP packet would exceed maximum payload size");
    }

    const auto codec = ReedSolomonCodec::create(options.k, options.t);
    (void)codec;
    return options;
}

// Resolves the destination address and opens a UDP socket for transmission.
[[nodiscard]] auto resolve_destination(const std::string& host, const std::string& port)
    -> Destination
{
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo* result = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &result);
    if (rc != 0) {
        throw std::runtime_error(std::string("getaddrinfo failed: ") + gai_strerror(rc));
    }

    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(result, ::freeaddrinfo);
    for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
        const int fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }

        Destination destination;
        destination.socket_fd = fd;
        destination.family = it->ai_family;
        std::memcpy(&destination.address, it->ai_addr,
                    static_cast<std::size_t>(it->ai_addrlen));
        destination.address_len = static_cast<socklen_t>(it->ai_addrlen);
        return destination;
    }

    throw std::runtime_error("failed to create UDP socket");
}

// Emits periodic sender throughput and FEC overhead statistics.
void log_stats(const Stats& stats, Clock::time_point start)
{
    const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    const double input_rate
        = elapsed > 0.0 ? static_cast<double>(stats.bytes_read) / elapsed : 0.0;
    const double output_rate
        = elapsed > 0.0 ? static_cast<double>(stats.output_bytes) / elapsed : 0.0;
    const double expansion
        = stats.bytes_read == 0
              ? 0.0
              : static_cast<double>(stats.output_bytes)
                    / static_cast<double>(stats.bytes_read);

    std::cerr << std::fixed << std::setprecision(2)
              << "tx stats: elapsed_s=" << elapsed
              << " bytes_read=" << stats.bytes_read
              << " stripes_sent=" << stats.stripes_sent
              << " packets_sent=" << stats.packets_sent
              << " packet_payload_bytes=" << stats.output_bytes
              << " input_rate=" << stream_demo::describe_rate_bps(input_rate)
              << " output_rate=" << stream_demo::describe_rate_bps(output_rate)
              << " fec_overhead_pct="
              << percent(stats.output_bytes > stats.bytes_read
                             ? stats.output_bytes - stats.bytes_read
                             : 0,
                         stats.bytes_read)
              << " expansion=" << expansion << "x"
              << '\n';
}

// Sends one fully assembled UDP packet to the configured destination.
void send_packet(const Destination& destination,
                 const std::vector<std::uint8_t>& packet)
{
    const auto sent = ::sendto(destination.socket_fd, packet.data(), packet.size(), 0,
                               reinterpret_cast<const sockaddr*>(&destination.address),
                               destination.address_len);
    if (sent < 0 || static_cast<std::size_t>(sent) != packet.size()) {
        throw std::runtime_error("sendto failed");
    }
}

} // namespace

// Reads stdin, encodes stripes, and streams them as UDP packets.
int main(int argc, char* argv[])
{
    try {
        const Options options = parse_args(argc, argv);
        const auto codec = ReedSolomonCodec::create(options.k, options.t);
        const auto destination = resolve_destination(options.host, options.port);
        const SocketHandle socket_guard(destination.socket_fd);

        const std::size_t data_capacity
            = options.shard_payload_size - stream_demo::data_shard_header_size;
        const auto send_order
            = stream_demo::make_shard_send_order(options.k, options.t);

        std::cerr << "tx config: host=" << options.host
                  << " port=" << options.port
                  << " k=" << options.k
                  << " t=" << options.t
                  << " total_shards=" << codec.total_shards()
                  << " shard_payload_size=" << options.shard_payload_size
                  << " data_bytes_per_shard=" << data_capacity
                  << " repeat=" << options.repeat_count
                  << " stream_id=" << options.stream_id
                  << " stats_ms=" << options.stats_ms
                  << '\n';

        Stats stats;
        const auto start = Clock::now();
        auto next_stats = start + std::chrono::milliseconds(options.stats_ms);
        bool finished = false;
        std::uint64_t stripe_id = 0;

        while (!finished) {
            std::vector<std::vector<std::uint8_t>> data_storage(
                options.k,
                std::vector<std::uint8_t>(options.shard_payload_size, std::uint8_t { 0 }));
            std::vector<std::size_t> shard_lengths(options.k, 0);

            bool reached_eof = false;
            for (std::size_t shard_index = 0; shard_index < options.k; ++shard_index) {
                // Reserve the prefix for the protected per-shard metadata.
                auto payload = std::span<std::uint8_t>(
                    data_storage[shard_index].data() + stream_demo::data_shard_header_size,
                    data_capacity);
                std::cin.read(reinterpret_cast<char*>(payload.data()),
                              static_cast<std::streamsize>(payload.size()));
                const auto count
                    = static_cast<std::size_t>(std::max<std::streamsize>(0, std::cin.gcount()));
                shard_lengths[shard_index] = count;
                stats.bytes_read += static_cast<std::uint64_t>(count);

                if (std::cin.bad()) {
                    throw std::runtime_error("stdin read failed");
                }
                if (count < payload.size()) {
                    if (!std::cin.eof()) {
                        throw std::runtime_error("stdin ended unexpectedly");
                    }
                    reached_eof = true;
                    break;
                }
            }

            if (!reached_eof && std::cin.peek() == std::char_traits<char>::eof()) {
                reached_eof = true;
            }

            const bool any_payload = std::any_of(
                shard_lengths.begin(), shard_lengths.end(),
                [](std::size_t length) { return length != 0; });
            if (!any_payload && stripe_id != 0 && reached_eof) {
                break;
            }

            const auto tx_time_ns = stream_demo::steady_time_ns();
            for (std::size_t shard_index = 0; shard_index < options.k; ++shard_index) {
                const std::uint16_t flags = reached_eof
                                                ? stream_demo::data_flag_final_stripe
                                                : std::uint16_t { 0 };
                stream_demo::encode_data_shard_header(
                    stream_demo::DataShardHeader {
                        .data_length = static_cast<std::uint32_t>(shard_lengths[shard_index]),
                        .flags = flags,
                        .payload_crc32 = 0,
                    },
                    std::span<std::uint8_t>(data_storage[shard_index].data(),
                                            stream_demo::data_shard_header_size));
            }

            std::vector<std::vector<std::uint8_t>> parity_storage(
                options.t,
                std::vector<std::uint8_t>(options.shard_payload_size, std::uint8_t { 0 }));

            std::vector<Shard> data_shards;
            data_shards.reserve(options.k);
            for (auto& shard : data_storage) {
                data_shards.push_back(Shard { shard.data(), shard.size() });
            }

            std::vector<Shard> parity_shards;
            parity_shards.reserve(options.t);
            for (auto& shard : parity_storage) {
                parity_shards.push_back(Shard { shard.data(), shard.size() });
            }

            codec.compute_parity(data_shards, parity_shards);

            for (std::size_t repeat = 0; repeat < options.repeat_count; ++repeat) {
                for (const std::size_t shard_index : send_order) {
                    const bool is_data = shard_index < options.k;
                    const auto& payload = is_data ? data_storage[shard_index]
                                                  : parity_storage[shard_index - options.k];
                    const auto flags = reached_eof ? stream_demo::packet_flag_final_stripe
                                                   : std::uint16_t { 0 };
                    const auto header = stream_demo::PacketHeader {
                        .magic = stream_demo::packet_magic,
                        .version = stream_demo::packet_version,
                        .header_size = static_cast<std::uint16_t>(
                            stream_demo::packet_header_size),
                        .stream_id = options.stream_id,
                        .flags = flags,
                        .stripe_id = stripe_id,
                        .tx_time_ns = tx_time_ns,
                        .shard_index = static_cast<std::uint16_t>(shard_index),
                        .k = static_cast<std::uint16_t>(options.k),
                        .t = static_cast<std::uint16_t>(options.t),
                        .shard_payload_size = static_cast<std::uint16_t>(
                            options.shard_payload_size),
                        .header_crc32 = 0,
                    };

                    // Packet layout is [fixed header][encoded shard payload].
                    auto packet = stream_demo::serialize_packet_header(header);
                    packet.insert(packet.end(), payload.begin(), payload.end());
                    send_packet(destination, packet);
                    ++stats.packets_sent;
                    stats.output_bytes += static_cast<std::uint64_t>(packet.size());
                }
            }

            ++stats.stripes_sent;
            ++stripe_id;

            if (Clock::now() >= next_stats) {
                log_stats(stats, start);
                next_stats = Clock::now() + std::chrono::milliseconds(options.stats_ms);
            }

            if (reached_eof) {
                finished = true;
            }
        }

        log_stats(stats, start);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "tx error: " << ex.what() << '\n';
        return 1;
    }
}
