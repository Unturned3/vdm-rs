#include "vdm_rs/stream_demo.hpp"
#include <argparse/argparse.hpp>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <memory>
#include <netdb.h>
#include <optional>
#include <random>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options
{
    double drop = 0.0;
    std::int64_t latency_ms = 0;
    std::int64_t jitter_ms = 0;
    std::uint64_t rate_bps = 0;
    std::size_t max_queue = 1024;
    std::uint64_t seed = 1;
    std::int64_t stats_ms = 1000;
    std::string host;
    std::string in_port;
    std::string out_port;
};

struct Destination
{
    int family = AF_UNSPEC;
    int protocol = 0;
    sockaddr_storage address {};
    socklen_t address_len = 0;
};

struct QueuedPacket
{
    std::vector<std::uint8_t> bytes;
    Clock::time_point ready_time;
};

struct Stats
{
    std::uint64_t packets_received = 0;
    std::uint64_t packets_forwarded = 0;
    std::uint64_t random_drops = 0;
    std::uint64_t queue_drops = 0;
    std::uint64_t bytes_forwarded = 0;
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
    // Takes ownership of an already-open socket descriptor.
    explicit SocketHandle(int fd)
        : fd_(fd)
    {
    }

    SocketHandle(const SocketHandle&) = delete;
    auto operator=(const SocketHandle&) -> SocketHandle& = delete;

    // Closes the socket on scope exit.
    ~SocketHandle()
    {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    // Returns the raw file descriptor without releasing ownership.
    [[nodiscard]] auto get() const noexcept -> int { return fd_; }

private:
    int fd_ = -1;
};

// Parses and validates the link simulation parameters.
[[nodiscard]] auto parse_args(int argc, char* argv[]) -> Options
{
    argparse::ArgumentParser program("link_sim");
    program.add_argument("--drop").default_value(0.0).scan<'g', double>();
    program.add_argument("--latency-ms")
        .default_value(std::int64_t { 0 })
        .scan<'i', std::int64_t>();
    program.add_argument("--jitter-ms")
        .default_value(std::int64_t { 0 })
        .scan<'i', std::int64_t>();
    program.add_argument("--rate-bps")
        .default_value(std::uint64_t { 0 })
        .scan<'u', std::uint64_t>();
    program.add_argument("--queue")
        .default_value(std::size_t { 1024 })
        .scan<'u', std::size_t>();
    program.add_argument("--seed")
        .default_value(std::uint64_t { 1 })
        .scan<'u', std::uint64_t>();
    program.add_argument("--stats-ms")
        .default_value(std::int64_t { 1000 })
        .scan<'i', std::int64_t>();
    program.add_argument("host");
    program.add_argument("in_port");
    program.add_argument("out_port");

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n' << program;
        std::exit(1);
    }

    Options options;
    options.drop = program.get<double>("--drop");
    options.latency_ms = program.get<std::int64_t>("--latency-ms");
    options.jitter_ms = program.get<std::int64_t>("--jitter-ms");
    options.rate_bps = program.get<std::uint64_t>("--rate-bps");
    options.max_queue = program.get<std::size_t>("--queue");
    options.seed = program.get<std::uint64_t>("--seed");
    options.stats_ms = program.get<std::int64_t>("--stats-ms");
    options.host = program.get<std::string>("host");
    options.in_port = program.get<std::string>("in_port");
    options.out_port = program.get<std::string>("out_port");

    if (options.drop < 0.0 || options.drop > 1.0) {
        throw std::invalid_argument("--drop must be in [0, 1]");
    }
    if (options.latency_ms < 0 || options.jitter_ms < 0) {
        throw std::invalid_argument("latency and jitter must be non-negative");
    }
    if (options.max_queue == 0) {
        throw std::invalid_argument("--queue must be positive");
    }
    if (options.stats_ms <= 0) {
        throw std::invalid_argument("--stats-ms must be positive");
    }

    return options;
}

// Binds a nonblocking UDP socket for incoming packets.
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

// Resolves the outgoing UDP destination address.
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
        Destination destination;
        destination.family = it->ai_family;
        destination.protocol = it->ai_protocol;
        std::memcpy(&destination.address, it->ai_addr,
                    static_cast<std::size_t>(it->ai_addrlen));
        destination.address_len = static_cast<socklen_t>(it->ai_addrlen);
        return destination;
    }

    throw std::runtime_error("failed to resolve destination");
}

// Emits simulator statistics about forwarding rate, drops, and queue depth.
void log_stats(const Stats& stats, std::size_t queued, std::uint64_t rate_bps,
               Clock::time_point start)
{
    const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    const double observed_rate
        = elapsed > 0.0 ? static_cast<double>(stats.bytes_forwarded) / elapsed : 0.0;
    const double configured_bps = static_cast<double>(rate_bps) / 8.0;
    const auto total_drops = stats.random_drops + stats.queue_drops;

    std::cerr << std::fixed << std::setprecision(2)
              << "link_sim stats: elapsed_s=" << elapsed
              << " packets_received=" << stats.packets_received
              << " packets_forwarded=" << stats.packets_forwarded
              << " random_drops=" << stats.random_drops
              << " queue_drops=" << stats.queue_drops
              << " total_drop_pct=" << percent(total_drops, stats.packets_received)
              << " queued=" << queued
              << " configured_rate="
              << stream_demo::describe_rate_bps(configured_bps)
              << " observed_rate=" << stream_demo::describe_rate_bps(observed_rate)
              << '\n';
}

} // namespace

// Applies configurable loss, delay, and rate limits to forwarded UDP packets.
int main(int argc, char* argv[])
{
    try {
        const Options options = parse_args(argc, argv);
        const SocketHandle in_socket(bind_socket(options.host, options.in_port));
        const Destination destination
            = resolve_destination(options.host, options.out_port);
        const SocketHandle out_socket(
            ::socket(destination.family, SOCK_DGRAM, destination.protocol));
        if (out_socket.get() < 0) {
            throw std::runtime_error("failed to create output socket");
        }

        std::cerr << "link_sim config: host=" << options.host
                  << " in_port=" << options.in_port
                  << " out_port=" << options.out_port
                  << " drop=" << options.drop
                  << " latency_ms=" << options.latency_ms
                  << " jitter_ms=" << options.jitter_ms
                  << " rate_bps=" << options.rate_bps
                  << " queue=" << options.max_queue
                  << " seed=" << options.seed
                  << " stats_ms=" << options.stats_ms
                  << '\n';

        std::mt19937_64 rng(options.seed);
        std::uniform_real_distribution<double> drop_dist(0.0, 1.0);
        std::uniform_int_distribution<std::int64_t> jitter_dist(
            0, std::max<std::int64_t>(0, options.jitter_ms));

        Stats stats;
        std::deque<QueuedPacket> queue;
        std::vector<std::uint8_t> recv_buffer(65507U);
        const auto start = Clock::now();
        auto next_stats = start + std::chrono::milliseconds(options.stats_ms);
        auto next_send_time = start;

        while (true) {
            const auto now = Clock::now();

            while (!queue.empty()) {
                const auto dispatch_time = std::max(queue.front().ready_time, next_send_time);
                if (dispatch_time > now) {
                    break;
                }

                const auto& packet = queue.front();
                const auto sent = ::sendto(
                    out_socket.get(), packet.bytes.data(), packet.bytes.size(), 0,
                    reinterpret_cast<const sockaddr*>(&destination.address),
                    destination.address_len);
                if (sent < 0 || static_cast<std::size_t>(sent) != packet.bytes.size()) {
                    throw std::runtime_error("forward sendto failed");
                }

                ++stats.packets_forwarded;
                stats.bytes_forwarded += static_cast<std::uint64_t>(packet.bytes.size());
                if (options.rate_bps > 0) {
                    const auto packet_bits = static_cast<long double>(packet.bytes.size()) * 8.0L;
                    const auto seconds = packet_bits
                                         / static_cast<long double>(options.rate_bps);
                    const auto delay = std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<long double>(seconds));
                    next_send_time = dispatch_time + delay;
                } else {
                    next_send_time = dispatch_time;
                }
                queue.pop_front();
            }

            if (now >= next_stats) {
                log_stats(stats, queue.size(), options.rate_bps, start);
                next_stats = now + std::chrono::milliseconds(options.stats_ms);
            }

            auto wake_time = next_stats;
            if (!queue.empty()) {
                wake_time = std::min(
                    wake_time, std::max(queue.front().ready_time, next_send_time));
            }

            auto timeout_left = std::chrono::duration_cast<std::chrono::milliseconds>(
                wake_time - now);
            if (timeout_left.count() < 0) {
                timeout_left = std::chrono::milliseconds(0);
            }

            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(in_socket.get(), &read_set);
            timeval tv {};
            tv.tv_sec = static_cast<long>(timeout_left.count() / 1000);
            tv.tv_usec = static_cast<int>((timeout_left.count() % 1000) * 1000);

            const int ready
                = ::select(in_socket.get() + 1, &read_set, nullptr, nullptr, &tv);
            if (ready < 0) {
                throw std::runtime_error("select failed");
            }
            if (ready == 0) {
                continue;
            }

            while (true) {
                const auto received = ::recv(in_socket.get(), recv_buffer.data(),
                                             recv_buffer.size(), 0);
                if (received < 0) {
                    break;
                }

                ++stats.packets_received;
                if (drop_dist(rng) < options.drop) {
                    ++stats.random_drops;
                    continue;
                }
                if (queue.size() >= options.max_queue) {
                    ++stats.queue_drops;
                    continue;
                }

                const auto packet_size = static_cast<std::size_t>(received);
                const auto latency = std::chrono::milliseconds(options.latency_ms);
                const auto jitter = std::chrono::milliseconds(jitter_dist(rng));
                // Store packets until their release time so latency/jitter can be simulated.
                queue.push_back(QueuedPacket {
                    .bytes = std::vector<std::uint8_t>(
                        recv_buffer.begin(),
                        recv_buffer.begin() + static_cast<std::ptrdiff_t>(packet_size)),
                    .ready_time = Clock::now() + latency + jitter,
                });
            }

            std::stable_sort(
                queue.begin(), queue.end(),
                [](const QueuedPacket& lhs, const QueuedPacket& rhs) {
                    return lhs.ready_time < rhs.ready_time;
                });
        }
    } catch (const std::exception& ex) {
        std::cerr << "link_sim error: " << ex.what() << '\n';
        return 1;
    }
}
