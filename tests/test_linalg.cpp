#include "linalg.hpp"
#include "matrix.hpp"
#include <doctest/doctest.h>

template <typename T>
bool matrix_eq(const matrix::Matrix<T>& a, const matrix::Matrix<T>& b)
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

bool matrix_eq(const matrix::Matrix<double>& a, const matrix::Matrix<double>& b,
               double eps = 1e-9)
{
    if (a.rows() != b.rows() || a.cols() != b.cols()) {
        return false;
    }
    for (std::size_t r = 0; r < a.rows(); ++r) {
        for (std::size_t c = 0; c < a.cols(); ++c) {
            if (std::abs(a(r, c) - b(r, c)) > eps) {
                return false;
            }
        }
    }
    return true;
}

TEST_CASE("matrix::matmul multiplies a 2x3 and 3x2 matrix")
{
    // clang-format off
    matrix::Matrix<int> a(2, 3, {
        1, 2, 3,
        4, 5, 6,
    });
    matrix::Matrix<int> b(3, 2, {
        7, 8,
        9, 10,
        11, 12,
    });

    const auto expected = matrix::Matrix<int>(2, 2, {
        58, 64,
        139, 154,
    });
    // clang-format on

    const auto c = matrix::matmul(a, b);

    CHECK(c.rows() == 2);
    CHECK(c.cols() == 2);
    CHECK(matrix_eq(c, expected));
}

TEST_CASE("float matrix multiplication is approximately correct")
{
    // clang-format off
    matrix::Matrix<double> a(2, 2, {
        3.2,  4.3,
       -0.9,  0.8,
    });
    matrix::Matrix<double> b(2, 2, {
        5.2, 1.9,
        3.0, 7.0,
    });

    const auto expected = matrix::Matrix<double>(2, 2, {
        29.54, 36.18,
        -2.28,  3.89,
    });
    // clang-format on

    const auto c = matrix::matmul(a, b);

    CHECK(c.rows() == 2);
    CHECK(c.cols() == 2);
    CHECK(matrix_eq(c, expected));
}

TEST_CASE("matrix::matmul writes into an output matrix")
{
    // clang-format off
    matrix::Matrix<int> a(2, 2, {
        1, 2,
        3, 4,
    });
    matrix::Matrix<int> b(2, 2, {
        5, 6,
        7, 8,
    });
    // clang-format on
    matrix::Matrix<int> c(2, 2, 0);

    matrix::matmul(a, b, c);

    CHECK(c(0, 0) == 19);
    CHECK(c(0, 1) == 22);
    CHECK(c(1, 0) == 43);
    CHECK(c(1, 1) == 50);
}

TEST_CASE("intentional failure example" * doctest::should_fail(true))
{
    CHECK(2 + 2 == 5);
}
