#pragma once

#include "lemma/dimension.h"

#include <string>

namespace lemma {

struct FormatOptions {
    int decimals = 3;           // maximum digits after the decimal point
    int exponentialThreshold = 3; // switch to ×10ⁿ notation at |x| ≥ 10^n or |x| < 10^-n
};

std::string formatNumber(double value, const FormatOptions& options = {});

// Formats a result in its default display unit, e.g. "20 J" or "9.807 m/s²".
std::string formatQuantity(const Quantity& q, const FormatOptions& options = {});

} // namespace lemma
