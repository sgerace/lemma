#pragma once

#include "lemma/format.h"
#include "lemma/math.h"
#include "lemma/symbolic.h"

#include <string>
#include <vector>

namespace lemma {

// The expression a tree means (docs/symbolic-spec.md, section 4): fractions are
// negative powers, radicals are powers of ½, and groups disappear. Throws lemma::Error
// naming the node, for an incomplete tree or one with no value (a division by zero).
Expr exprFromMath(const MathNode& node);

// The names in a tree, in the order they first appear: the order results show them in.
std::vector<std::string> namesInOrder(const MathNode& node);

struct ExprDisplayOptions {
    std::vector<std::string> order; // from namesInOrder()
    FormatOptions format;           // for decimals
};

// A tree that shows the expression, following the display rules (section 11):
// w L⁴/(8 E I), not (1/8)·w·L⁴·E⁻¹·I⁻¹.
MathNode mathFromExpr(const Expr& e, const ExprDisplayOptions& options = {});

} // namespace lemma
