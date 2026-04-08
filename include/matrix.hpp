#pragma once

#include <cassert>
#include <iomanip>
#include <print>
#include <sstream>
#include <string>
#include <vector>

namespace matrix
{

template <class T>
class Matrix
{
public:
    // Initialize with default value
    Matrix(std::size_t rows, std::size_t cols, T init = T { })
        : rows_(rows)
        , cols_(cols)
        , data_(rows * cols, init)
    {
    }

    // Initialize from a vector of data
    explicit Matrix(std::size_t rows, std::size_t cols, std::vector<T> data)
        : rows_(rows)
        , cols_(cols)
        , data_(std::move(data))
    {
        assert(data_.size() == rows_ * cols_);
    }

    // Initialize from an initializer list
    Matrix(std::size_t rows, std::size_t cols, std::initializer_list<T> data)
        : rows_(rows)
        , cols_(cols)
        , data_(data)
    {
        assert(data_.size() == rows_ * cols_);
    }

    T& operator()(std::size_t r, std::size_t c) { return data_[r * cols_ + c]; }

    const T& operator()(std::size_t r, std::size_t c) const
    {
        return data_[r * cols_ + c];
    }

    [[nodiscard]] std::size_t rows() const { return rows_; }

    [[nodiscard]] std::size_t cols() const { return cols_; }

    T* data() { return data_.data(); }

    const T* data() const { return data_.data(); }

private:
    std::size_t rows_;
    std::size_t cols_;
    std::vector<T> data_ { };
};

template <class T>
Matrix<T> matmul(Matrix<T>& a, Matrix<T>& b)
{
    Matrix<T> c(a.rows(), b.cols(), T { });
    for (std::size_t i = 0; i < a.rows(); i++) {
        for (std::size_t k = 0; k < a.cols(); k++) {
            T aik = a(i, k);
            for (std::size_t j = 0; j < b.cols(); j++) {
                c(i, j) += aik * b(k, j);
            }
        }
    }
    return c;
}

template <class T>
void matmul(Matrix<T>& a, Matrix<T>& b, Matrix<T>& c)
{
    for (std::size_t i = 0; i < a.rows(); i++) {
        for (std::size_t k = 0; k < a.cols(); k++) {
            T aik = a(i, k);
            for (std::size_t j = 0; j < b.cols(); j++) {
                c(i, j) += aik * b(k, j);
            }
        }
    }
}

template <class T>
std::string to_string(const Matrix<T>& m, int width = 4)
{
    std::ostringstream oss;

    for (std::size_t i = 0; i < m.rows(); i++) {
        for (std::size_t j = 0; j < m.cols(); j++) {
            oss << std::setw(width) << m(i, j) << " ";
        }
        oss << '\n';
    }

    return oss.str();
}

} // namespace matrix
