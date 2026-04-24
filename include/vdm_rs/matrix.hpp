#pragma once

#include <array>
#include <cassert>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

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

    // Returns mutable access to the element at row r, column c.
    T& operator()(std::size_t r, std::size_t c)
    {
        assert(r < rows_);
        assert(c < cols_);
        return data_[r * cols_ + c];
    }

    // Returns read-only access to the element at row r, column c.
    const T& operator()(std::size_t r, std::size_t c) const
    {
        assert(r < rows_);
        assert(c < cols_);
        return data_[r * cols_ + c];
    }

    // Returns the number of rows.
    [[nodiscard]] std::size_t rows() const { return rows_; }

    // Returns the number of columns.
    [[nodiscard]] std::size_t cols() const { return cols_; }

    // Returns a pointer to the contiguous backing storage.
    T* data() { return data_.data(); }

    // Returns a read-only pointer to the contiguous backing storage.
    const T* data() const { return data_.data(); }

private:
    std::size_t rows_;
    std::size_t cols_;
    std::vector<T> data_ { };
};

template <class T>
// Creates an n x n identity matrix.
Matrix<T> make_identity(std::size_t n)
{
    Matrix<T> identity(n, n, T { });
    for (std::size_t i = 0; i < n; ++i) {
        identity(i, i) = T { 1 };
    }
    return identity;
}

template <class T>
// Formats the matrix as aligned rows for debugging output.
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
