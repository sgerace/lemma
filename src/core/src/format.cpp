#include "lemma/format.h"

#include "lemma/units.h"

#include <cmath>
#include <cstdio>

namespace lemma {

namespace {

std::string fixed(double value, int decimals)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, value);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') {
            s.pop_back();
        }
        if (s.back() == '.') {
            s.pop_back();
        }
    }
    if (s == "-0") {
        s = "0";
    }
    return s;
}

} // namespace

std::string formatNumber(double value, const FormatOptions& options)
{
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return value > 0 ? "∞" : "-∞";
    }
    const double magnitude = std::abs(value);
    const double upper = std::pow(10.0, options.exponentialThreshold);
    if (magnitude == 0.0 || (magnitude < upper && magnitude >= 1.0 / upper)) {
        return fixed(value, options.decimals);
    }

    int exponent = static_cast<int>(std::floor(std::log10(magnitude)));
    const double scale = std::pow(10.0, options.decimals);
    double mantissa = std::round(value / std::pow(10.0, exponent) * scale) / scale;
    if (std::abs(mantissa) >= 10.0) {
        mantissa /= 10.0;
        ++exponent;
    }
    return fixed(mantissa, options.decimals) + "×10" + superscript(exponent);
}

std::string formatQuantity(const Quantity& q, const FormatOptions& options)
{
    if (q.dim.isDimensionless()) {
        return formatNumber(q.value, options);
    }
    if (const Unit* unit = UnitRegistry::instance().preferredFor(q.dim)) {
        return formatNumber(q.value / unit->value.value, options) + " " + unit->name;
    }
    return formatNumber(q.value, options) + " " + q.dim.toString();
}

} // namespace lemma
