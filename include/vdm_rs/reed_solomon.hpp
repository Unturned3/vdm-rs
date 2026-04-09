#pragma once

#include "vdm_rs/gf256.hpp"
#include "vdm_rs/linalg.hpp"
#include "vdm_rs/matrix.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

struct Shard
{
    std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

class ReedSolomonCodec
{
public:
    [[nodiscard]] static ReedSolomonCodec create(std::size_t k, std::size_t t)
    {
        validate_params_(k, t);
        return ReedSolomonCodec(k, t);
    }

    [[nodiscard]] std::size_t data_shards() const noexcept { return k_; }

    [[nodiscard]] std::size_t parity_shards() const noexcept { return t_; }

    [[nodiscard]] std::size_t total_shards() const noexcept { return n_; }

    [[nodiscard]] const Matrix<GF256>& generator_matrix() const noexcept
    {
        return generator_matrix_;
    }

    [[nodiscard]] const Matrix<GF256>& parity_matrix() const noexcept
    {
        return parity_matrix_;
    }

    void compute_parity(const std::vector<Shard>& data_shards,
                        const std::vector<Shard>& parity_shards) const
    {
        if (data_shards.size() != k_) {
            throw std::invalid_argument("expected exactly k data shards");
        }
        if (parity_shards.size() != t_) {
            throw std::invalid_argument("expected exactly t parity shards");
        }

        const std::size_t shard_size
            = validate_equal_sizes_(data_shards, parity_shards);

        for (const auto& shard : parity_shards) {
            std::fill_n(shard.data, shard.size, std::uint8_t { 0 });
        }

        for (std::size_t parity_row = 0; parity_row < t_; ++parity_row) {
            for (std::size_t data_col = 0; data_col < k_; ++data_col) {
                const GF256 coeff = parity_matrix_(parity_row, data_col);
                if (coeff.is_zero()) {
                    continue;
                }
                addmul_shard_(parity_shards[parity_row].data,
                              data_shards[data_col].data, coeff, shard_size);
            }
        }
    }

    [[nodiscard]] bool reconstruct(const std::vector<Shard>& shards,
                                   const std::vector<bool>& present) const
    {
        if (shards.size() != n_) {
            throw std::invalid_argument("expected exactly n shards");
        }
        if (present.size() != n_) {
            throw std::invalid_argument("expected exactly n presence flags");
        }

        std::size_t present_count = 0;
        for (const bool bit : present) {
            present_count += bit ? 1U : 0U;
        }
        if (present_count < k_) {
            return false;
        }

        const std::size_t shard_size = validate_present_sizes_(shards, present);

        std::vector<std::size_t> selected;
        selected.reserve(k_);
        for (std::size_t i = 0; i < n_ && selected.size() < k_; ++i) {
            if (present[i]) {
                selected.push_back(i);
            }
        }

        Matrix<GF256> decode_matrix(k_, k_, GF256::zero());
        for (std::size_t row = 0; row < k_; ++row) {
            for (std::size_t col = 0; col < k_; ++col) {
                decode_matrix(row, col) = generator_matrix_(selected[row], col);
            }
        }

        if (invert_mat(decode_matrix) < 0) {
            return false;
        }

        std::vector<std::vector<std::uint8_t>> recovered_data(
            k_, std::vector<std::uint8_t>(shard_size, std::uint8_t { 0 }));

        for (std::size_t out_row = 0; out_row < k_; ++out_row) {
            for (std::size_t src_col = 0; src_col < k_; ++src_col) {
                const GF256 coeff = decode_matrix(out_row, src_col);
                if (coeff.is_zero()) {
                    continue;
                }
                addmul_shard_(recovered_data[out_row].data(),
                              shards[selected[src_col]].data, coeff, shard_size);
            }
        }

        for (std::size_t i = 0; i < k_; ++i) {
            if (shards[i].data == nullptr || shards[i].size != shard_size) {
                throw std::invalid_argument(
                    "all data shard buffers must be allocated to reconstruct");
            }
            std::copy(recovered_data[i].begin(), recovered_data[i].end(),
                      shards[i].data);
        }

        std::vector<Shard> data_views;
        data_views.reserve(k_);
        for (std::size_t i = 0; i < k_; ++i) {
            data_views.push_back(shards[i]);
        }

        std::vector<Shard> parity_views;
        parity_views.reserve(t_);
        for (std::size_t i = 0; i < t_; ++i) {
            if (shards[k_ + i].data == nullptr || shards[k_ + i].size != shard_size) {
                throw std::invalid_argument(
                    "all parity shard buffers must be allocated to reconstruct");
            }
            parity_views.push_back(shards[k_ + i]);
        }

        compute_parity(data_views, parity_views);
        return true;
    }

private:
    ReedSolomonCodec(std::size_t k, std::size_t t)
        : k_(k)
        , t_(t)
        , n_(k + t)
        , generator_matrix_(build_generator_matrix_(k_, n_))
        , parity_matrix_(build_parity_matrix_(generator_matrix_, k_, t_))
    {
    }

    std::size_t k_;
    std::size_t t_;
    std::size_t n_;
    Matrix<GF256> generator_matrix_;
    Matrix<GF256> parity_matrix_;

    static void validate_params_(std::size_t k, std::size_t t)
    {
        if (k == 0) {
            throw std::invalid_argument("k must be positive");
        }
        if (t == 0) {
            throw std::invalid_argument("t must be positive");
        }
        const std::size_t n = k + t;
        if (n > 256) {
            throw std::invalid_argument("GF256 only supports up to 256 total shards");
        }
    }

    [[nodiscard]] static GF256 pow_(GF256 base, std::size_t exponent)
    {
        GF256 result = GF256::one();
        for (std::size_t i = 0; i < exponent; ++i) {
            result *= base;
        }
        return result;
    }

    [[nodiscard]] static Matrix<GF256> build_generator_matrix_(std::size_t k,
                                                               std::size_t n)
    {
        Matrix<GF256> vandermonde(n, k, GF256::zero());
        for (std::size_t row = 0; row < n; ++row) {
            const GF256 x { static_cast<GF256::storage_type>(row) };
            for (std::size_t col = 0; col < k; ++col) {
                vandermonde(row, col) = pow_(x, col);
            }
        }

        Matrix<GF256> top(k, k, GF256::zero());
        for (std::size_t row = 0; row < k; ++row) {
            for (std::size_t col = 0; col < k; ++col) {
                top(row, col) = vandermonde(row, col);
            }
        }

        if (invert_mat(top) < 0) {
            throw std::runtime_error("failed to invert Vandermonde top matrix");
        }

        return matmul(vandermonde, top);
    }

    [[nodiscard]] static Matrix<GF256>
    build_parity_matrix_(const Matrix<GF256>& generator_matrix, std::size_t k,
                         std::size_t t)
    {
        Matrix<GF256> parity(t, k, GF256::zero());
        for (std::size_t row = 0; row < t; ++row) {
            for (std::size_t col = 0; col < k; ++col) {
                parity(row, col) = generator_matrix(k + row, col);
            }
        }
        return parity;
    }

    [[nodiscard]] static std::size_t
    validate_equal_sizes_(const std::vector<Shard>& data_shards,
                          const std::vector<Shard>& parity_shards)
    {
        if (data_shards.empty()) {
            throw std::invalid_argument("at least one data shard is required");
        }

        const std::size_t shard_size = validate_shard_(data_shards.front());
        for (const auto& shard : data_shards) {
            validate_shard_size_(shard, shard_size);
        }
        for (const auto& shard : parity_shards) {
            validate_shard_size_(shard, shard_size);
        }
        return shard_size;
    }

    [[nodiscard]] static std::size_t
    validate_present_sizes_(const std::vector<Shard>& shards,
                            const std::vector<bool>& present)
    {
        const auto it = std::find(present.begin(), present.end(), true);
        if (it == present.end()) {
            throw std::invalid_argument("at least one shard must be present");
        }

        const auto first_present
            = static_cast<std::size_t>(std::distance(present.begin(), it));
        const std::size_t shard_size = validate_shard_(shards[first_present]);

        for (std::size_t i = 0; i < shards.size(); ++i) {
            if (present[i]) {
                validate_shard_size_(shards[i], shard_size);
            }
        }

        return shard_size;
    }

    [[nodiscard]] static std::size_t validate_shard_(const Shard& shard)
    {
        if (shard.data == nullptr) {
            throw std::invalid_argument("shard data pointer must not be null");
        }
        return shard.size;
    }

    static void validate_shard_size_(const Shard& shard, std::size_t expected_size)
    {
        if (shard.data == nullptr) {
            throw std::invalid_argument("shard data pointer must not be null");
        }
        if (shard.size != expected_size) {
            throw std::invalid_argument("all shards must have the same size");
        }
    }

    static void addmul_shard_(std::uint8_t* dst, const std::uint8_t* src, GF256 coeff,
                              std::size_t shard_size)
    {
        for (std::size_t i = 0; i < shard_size; ++i) {
            dst[i] ^= (coeff * GF256 { src[i] }).value();
        }
    }
};
