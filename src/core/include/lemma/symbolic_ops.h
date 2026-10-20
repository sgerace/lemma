#pragma once

#include "lemma/symbolic.h"

namespace lemma {

// What `→` does with no keyword (docs/symbolic-spec.md, section 9.1): the canonical
// form, plus sin²u + cos²u = 1 and terms over the same denominator combined
// (a/b + c/b is (a + c)/b).
Expr simplify(const Expr& e);

} // namespace lemma
