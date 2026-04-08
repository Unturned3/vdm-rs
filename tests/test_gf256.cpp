#include "gf256.hpp"
#include <doctest/doctest.h>

TEST_CASE("GF256 constructor initializes correctly")
{
    GF256 gf256(3);
    CHECK(gf256.value() == 3);
}
