#pragma once

#include "vdm_rs/gf256.hpp"
#include "vdm_rs/matrix.hpp"
#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

template <class T>
Matrix<T> matmul(const Matrix<T>& a, const Matrix<T>& b)
{
    assert(a.cols() == b.rows());
    Matrix<T> c(a.rows(), b.cols(), T { });
    for (std::size_t i = 0; i < a.rows(); i++) {
        for (std::size_t k = 0; k < a.cols(); k++) {
            const T aik = a(i, k);
            for (std::size_t j = 0; j < b.cols(); j++) {
                c(i, j) += aik * b(k, j);
            }
        }
    }
    return c;
}

// TODO: Add a variant that does not zero the output matrix before multiplication?
template <class T>
void matmul(const Matrix<T>& a, const Matrix<T>& b, Matrix<T>& c)
{
    assert(a.cols() == b.rows());
    assert(c.rows() == a.rows());
    assert(c.cols() == b.cols());

    for (std::size_t i = 0; i < c.rows(); ++i) {
        for (std::size_t j = 0; j < c.cols(); ++j) {
            c(i, j) = T { };
        }
    }

    for (std::size_t i = 0; i < a.rows(); i++) {
        for (std::size_t k = 0; k < a.cols(); k++) {
            const T aik = a(i, k);
            for (std::size_t j = 0; j < b.cols(); j++) {
                c(i, j) += aik * b(k, j);
            }
        }
    }
}

// Inverts matrix in-place.
// Returns -1 if the matrix is singular. Otherwise, returns the number of row swaps
// performed.
inline int invert_mat(Matrix<GF256>& m)
{
    assert(m.rows() == m.cols());
    const std::size_t n = m.rows();
    assert(n <= 256);

    std::array<std::size_t, 256> ar { };
    for (std::size_t i = 0; i < n; i++)
        ar[i] = i;

    for (std::size_t i = 0; i < n; i++) {
        std::size_t j = 0;

        for (j = i; j < n; j++)
            if (!m(j, i).is_zero())
                break; // Row j has a valid pivot at the i-th column

        if (j >= n) {
            return -1; // No valid pivot found
        }
        if (i != j) { // Perform row swap
            std::swap(ar[i], ar[j]);
            for (std::size_t k = 0; k < n; k++)
                std::swap(m(i, k), m(j, k));
        }
        const GF256 inv_piv = m(i, i).inverse();
        for (std::size_t k = 0; k < n; k++)
            m(i, k) *= inv_piv;
        m(i, i) = inv_piv;

        for (j = 0; j < n; j++) {
            if (j == i)
                continue;
            const GF256 scale = -m(j, i);
            m(j, i) = GF256 { 0 };
            for (std::size_t k = 0; k < n; k++)
                m(j, k) += scale * m(i, k);
        }
    }

    int swapCnt = 0;
    for (std::size_t i = 0; i < n; i++) {
        while (ar[i] != i) {
            swapCnt++;
            for (std::size_t j = 0; j < n; j++) // Swap columns i, ar[i]
                std::swap(m(j, ar[i]), m(j, i));
            std::swap(ar[ar[i]], ar[i]);
        }
    }

    return swapCnt;
}
