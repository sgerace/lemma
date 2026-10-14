#pragma once

#include "lemma/rational.h"

#include <array>
#include <cstddef>
#include <string>

namespace lemma {

enum class BaseDimension : std::size_t {
    Length,
    Mass,
    Time,
    Current,
    Temperature,
    Amount,
    Luminosity,
};

// The physical dimension of a quantity: a rational exponent for each SI base dimension.
class Dimension {
public:
    static constexpr std::size_t kCount = 7;

    Dimension() = default;

    // Integer exponents in SI base order: length, mass, time, current, temperature, amount, luminosity.
    static Dimension of(int length, int mass = 0, int time = 0, int current = 0,
                        int temperature = 0, int amount = 0, int luminosity = 0);

    const Rational& operator[](BaseDimension d) const { return exps_[static_cast<std::size_t>(d)]; }
    Rational& operator[](BaseDimension d) { return exps_[static_cast<std::size_t>(d)]; }

    bool isDimensionless() const;

    Dimension operator*(const Dimension& other) const;
    Dimension operator/(const Dimension& other) const;
    Dimension pow(Rational exponent) const;

    friend bool operator==(const Dimension&, const Dimension&) = default;

    // Expressed in SI base units, e.g. "kg·m/s²". Empty for dimensionless.
    std::string toString() const;

private:
    std::array<Rational, kCount> exps_{};
};

// A number together with its physical dimension. Values are always stored in SI base units.
struct Quantity {
    double value = 0.0;
    Dimension dim;

    static Quantity scalar(double v) { return {v, {}}; }
};

// Formats an exponent as Unicode superscripts ("²", "⁻¹"), or "^(1/2)" for fractions.
std::string superscript(Rational exponent);

} // namespace lemma
