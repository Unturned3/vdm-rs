#include "linalg.hpp"
#include "matrix.hpp"
#include <doctest/doctest.h>

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
