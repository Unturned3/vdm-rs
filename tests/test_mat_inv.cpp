#include "gf256.hpp"
#include "linalg.hpp"
#include "matrix.hpp"
#include "test_support.hpp"
#include <doctest/doctest.h>

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
    CHECK(matrix_eq(m, make_identity<GF256>(3)));
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
    CHECK(matrix_eq(id, make_identity<GF256>(3)));
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
    CHECK(matrix_eq(id, make_identity<GF256>(4)));
}
