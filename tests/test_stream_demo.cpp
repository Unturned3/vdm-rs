#include "vdm_rs/stream_demo.hpp"
#include <array>
#include <doctest/doctest.h>
#include <span>

TEST_CASE("packet header CRC validates serialized metadata")
{
    auto header = stream_demo::PacketHeader {
        .magic = stream_demo::packet_magic,
        .version = stream_demo::packet_version,
        .header_size = static_cast<std::uint16_t>(stream_demo::packet_header_size),
        .stream_id = 7,
        .flags = static_cast<std::uint16_t>(stream_demo::packet_flag_final_stripe),
        .stripe_id = 42,
        .tx_time_ns = 123456789U,
        .shard_index = 3,
        .k = 8,
        .t = 4,
        .shard_payload_size = 1400,
        .header_crc32 = 0,
    };
    header.header_crc32 = stream_demo::compute_packet_header_crc32(header);

    CHECK(stream_demo::packet_header_crc_valid(header));

    header.stripe_id += 1;
    CHECK_FALSE(stream_demo::packet_header_crc_valid(header));
}

TEST_CASE("CRC32 matches a stable reference value")
{
    const std::array<std::uint8_t, 5> bytes { 'h', 'e', 'l', 'l', 'o' };
    CHECK(stream_demo::compute_crc32(std::span<const std::uint8_t>(bytes))
          == 0x3610A686U);
}
