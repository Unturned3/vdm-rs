#include "gf256.hpp"
#include "linalg.hpp"
#include "matrix.hpp"
#include <doctest/doctest.h>

inline Matrix<GF256> make_gf_matrix(std::size_t rows, std::size_t cols,
                                    std::initializer_list<std::uint8_t> data)
{
    std::vector<GF256> values;
    values.reserve(data.size());
    for (auto x : data)
        values.emplace_back(x);
    return Matrix<GF256>(rows, cols, std::move(values));
}

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

TEST_CASE("invert identity matrix is no-op")
{
    // clang-format off
    auto m = make_gf_matrix(3, 3, {
        1, 0, 0,
        0, 1, 0,
        0, 0, 1,
    });
    auto m_copy = m;
    // clang-format on
    invert_mat(m);
    CHECK(matrix_eq(m, m_copy));
}

TEST_CASE("matrix times inverse is identity")
{
    // clang-format off
    auto m = make_gf_matrix(3, 3, {
        0, 0, 3,
        1, 0, 4,
        5, 6, 0,
    });
    auto m_copy = m;
    // clang-format on
    auto num_swaps = invert_mat(m);
    auto id = matmul(m_copy, m);
    CHECK(num_swaps == 2);
    CHECK(matrix_eq(id, make_gf_matrix(3, 3, { 1, 0, 0, 0, 1, 0, 0, 0, 1 })));
}

TEST_CASE("3-swap inverse")
{
    // clang-format off
    auto m = make_gf_matrix(4, 4, {
        0, 0, 0, 1,
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
    });
    auto m_copy = m;
    // clang-format on
    auto num_swaps = invert_mat(m);
    auto id = matmul(m_copy, m);
    CHECK(num_swaps == 3);
    CHECK(matrix_eq(
        id, make_gf_matrix(4, 4, { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 })));
}
