#include "lemma/dimension.h"

#include <string_view>
#include <vector>

namespace lemma {

Dimension Dimension::of(int length, int mass, int time, int current,
                        int temperature, int amount, int luminosity)
{
    Dimension d;
    d.exps_ = {length, mass, time, current, temperature, amount, luminosity};
    return d;
}

bool Dimension::isDimensionless() const
{
    for (const Rational& e : exps_) {
        if (!e.isZero()) {
            return false;
        }
    }
    return true;
}

Dimension Dimension::operator*(const Dimension& other) const
{
    Dimension d;
    for (std::size_t i = 0; i < kCount; ++i) {
        d.exps_[i] = exps_[i] + other.exps_[i];
    }
    return d;
}

Dimension Dimension::operator/(const Dimension& other) const
{
    Dimension d;
    for (std::size_t i = 0; i < kCount; ++i) {
        d.exps_[i] = exps_[i] - other.exps_[i];
    }
    return d;
}

Dimension Dimension::pow(Rational exponent) const
{
    Dimension d;
    for (std::size_t i = 0; i < kCount; ++i) {
        d.exps_[i] = exps_[i] * exponent;
    }
    return d;
}

std::string superscript(Rational exponent)
{
    if (!exponent.isInteger()) {
        return "^(" + exponent.toString() + ")";
    }
    static constexpr std::string_view kDigits[] = {"⁰", "¹", "²", "³", "⁴", "⁵", "⁶", "⁷", "⁸", "⁹"};
    std::string out;
    for (char c : std::to_string(exponent.num())) {
        if (c == '-') {
            out += "⁻";
        } else {
            out += kDigits[c - '0'];
        }
    }
    return out;
}

std::string Dimension::toString() const
{
    // Mathcad-style ordering of SI base units.
    struct Entry { BaseDimension dim; std::string_view symbol; };
    static constexpr Entry kOrder[] = {
        {BaseDimension::Mass, "kg"},      {BaseDimension::Length, "m"},
        {BaseDimension::Time, "s"},       {BaseDimension::Current, "A"},
        {BaseDimension::Temperature, "K"}, {BaseDimension::Amount, "mol"},
        {BaseDimension::Luminosity, "cd"},
    };

    auto join = [](const std::vector<std::string>& parts) {
        std::string s;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            s += (i ? "·" : "") + parts[i];
        }
        return s;
    };

    std::vector<std::string> numerator;
    std::vector<std::string> denominator;
    for (const Entry& e : kOrder) {
        const Rational exp = (*this)[e.dim];
        if (exp.isZero()) {
            continue;
        }
        const Rational mag = exp < Rational(0) ? -exp : exp;
        std::string part(e.symbol);
        if (!(mag == Rational(1))) {
            part += superscript(mag);
        }
        (exp < Rational(0) ? denominator : numerator).push_back(part);
    }

    if (denominator.empty()) {
        return join(numerator);
    }
    std::string s = numerator.empty() ? "1" : join(numerator);
    s += "/";
    s += denominator.size() > 1 ? "(" + join(denominator) + ")" : denominator.front();
    return s;
}

} // namespace lemma
