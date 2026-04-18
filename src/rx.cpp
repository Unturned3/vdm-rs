#include "vdm_rs/reed_solomon.hpp"
#include "vdm_rs/stream_demo.hpp"
#include <argparse/argparse.hpp>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <netdb.h>
#include <optional>
#include <span>
#include <string>
#include <sys/select.h>
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
    std::size_t max_inflight = 32;
    std::int64_t timeout_ms = 1000;
    bool exit_on_eof = false;
    std::int64_t stats_ms = 1000;
    std::string host;
    std::string port;
};

struct Stats
{
    std::uint64_t packets_received = 0;
    std::uint64_t invalid_packets = 0;
    std::uint64_t duplicate_shards = 0;
    std::uint64_t late_shards = 0;
    std::uint64_t corrupt_data_shards = 0;
    std::uint64_t reconstructed_stripes = 0;
    std::uint64_t lost_stripes = 0;
    std::uint64_t eof_markers_received = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t latency_samples = 0;
    std::uint64_t latency_total_ns = 0;
    std::uint64_t latency_max_ns = 0;
};

[[nodiscard]] auto percent(std::uint64_t numerator, std::uint64_t denominator) -> double
{
    if (denominator == 0) {
        return 0.0;
    }
    return (100.0 * static_cast<double>(numerator))
           / static_cast<double>(denominator);
}

struct StripeState
{
    explicit StripeState(std::size_t shard_count, std::size_t shard_size)
        : storage(shard_count, std::vector<std::uint8_t>(shard_size, std::uint8_t { 0 }))
        , present(shard_count, false)
    {
    }

    std::vector<std::vector<std::uint8_t>> storage;
    std::vector<bool> present;
    std::size_t unique_count = 0;
    bool reconstructed = false;
    bool final_stripe = false;
    std::uint64_t tx_time_ns = 0;
    Clock::time_point first_seen = Clock::now();
};

class SocketHandle
{
public:
    explicit SocketHandle(int fd)
        : fd_(fd)
    {
    }

    SocketHandle(const SocketHandle&) = delete;
    auto operator=(const SocketHandle&) -> SocketHandle& = delete;

    ~SocketHandle()
    {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    [[nodiscard]] auto get() const noexcept -> int { return fd_; }

private:
    int fd_ = -1;
};

[[nodiscard]] auto parse_args(int argc, char* argv[]) -> Options
{
    argparse::ArgumentParser program("rx");
    program.add_argument("-k").default_value(std::size_t { 8 }).scan<'u', std::size_t>();
    program.add_argument("-t").default_value(std::size_t { 4 }).scan<'u', std::size_t>();
    program.add_argument("-s")
        .default_value(std::size_t { 1400 })
        .scan<'u', std::size_t>();
    program.add_argument("-p")
        .default_value(std::uint16_t { 0 })
        .scan<'u', std::uint16_t>();
    program.add_argument("-d")
        .default_value(std::size_t { 32 })
        .scan<'u', std::size_t>();
    program.add_argument("--timeout-ms")
        .default_value(std::int64_t { 1000 })
        .scan<'i', std::int64_t>();
    program.add_argument("--exit-on-eof").flag();
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
    options.max_inflight = program.get<std::size_t>("-d");
    options.timeout_ms = program.get<std::int64_t>("--timeout-ms");
    options.exit_on_eof = program.get<bool>("--exit-on-eof");
    options.stats_ms = program.get<std::int64_t>("--stats-ms");
    options.host = program.get<std::string>("host");
    options.port = program.get<std::string>("port");

    if (options.max_inflight == 0) {
        throw std::invalid_argument("-d must be positive");
    }
    if (options.timeout_ms <= 0) {
        throw std::invalid_argument("--timeout-ms must be positive");
    }
    if (options.stats_ms <= 0) {
        throw std::invalid_argument("--stats-ms must be positive");
    }
    if (options.shard_payload_size <= stream_demo::data_shard_header_size) {
        throw std::invalid_argument("shard payload size too small for protected header");
    }

    const auto codec = ReedSolomonCodec::create(options.k, options.t);
    (void)codec;
    return options;
}

[[nodiscard]] auto bind_socket(const std::string& host, const std::string& port) -> int
{
    std::string last_error = "unknown bind error";

    for (const bool bind_any : { false, true }) {
        addrinfo hints {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags = bind_any ? AI_PASSIVE : 0;

        const char* host_arg = bind_any ? nullptr : host.c_str();
        addrinfo* result = nullptr;
        const int rc = ::getaddrinfo(host_arg, port.c_str(), &hints, &result);
        if (rc != 0) {
            last_error = std::string("getaddrinfo failed: ") + gai_strerror(rc);
            continue;
        }

        std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(result,
                                                                   ::freeaddrinfo);
        for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
            const int fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
            if (fd < 0) {
                last_error = std::strerror(errno);
                continue;
            }

            const int reuse = 1;
            (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

            if (::bind(fd, it->ai_addr, static_cast<socklen_t>(it->ai_addrlen)) == 0) {
                const int flags = ::fcntl(fd, F_GETFL, 0);
                if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
                    ::close(fd);
                    throw std::runtime_error("failed to set nonblocking mode");
                }
                return fd;
            }

            last_error = std::strerror(errno);
            ::close(fd);
        }
    }

    throw std::runtime_error("failed to bind UDP socket: " + last_error);
}

void log_stats(const Stats& stats, std::size_t inflight, Clock::time_point start)
{
    const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    const double avg_latency_ms
        = stats.latency_samples == 0
              ? 0.0
              : static_cast<double>(stats.latency_total_ns)
                    / static_cast<double>(stats.latency_samples) / 1'000'000.0;
    const double max_latency_ms
        = static_cast<double>(stats.latency_max_ns) / 1'000'000.0;
    const double goodput
        = elapsed > 0.0 ? static_cast<double>(stats.bytes_written) / elapsed : 0.0;
    const auto completed = stats.reconstructed_stripes + stats.lost_stripes;

    std::cerr << std::fixed << std::setprecision(2)
              << "rx stats: elapsed_s=" << elapsed
              << " packets_received=" << stats.packets_received
              << " invalid_packets=" << stats.invalid_packets
              << " duplicate_shards=" << stats.duplicate_shards
              << " duplicate_pct="
              << percent(stats.duplicate_shards, stats.packets_received)
              << " late_shards=" << stats.late_shards
              << " corrupt_data_shards=" << stats.corrupt_data_shards
              << " reconstructed_stripes=" << stats.reconstructed_stripes
              << " lost_stripes=" << stats.lost_stripes
              << " stripe_loss_pct=" << percent(stats.lost_stripes, completed)
              << " eof_markers_received=" << stats.eof_markers_received
              << " bytes_written=" << stats.bytes_written
              << " goodput=" << stream_demo::describe_rate_bps(goodput)
              << " avg_latency_ms=" << avg_latency_ms
              << " max_latency_ms=" << max_latency_ms
              << " inflight=" << inflight << '\n';
}

[[nodiscard]] auto validate_data_shard(std::span<const std::uint8_t> shard,
                                       std::size_t shard_payload_size)
    -> std::optional<stream_demo::DataShardHeader>
{
    if (shard.size() != shard_payload_size) {
        return std::nullopt;
    }

    const auto data_header = stream_demo::parse_data_shard_header(
        shard.first(stream_demo::data_shard_header_size));
    if (!data_header.has_value()) {
        return std::nullopt;
    }

    const std::size_t data_capacity
        = shard_payload_size - stream_demo::data_shard_header_size;
    if (data_header->data_length > data_capacity) {
        return std::nullopt;
    }

    const auto payload = shard.subspan(stream_demo::data_shard_header_size,
                                       data_header->data_length);
    if (stream_demo::compute_crc32(payload) != data_header->payload_crc32) {
        return std::nullopt;
    }

    return data_header;
}

[[nodiscard]] auto maybe_missing_stripe_timed_out(
    std::uint64_t next_emit, const std::map<std::uint64_t, StripeState>& stripes,
    std::chrono::milliseconds timeout, Clock::time_point now) -> bool
{
    auto candidate = Clock::time_point::max();
    for (const auto& [stripe_id, stripe] : stripes) {
        if (stripe_id > next_emit) {
            candidate = std::min(candidate, stripe.first_seen);
        }
    }
    if (candidate == Clock::time_point::max()) {
        return false;
    }
    return now - candidate >= timeout;
}

[[nodiscard]] auto emit_stripe(StripeState& stripe, std::size_t k,
                               std::size_t shard_payload_size, Stats& stats)
    -> std::optional<bool>
{
    bool saw_final = stripe.final_stripe;
    std::vector<stream_demo::DataShardHeader> headers;
    headers.reserve(k);

    for (std::size_t shard_index = 0; shard_index < k; ++shard_index) {
        const auto shard = std::span<const std::uint8_t>(stripe.storage[shard_index]);
        const auto data_header = validate_data_shard(shard, shard_payload_size);
        if (!data_header.has_value()) {
            return std::nullopt;
        }
        if ((data_header->flags & stream_demo::data_flag_final_stripe) != 0U) {
            saw_final = true;
        }
        headers.push_back(*data_header);
    }

    for (std::size_t shard_index = 0; shard_index < k; ++shard_index) {
        const auto* payload_begin = reinterpret_cast<const char*>(
            stripe.storage[shard_index].data() + stream_demo::data_shard_header_size);
        std::cout.write(payload_begin,
                        static_cast<std::streamsize>(headers[shard_index].data_length));
        if (!std::cout) {
            throw std::runtime_error("stdout write failed");
        }
        stats.bytes_written += headers[shard_index].data_length;
    }

    std::cout.flush();
    return saw_final;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const Options options = parse_args(argc, argv);
        const auto codec = ReedSolomonCodec::create(options.k, options.t);
        const SocketHandle socket(bind_socket(options.host, options.port));
        const std::size_t total_shards = options.k + options.t;
        const auto timeout = std::chrono::milliseconds(options.timeout_ms);
        const auto start = Clock::now();

        std::cerr << "rx config: host=" << options.host
                  << " port=" << options.port
                  << " k=" << options.k
                  << " t=" << options.t
                  << " total_shards=" << total_shards
                  << " shard_payload_size=" << options.shard_payload_size
                  << " max_inflight=" << options.max_inflight
                  << " timeout_ms=" << options.timeout_ms
                  << " exit_on_eof=" << (options.exit_on_eof ? "true" : "false")
                  << " stream_id=" << options.stream_id
                  << " stats_ms=" << options.stats_ms
                  << '\n';

        Stats stats;
        std::map<std::uint64_t, StripeState> stripes;
        std::uint64_t next_emit = 0;
        std::uint64_t highest_stripe_seen = 0;
        bool have_seen_packet = false;
        std::optional<std::uint64_t> eof_stripe_id;
        auto next_stats = Clock::now() + std::chrono::milliseconds(options.stats_ms);

        std::vector<std::uint8_t> recv_buffer(stream_demo::packet_header_size
                                              + options.shard_payload_size);

        while (true) {
            const auto now = Clock::now();

            bool should_exit = false;
            while (true) {
                auto it = stripes.find(next_emit);
                if (it != stripes.end() && it->second.reconstructed) {
                    const auto emit_result = emit_stripe(it->second, options.k,
                                                         options.shard_payload_size, stats);
                    const bool valid = emit_result.has_value();
                    const bool saw_final
                        = valid ? *emit_result : it->second.final_stripe;
                    stripes.erase(it);
                    ++next_emit;
                    if (!valid) {
                        if (stats.reconstructed_stripes > 0) {
                            --stats.reconstructed_stripes;
                        }
                        ++stats.lost_stripes;
                    }
                    if ((saw_final && options.exit_on_eof)
                        || (options.exit_on_eof && eof_stripe_id.has_value()
                            && next_emit > *eof_stripe_id)) {
                        should_exit = true;
                        break;
                    }
                    continue;
                }

                const bool window_overflow
                    = have_seen_packet
                      && highest_stripe_seen >= next_emit + options.max_inflight;
                const bool timed_out_missing = maybe_missing_stripe_timed_out(
                    next_emit, stripes, timeout, now);
                const bool timed_out_present
                    = it != stripes.end() && (now - it->second.first_seen) >= timeout;

                if (window_overflow || timed_out_present || timed_out_missing) {
                    const bool lost_final
                        = it != stripes.end() ? it->second.final_stripe : false;
                    stripes.erase(next_emit);
                    ++stats.lost_stripes;
                    ++next_emit;
                    if ((lost_final && options.exit_on_eof)
                        || (options.exit_on_eof && eof_stripe_id.has_value()
                            && next_emit > *eof_stripe_id)) {
                        should_exit = true;
                        break;
                    }
                    continue;
                }
                break;
            }

            if (should_exit) {
                log_stats(stats, stripes.size(), start);
                return 0;
            }

            if (now >= next_stats) {
                log_stats(stats, stripes.size(), start);
                next_stats = now + std::chrono::milliseconds(options.stats_ms);
            }

            auto timeout_left = std::chrono::duration_cast<std::chrono::milliseconds>(
                next_stats - now);
            if (timeout_left.count() < 0) {
                timeout_left = std::chrono::milliseconds(0);
            }

            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(socket.get(), &read_set);
            timeval tv {};
            tv.tv_sec = static_cast<long>(timeout_left.count() / 1000);
            tv.tv_usec = static_cast<int>((timeout_left.count() % 1000) * 1000);

            const int ready
                = ::select(socket.get() + 1, &read_set, nullptr, nullptr, &tv);
            if (ready < 0) {
                throw std::runtime_error("select failed");
            }
            if (ready == 0) {
                continue;
            }

            while (true) {
                const auto received = ::recv(socket.get(), recv_buffer.data(),
                                             recv_buffer.size(), 0);
                if (received < 0) {
                    break;
                }

                ++stats.packets_received;
                const auto packet_size = static_cast<std::size_t>(received);
                const auto bytes = std::span<const std::uint8_t>(recv_buffer.data(),
                                                                 packet_size);
                const auto header = stream_demo::parse_packet_header(bytes);
                if (!header.has_value()) {
                    ++stats.invalid_packets;
                    continue;
                }
                const bool is_eof_marker
                    = (header->flags & stream_demo::packet_flag_eof_marker) != 0U;
                if (!stream_demo::packet_header_crc_valid(*header)
                    || header->magic != stream_demo::packet_magic
                    || header->version != stream_demo::packet_version
                    || header->header_size != stream_demo::packet_header_size
                    || header->stream_id != options.stream_id
                    || header->k != options.k || header->t != options.t
                    || header->shard_payload_size != options.shard_payload_size) {
                    ++stats.invalid_packets;
                    continue;
                }
                if (packet_size != stream_demo::packet_header_size
                                       + options.shard_payload_size) {
                    ++stats.invalid_packets;
                    continue;
                }
                if (is_eof_marker) {
                    if (header->shard_index != total_shards) {
                        ++stats.invalid_packets;
                        continue;
                    }

                    ++stats.eof_markers_received;
                    eof_stripe_id = eof_stripe_id.has_value()
                                        ? std::max(*eof_stripe_id, header->stripe_id)
                                        : header->stripe_id;
                    highest_stripe_seen = std::max(highest_stripe_seen, header->stripe_id);
                    have_seen_packet = true;
                    continue;
                }
                if (header->shard_index >= total_shards) {
                    ++stats.invalid_packets;
                    continue;
                }
                if (header->stripe_id < next_emit) {
                    ++stats.late_shards;
                    continue;
                }

                have_seen_packet = true;
                highest_stripe_seen = std::max(highest_stripe_seen, header->stripe_id);

                auto [it, inserted] = stripes.try_emplace(
                    header->stripe_id, total_shards, options.shard_payload_size);
                StripeState& stripe = it->second;
                if (inserted) {
                    stripe.first_seen = Clock::now();
                    stripe.tx_time_ns = header->tx_time_ns;
                }
                if (stripe.present[header->shard_index]) {
                    ++stats.duplicate_shards;
                    continue;
                }

                const auto payload = bytes.subspan(header->header_size);
                if (header->shard_index < options.k
                    && !validate_data_shard(payload, options.shard_payload_size).has_value()) {
                    ++stats.invalid_packets;
                    ++stats.corrupt_data_shards;
                    continue;
                }

                std::copy(bytes.begin() + static_cast<std::ptrdiff_t>(header->header_size),
                          bytes.end(), stripe.storage[header->shard_index].begin());
                stripe.present[header->shard_index] = true;
                ++stripe.unique_count;
                stripe.final_stripe = stripe.final_stripe
                                      || ((header->flags
                                           & stream_demo::packet_flag_final_stripe)
                                          != 0U);

                if (!stripe.reconstructed && stripe.unique_count >= options.k) {
                    std::vector<Shard> shard_views;
                    shard_views.reserve(total_shards);
                    for (auto& shard_storage : stripe.storage) {
                        shard_views.push_back(
                            Shard { shard_storage.data(), shard_storage.size() });
                    }

                    if (codec.reconstruct(shard_views, stripe.present)) {
                        stripe.reconstructed = true;
                        ++stats.reconstructed_stripes;
                        const auto now_ns = stream_demo::steady_time_ns();
                        if (now_ns >= stripe.tx_time_ns) {
                            const auto latency = now_ns - stripe.tx_time_ns;
                            stats.latency_total_ns += latency;
                            stats.latency_max_ns = std::max(stats.latency_max_ns, latency);
                            ++stats.latency_samples;
                        }
                    }
                }
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "rx error: " << ex.what() << '\n';
        return 1;
    }
}
