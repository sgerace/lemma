#pragma once

#include "lemma/dimension.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lemma {

struct Unit {
    std::string name;
    Quantity value; // the size of one of this unit, in SI base units
};

// The built-in unit table. Units are ordinary names that user variables may shadow,
// which is why grams are "gm": as in Mathcad, "g" is standard gravity.
class UnitRegistry {
public:
    static const UnitRegistry& instance();

    const Unit* find(std::string_view name) const;

    // The named unit results are displayed in by default (e.g. J for energy), or nullptr
    // when the result should be shown in SI base units.
    const Unit* preferredFor(const Dimension& dim) const;

private:
    UnitRegistry();

    std::unordered_map<std::string, Unit> units_;
    std::vector<std::string> preferred_;
};

} // namespace lemma
