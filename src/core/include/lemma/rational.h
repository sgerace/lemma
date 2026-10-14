#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>

namespace lemma {

// An exact fraction, used for unit exponents so that sqrt(m^2) is exactly m.
class Rational {
public:
    constexpr Rational() = default;
    constexpr Rational(std::int64_t n) : num_(n) {}

    Rational(std::int64_t n, std::int64_t d) : num_(n), den_(d)
    {
        if (d == 0) {
            throw std::domain_error("Rational with zero denominator");
        }
        normalize();
    }

    std::int64_t num() const { return num_; }
    std::int64_t den() const { return den_; }

    bool isZero() const { return num_ == 0; }
    bool isInteger() const { return den_ == 1; }
    double toDouble() const { return static_cast<double>(num_) / static_cast<double>(den_); }

    std::string toString() const
    {
        return isInteger() ? std::to_string(num_)
                           : std::to_string(num_) + "/" + std::to_string(den_);
    }

    // Recovers a simple fraction from a double (e.g. 0.5 -> 1/2), if there is one.
    static std::optional<Rational> fromDouble(double x, std::int64_t maxDen = 1000)
    {
        if (!std::isfinite(x)) {
            return std::nullopt;
        }
        for (std::int64_t d = 1; d <= maxDen; ++d) {
            const double n = std::round(x * static_cast<double>(d));
            if (std::abs(n / static_cast<double>(d) - x) < 1e-9) {
                return Rational(static_cast<std::int64_t>(n), d);
            }
        }
        return std::nullopt;
    }

    friend Rational operator+(Rational a, Rational b) { return {a.num_ * b.den_ + b.num_ * a.den_, a.den_ * b.den_}; }
    friend Rational operator-(Rational a, Rational b) { return {a.num_ * b.den_ - b.num_ * a.den_, a.den_ * b.den_}; }
    friend Rational operator*(Rational a, Rational b) { return {a.num_ * b.num_, a.den_ * b.den_}; }
    friend Rational operator/(Rational a, Rational b) { return {a.num_ * b.den_, a.den_ * b.num_}; }
    Rational operator-() const { return {-num_, den_}; }

    friend bool operator==(Rational a, Rational b) = default;
    friend bool operator<(Rational a, Rational b) { return a.num_ * b.den_ < b.num_ * a.den_; }
    friend bool operator>(Rational a, Rational b) { return b < a; }

private:
    void normalize()
    {
        if (den_ < 0) {
            num_ = -num_;
            den_ = -den_;
        }
        const std::int64_t g = std::gcd(num_, den_);
        if (g > 1) {
            num_ /= g;
            den_ /= g;
        }
    }

    std::int64_t num_ = 0;
    std::int64_t den_ = 1;
};

} // namespace lemma
