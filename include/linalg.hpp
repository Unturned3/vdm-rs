#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

template <class T>
concept FieldLike = requires(T a, T b) {
    { T::zero() } -> std::same_as<T>;
    { T::one() } -> std::same_as<T>;
    { a.is_zero() } -> std::convertible_to<bool>;
    { a + b } -> std::same_as<T>;
    { a - b } -> std::same_as<T>;
    { a * b } -> std::same_as<T>;
    { a / b } -> std::same_as<T>;
    { -a } -> std::same_as<T>;
    { a == b } -> std::convertible_to<bool>;
};
