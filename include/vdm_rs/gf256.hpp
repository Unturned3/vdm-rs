#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <format>
#include <ostream>

/*
A lightweight class representing elements of the finite field GF(2^8).
 */
class GF256
{
public:
    using storage_type = std::uint8_t; // Underlying storage type for field elements

    constexpr GF256() = default; // Default constructor initializes to zero

    constexpr explicit GF256(storage_type v) // Explicit constructor from byte value
        : value_(v)
    {
    }

    // Returns the shared multiplication lookup table for GF(2^8).
    static const auto& mul_table() // Class-wide multiplication lookup table
    {
        // Initialized on first use
        static const auto table = make_mul_table();
        return table;
    }

    // Returns the shared inverse lookup table for non-zero field elements.
    static const auto& inv_table() // Class-wide multiplicative inverse lookup table
    {
        // Initialized on first use
        static const auto table = make_inv_table();
        return table;
    }

    // Returns the additive identity.
    [[nodiscard]] static constexpr GF256 zero() { return GF256 { 0 }; }

    // Returns the multiplicative identity.
    [[nodiscard]] static constexpr GF256 one() { return GF256 { 1 }; }

    // Checks whether this element is zero.
    [[nodiscard]] constexpr bool is_zero() const { return value_ == 0; }

    // Exposes the underlying byte representation.
    [[nodiscard]] constexpr storage_type value() const { return value_; }

    friend constexpr bool operator==(GF256, GF256) = default;

    // Additions and subtractions are the same in GF256, implemented as XOR.
    friend constexpr GF256 operator+(GF256 a, GF256 b)
    {
        return GF256 { static_cast<storage_type>(a.value_ ^ b.value_) };
    }

    friend constexpr GF256 operator-(GF256 a, GF256 b)
    {
        return GF256 { static_cast<storage_type>(a.value_ ^ b.value_) };
    }

    // Multiplies two field elements using the precomputed table.
    friend GF256 operator*(GF256 a, GF256 b)
    {
        return GF256 { mul_table()[a.value_][b.value_] };
    }

    // Divides by multiplying with the multiplicative inverse of b.
    friend GF256 operator/(GF256 a, GF256 b)
    {
        assert(!b.is_zero());
        return a * b.inverse();
    }

    [[nodiscard]] constexpr GF256 operator-() const
    {
        // In GF256, additive inverse is self.
        return *this;
    }

    // Returns the multiplicative inverse; zero is rejected with an assert.
    [[nodiscard]] GF256 inverse() const
    {
        assert(!is_zero());
        return GF256 { inv_table()[value_] };
    }

    // Accumulates rhs using field addition.
    GF256& operator+=(GF256 rhs)
    {
        value_ ^= rhs.value_;
        return *this;
    }

    // Subtracts rhs, which is identical to addition in GF(2^8).
    GF256& operator-=(GF256 rhs)
    {
        value_ ^= rhs.value_;
        return *this;
    }

    // Multiplies this element in place.
    GF256& operator*=(GF256 rhs)
    {
        value_ = mul_table()[value_][rhs.value_];
        return *this;
    }

    // Divides this element in place.
    GF256& operator/=(GF256 rhs)
    {
        assert(!rhs.is_zero());
        *this *= rhs.inverse();
        return *this;
    }

private:
    storage_type value_ = 0;

    // Rjindael (AES) irreducible polynomial: x^8 + x^4 + x^3 + x + 1
    static constexpr storage_type irreducible_polynomial_low_ = 0x1B;

    // Multiplies two polynomial representations and reduces modulo the field polynomial.
    static storage_type poly_mul(storage_type a, storage_type b)
    {
        storage_type result = 0;
        for (int i = 0; i < 8; ++i) {
            if (b & 1) {
                result ^= a;
            }

            bool carry = (a & 0x80) != 0;
            a <<= 1;
            if (carry) {
                a ^= irreducible_polynomial_low_;
            }
            b >>= 1;
        }
        return result;
    }

    // Builds the full byte-by-byte multiplication table once at startup.
    static std::array<std::array<storage_type, 256>, 256> make_mul_table()
    {
        std::array<std::array<storage_type, 256>, 256> table { };

        for (size_t a = 0; a < 256; ++a) {
            for (size_t b = 0; b < 256; ++b) {
                table[a][b] = poly_mul(static_cast<storage_type>(a),
                                       static_cast<storage_type>(b));
            }
        }
        return table;
    }

    // Builds inverse lookups for all non-zero field elements.
    static std::array<storage_type, 256> make_inv_table()
    {
        std::array<storage_type, 256> table { };
        table[0] = 0; // sentinel; inverse(0) is undefined anyways

        for (size_t a = 1; a < 256; ++a) {
            for (size_t b = 1; b < 256; ++b) {
                if (poly_mul(static_cast<storage_type>(a), static_cast<storage_type>(b))
                    == 1) {
                    table[a] = static_cast<storage_type>(b);
                    break;
                }
            }
        }
        return table;
    }
};

// For printing GF256 values

// Prints a field element as its integer byte value.
inline std::ostream& operator<<(std::ostream& os, const GF256& x)
{
    return os << static_cast<int>(x.value());
}

template <>
struct std::formatter<GF256> : std::formatter<int>
{
    // Reuses integer formatting so GF256 works with std::format.
    auto format(const GF256& x, auto& ctx) const
    {
        return std::formatter<int>::format(x.value(), ctx);
    }
};
