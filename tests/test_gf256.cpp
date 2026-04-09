#include "vdm_rs/gf256.hpp"
#include <doctest/doctest.h>

TEST_CASE("GF256 multiplication")
{
    GF256 a(35), b(36);
    GF256 result = a * b;
    CHECK(result == GF256(128));
}
