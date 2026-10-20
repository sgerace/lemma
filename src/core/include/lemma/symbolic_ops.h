#pragma once

#include "lemma/symbolic.h"

#include <string>
#include <vector>

namespace lemma {

// What `→` does with no keyword (docs/symbolic-spec.md, section 9.1): the canonical
// form, plus sin²u + cos²u = 1 and terms over the same denominator combined
// (a/b + c/b is (a + c)/b).
Expr simplify(const Expr& e);

// Terms alike but for units of the same dimension are converted to the units that
// appear first in `order`, and combined: 2 m + 3 mm is 2.003 m (section 8). Converted
// coefficients show as decimals.
Expr combineUnits(const Expr& e, const std::vector<std::string>& order);

} // namespace lemma
