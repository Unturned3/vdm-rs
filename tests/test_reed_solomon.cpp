#include "test_support.hpp"
#include "vdm_rs/reed_solomon.hpp"
#include <array>
#include <doctest/doctest.h>
#include <vector>

TEST_CASE("ReedSolomonCodec builds a systematic generator matrix")
{
    const auto codec = ReedSolomonCodec::create(3, 2);

    CHECK(codec.data_shards() == 3);
    CHECK(codec.parity_shards() == 2);
    CHECK(codec.total_shards() == 5);

    const auto& generator = codec.generator_matrix();
    CHECK(generator.rows() == 5);
    CHECK(generator.cols() == 3);

    for (std::size_t row = 0; row < codec.data_shards(); ++row) {
        for (std::size_t col = 0; col < codec.data_shards(); ++col) {
            CHECK(generator(row, col) == (row == col ? GF256::one() : GF256::zero()));
        }
    }
}

TEST_CASE("compute_parity fills parity shards from data shards")
{
    const auto codec = ReedSolomonCodec::create(3, 2);

    std::array<std::uint8_t, 4> d0 { 1, 2, 3, 4 };
    std::array<std::uint8_t, 4> d1 { 5, 6, 7, 8 };
    std::array<std::uint8_t, 4> d2 { 9, 10, 11, 12 };
    std::array<std::uint8_t, 4> p0 { 0, 0, 0, 0 };
    std::array<std::uint8_t, 4> p1 { 0, 0, 0, 0 };

    const std::vector<Shard> data {
        Shard { d0.data(), d0.size() },
        Shard { d1.data(), d1.size() },
        Shard { d2.data(), d2.size() },
    };
    const std::vector<Shard> parity {
        Shard { p0.data(), p0.size() },
        Shard { p1.data(), p1.size() },
    };

    codec.compute_parity(data, parity);

    CHECK_FALSE(
        std::all_of(p0.begin(), p0.end(), [](std::uint8_t x) { return x == 0; }));
    CHECK_FALSE(
        std::all_of(p1.begin(), p1.end(), [](std::uint8_t x) { return x == 0; }));
}

TEST_CASE("reconstruct recovers a missing data shard")
{
    const auto codec = ReedSolomonCodec::create(3, 2);

    std::array<std::uint8_t, 4> d0 { 1, 2, 3, 4 };
    std::array<std::uint8_t, 4> d1 { 5, 6, 7, 8 };
    std::array<std::uint8_t, 4> d2 { 9, 10, 11, 12 };
    std::array<std::uint8_t, 4> p0 { 0, 0, 0, 0 };
    std::array<std::uint8_t, 4> p1 { 0, 0, 0, 0 };

    {
        const std::vector<Shard> data {
            Shard { d0.data(), d0.size() },
            Shard { d1.data(), d1.size() },
            Shard { d2.data(), d2.size() },
        };
        const std::vector<Shard> parity {
            Shard { p0.data(), p0.size() },
            Shard { p1.data(), p1.size() },
        };
        codec.compute_parity(data, parity);
    }

    const auto expected_d1 = d1;

    d1.fill(0);

    const std::vector<Shard> shards {
        Shard { d0.data(), d0.size() }, Shard { d1.data(), d1.size() },
        Shard { d2.data(), d2.size() }, Shard { p0.data(), p0.size() },
        Shard { p1.data(), p1.size() },
    };
    const std::vector<bool> present { true, false, true, true, false };

    CHECK(codec.reconstruct(shards, present));
    CHECK(d1 == expected_d1);
}
