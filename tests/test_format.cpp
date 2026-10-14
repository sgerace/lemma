#include "lemma/format.h"

#include <catch2/catch_test_macros.hpp>

using namespace lemma;

TEST_CASE("Numbers show up to three decimals without trailing zeros")
{
    CHECK(formatNumber(0) == "0");
    CHECK(formatNumber(20) == "20");
    CHECK(formatNumber(2.5) == "2.5");
    CHECK(formatNumber(3.14159265) == "3.142");
    CHECK(formatNumber(-0.0001234) == "-1.234×10⁻⁴");
    CHECK(formatNumber(-1e-12) == "-1×10⁻¹²");
}

TEST_CASE("Large and small numbers switch to powers of ten")
{
    CHECK(formatNumber(999) == "999");
    CHECK(formatNumber(1500) == "1.5×10³");
    CHECK(formatNumber(0.002) == "0.002");
    CHECK(formatNumber(0.0002) == "2×10⁻⁴");
    CHECK(formatNumber(99999.9) == "1×10⁵"); // mantissa rounding carries into the exponent
}

TEST_CASE("The exponential threshold is configurable")
{
    FormatOptions options;
    options.exponentialThreshold = 6;
    CHECK(formatNumber(1500, options) == "1500");
}

TEST_CASE("Quantities use a named unit when one fits")
{
    CHECK(formatQuantity({20.0, Dimension::of(2, 1, -2)}) == "20 J");
    CHECK(formatQuantity({9.80665, Dimension::of(1, 0, -2)}) == "9.807 m/s²");
    CHECK(formatQuantity(Quantity::scalar(2)) == "2");
}
