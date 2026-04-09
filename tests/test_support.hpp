#pragma once

#include "gf256.hpp"
#include "matrix.hpp"
#include <cstdint>
#include <initializer_list>
#include <vector>

template <typename T>
bool matrix_eq(const Matrix<T>& a, const Matrix<T>& b)
{
    if (a.rows() != b.rows() || a.cols() != b.cols()) {
        return false;
    }
    for (std::size_t r = 0; r < a.rows(); ++r) {
        for (std::size_t c = 0; c < a.cols(); ++c) {
            if (a(r, c) != b(r, c)) {
                return false;
            }
        }
    }
    return true;
}

inline Matrix<GF256> make_gf_matrix(std::size_t rows, std::size_t cols,
                                    std::initializer_list<std::uint8_t> data)
{
    std::vector<GF256> values;
    values.reserve(data.size());
    for (const auto x : data) {
        values.emplace_back(x);
    }
    return Matrix<GF256>(rows, cols, std::move(values));
}
