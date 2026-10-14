#include "lemma/dimension.h"
#include "lemma/rational.h"

#include <catch2/catch_test_macros.hpp>

using namespace lemma;

TEST_CASE("Rational arithmetic normalizes")
{
    CHECK(Rational(2, 4) == Rational(1, 2));
    CHECK(Rational(1, -2) == Rational(-1, 2));
    CHECK(Rational(1, 2) + Rational(1, 3) == Rational(5, 6));
    CHECK(Rational(3, 4) * Rational(2, 3) == Rational(1, 2));
    CHECK(Rational(1, 2).toString() == "1/2");
    CHECK(Rational(-3).toString() == "-3");
}

TEST_CASE("Rational recovers simple fractions from doubles")
{
    CHECK(Rational::fromDouble(0.5) == Rational(1, 2));
    CHECK(Rational::fromDouble(-2.0) == Rational(-2));
    CHECK(Rational::fromDouble(1.0 / 3.0) == Rational(1, 3));
    CHECK_FALSE(Rational::fromDouble(3.14159265358979).has_value());
}

TEST_CASE("Dimensions combine by adding exponents")
{
    const Dimension length = Dimension::of(1);
    const Dimension time = Dimension::of(0, 0, 1);
    const Dimension accel = length / (time * time);

    CHECK(accel == Dimension::of(1, 0, -2));
    CHECK((length / length).isDimensionless());
    CHECK((length * length).pow(Rational(1, 2)) == length);
}

TEST_CASE("Dimensions print in SI base units")
{
    CHECK(Dimension::of(1).toString() == "m");
    CHECK(Dimension::of(2).toString() == "m²");
    CHECK(Dimension::of(1, 0, -2).toString() == "m/s²");
    CHECK(Dimension::of(1, 1, -2).toString() == "kg·m/s²");
    CHECK(Dimension::of(-1, 1, -2).toString() == "kg/(m·s²)");
    CHECK(Dimension::of(0, 0, -1).toString() == "1/s");
    CHECK(Dimension::of(1).pow(Rational(1, 2)).toString() == "m^(1/2)");
}
