#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace stream_demo {

constexpr std::uint32_t packet_magic = 0x56444D52U; // "VDMR"
constexpr std::uint16_t packet_version = 1;
constexpr std::uint16_t packet_flag_final_stripe = 0x0001U;
constexpr std::uint16_t data_flag_final_stripe = 0x0001U;

struct PacketHeader
{
    std::uint32_t magic = packet_magic;
    std::uint16_t version = packet_version;
    std::uint16_t header_size = 0;
    std::uint16_t stream_id = 0;
    std::uint16_t flags = 0;
    std::uint64_t stripe_id = 0;
    std::uint64_t tx_time_ns = 0;
    std::uint16_t shard_index = 0;
    std::uint16_t k = 0;
    std::uint16_t t = 0;
    std::uint16_t shard_payload_size = 0;
    std::uint32_t header_crc32 = 0;
};

struct DataShardHeader
{
    std::uint32_t data_length = 0;
    std::uint16_t flags = 0;
    std::uint32_t payload_crc32 = 0;
};

constexpr std::size_t packet_header_size
    = sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(std::uint16_t)
      + sizeof(std::uint16_t) + sizeof(std::uint16_t) + sizeof(std::uint64_t)
      + sizeof(std::uint64_t) + sizeof(std::uint16_t) + sizeof(std::uint16_t)
      + sizeof(std::uint16_t) + sizeof(std::uint16_t) + sizeof(std::uint32_t);

constexpr std::size_t data_shard_header_size
    = sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(std::uint32_t);

// Appends a 16-bit integer in network byte order.
inline void append_u16_be(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

// Appends a 32-bit integer in network byte order.
inline void append_u32_be(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

// Appends a 64-bit integer in network byte order.
inline void append_u64_be(std::vector<std::uint8_t>& out, std::uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(
            static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & 0xFFU));
    }
}

// Reads a big-endian 16-bit integer starting at offset.
[[nodiscard]] inline auto read_u16_be(std::span<const std::uint8_t> bytes,
                                      std::size_t offset) -> std::uint16_t
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8U)
        | static_cast<std::uint16_t>(bytes[offset + 1]));
}

// Reads a big-endian 32-bit integer starting at offset.
[[nodiscard]] inline auto read_u32_be(std::span<const std::uint8_t> bytes,
                                      std::size_t offset) -> std::uint32_t
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U)
           | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U)
           | (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U)
           | static_cast<std::uint32_t>(bytes[offset + 3]);
}

// Reads a big-endian 64-bit integer starting at offset.
[[nodiscard]] inline auto read_u64_be(std::span<const std::uint8_t> bytes,
                                      std::size_t offset) -> std::uint64_t
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < sizeof(std::uint64_t); ++i) {
        value = (value << 8U) | static_cast<std::uint64_t>(bytes[offset + i]);
    }
    return value;
}

// Serializes the fixed packet header into the on-the-wire byte layout.
[[nodiscard]] inline auto serialize_packet_header(const PacketHeader& header)
    -> std::vector<std::uint8_t>
{
    std::vector<std::uint8_t> out;
    out.reserve(packet_header_size);
    append_u32_be(out, header.magic);
    append_u16_be(out, header.version);
    append_u16_be(out, header.header_size);
    append_u16_be(out, header.stream_id);
    append_u16_be(out, header.flags);
    append_u64_be(out, header.stripe_id);
    append_u64_be(out, header.tx_time_ns);
    append_u16_be(out, header.shard_index);
    append_u16_be(out, header.k);
    append_u16_be(out, header.t);
    append_u16_be(out, header.shard_payload_size);
    append_u32_be(out, header.header_crc32);
    return out;
}

// Writes the protected per-shard header into the start of a shard buffer.
inline void encode_data_shard_header(const DataShardHeader& header,
                                     std::span<std::uint8_t> out)
{
    if (out.size() < data_shard_header_size) {
        throw std::invalid_argument("data shard buffer too small for header");
    }

    out[0] = static_cast<std::uint8_t>((header.data_length >> 24U) & 0xFFU);
    out[1] = static_cast<std::uint8_t>((header.data_length >> 16U) & 0xFFU);
    out[2] = static_cast<std::uint8_t>((header.data_length >> 8U) & 0xFFU);
    out[3] = static_cast<std::uint8_t>(header.data_length & 0xFFU);
    out[4] = static_cast<std::uint8_t>((header.flags >> 8U) & 0xFFU);
    out[5] = static_cast<std::uint8_t>(header.flags & 0xFFU);
    out[6] = static_cast<std::uint8_t>((header.payload_crc32 >> 24U) & 0xFFU);
    out[7] = static_cast<std::uint8_t>((header.payload_crc32 >> 16U) & 0xFFU);
    out[8] = static_cast<std::uint8_t>((header.payload_crc32 >> 8U) & 0xFFU);
    out[9] = static_cast<std::uint8_t>(header.payload_crc32 & 0xFFU);
}

// Parses a packet header if enough bytes are available.
[[nodiscard]] inline auto parse_packet_header(std::span<const std::uint8_t> bytes)
    -> std::optional<PacketHeader>
{
    if (bytes.size() < packet_header_size) {
        return std::nullopt;
    }

    PacketHeader header;
    std::size_t offset = 0;
    header.magic = read_u32_be(bytes, offset);
    offset += sizeof(std::uint32_t);
    header.version = read_u16_be(bytes, offset);
    offset += sizeof(std::uint16_t);
    header.header_size = read_u16_be(bytes, offset);
    offset += sizeof(std::uint16_t);
    header.stream_id = read_u16_be(bytes, offset);
    offset += sizeof(std::uint16_t);
    header.flags = read_u16_be(bytes, offset);
    offset += sizeof(std::uint16_t);
    header.stripe_id = read_u64_be(bytes, offset);
    offset += sizeof(std::uint64_t);
    header.tx_time_ns = read_u64_be(bytes, offset);
    offset += sizeof(std::uint64_t);
    header.shard_index = read_u16_be(bytes, offset);
    offset += sizeof(std::uint16_t);
    header.k = read_u16_be(bytes, offset);
    offset += sizeof(std::uint16_t);
    header.t = read_u16_be(bytes, offset);
    offset += sizeof(std::uint16_t);
    header.shard_payload_size = read_u16_be(bytes, offset);
    offset += sizeof(std::uint16_t);
    header.header_crc32 = read_u32_be(bytes, offset);
    return header;
}

// Parses the per-shard metadata stored inside data shards.
[[nodiscard]] inline auto parse_data_shard_header(
    std::span<const std::uint8_t> bytes) -> std::optional<DataShardHeader>
{
    if (bytes.size() < data_shard_header_size) {
        return std::nullopt;
    }

    DataShardHeader header;
    header.data_length = read_u32_be(bytes, 0);
    header.flags = read_u16_be(bytes, 4);
    header.payload_crc32 = read_u32_be(bytes, 6);
    return header;
}

// Interleaves data and parity indices so parity is spread throughout transmission.
[[nodiscard]] inline auto make_shard_send_order(std::size_t k, std::size_t t)
    -> std::vector<std::size_t>
{
    std::vector<std::size_t> order;
    order.reserve(k + t);
    const std::size_t shared = std::min(k, t);
    for (std::size_t i = 0; i < shared; ++i) {
        order.push_back(i);
        order.push_back(k + i);
    }
    for (std::size_t i = shared; i < k; ++i) {
        order.push_back(i);
    }
    for (std::size_t i = shared; i < t; ++i) {
        order.push_back(k + i);
    }
    return order;
}

// Returns a monotonic timestamp used for latency measurement.
[[nodiscard]] inline auto steady_time_ns() -> std::uint64_t
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

// Formats a byte rate using binary throughput units.
[[nodiscard]] inline auto describe_rate_bps(double bytes_per_second) -> std::string
{
    if (bytes_per_second <= 0.0) {
        return "0.00 B/s";
    }

    static constexpr std::array<std::string_view, 4> units {
        "B/s",
        "KiB/s",
        "MiB/s",
        "GiB/s",
    };

    std::size_t unit_index = 0;
    while (bytes_per_second >= 1024.0 && unit_index + 1 < units.size()) {
        bytes_per_second /= 1024.0;
        ++unit_index;
    }

    std::string result;
    result.reserve(32);
    const auto whole = static_cast<std::uint64_t>(bytes_per_second);
    const auto frac = static_cast<std::uint64_t>(
        (bytes_per_second - static_cast<double>(whole)) * 100.0);
    result += std::to_string(whole);
    result += '.';
    if (frac < 10U) {
        result += '0';
    }
    result += std::to_string(frac);
    result += ' ';
    result += units[unit_index];
    return result;
}

} // namespace stream_demo
