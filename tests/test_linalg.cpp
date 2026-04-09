#include "test_support.hpp"
#include "vdm_rs/linalg.hpp"
#include "vdm_rs/matrix.hpp"
#include <doctest/doctest.h>

TEST_CASE("matmul multiplies a 2x3 and 3x2 matrix")
{
    // clang-format off
    Matrix<int> a(2, 3, {
        1, 2, 3,
        4, 5, 6,
    });
    Matrix<int> b(3, 2, {
        7, 8,
        9, 10,
        11, 12,
    });

    const auto expected = Matrix<int>(2, 2, {
        58, 64,
        139, 154,
    });
    // clang-format on

    const auto c = matmul(a, b);

    CHECK(c.rows() == 2);
    CHECK(c.cols() == 2);
    CHECK(matrix_eq(c, expected));
}

TEST_CASE("matmul into output matrix overwrites previous contents")
{
    // clang-format off
    const Matrix<int> a(2, 2, {
        1, 2,
        3, 4,
    });
    const Matrix<int> b(2, 2, {
        5, 6,
        7, 8,
    });
    Matrix<int> c(2, 2, 99);

    const auto expected = Matrix<int>(2, 2, {
        19, 22,
        43, 50,
    });
    // clang-format on

    matmul(a, b, c);

    CHECK(matrix_eq(c, expected));
}
