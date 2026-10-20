#pragma once

#include "lemma/format.h"
#include "lemma/math.h"
#include "lemma/symbolic.h"

#include <span>
#include <string>
#include <vector>

namespace lemma {

// A definition above a symbolic evaluation, which it substitutes (section 7).
struct SymbolicDefinition {
    std::string name;
    std::vector<std::string> params; // a function's
    bool function = false;
    const MathNode* body = nullptr;  // the right-hand side; it sees the definitions before it
};

struct LowerOptions {
    // The definitions above the region, in worksheet order.
    std::span<const SymbolicDefinition> definitions;
    std::vector<std::string> keep; // names left symbolic: the `keep` keyword
    std::vector<std::string> real; // names that may be negative: the `real` keyword
    // Out: the identifiers of the region's tree that meant units, to show them upright.
    std::vector<int>* units = nullptr;
    // Out: names in the order they first appear, definitions' included where they're
    // substituted, for the display order (section 11).
    std::vector<std::string>* order = nullptr;
};

// The expression a tree means (docs/symbolic-spec.md, section 4): fractions are
// negative powers, radicals are powers of ½, and groups disappear. Names defined above
// are replaced by their definitions, and a unit name is a unit where it follows a
// number. Throws lemma::Error naming the node, for an incomplete tree or one with no
// value (a division by zero, units that don't match).
Expr exprFromMath(const MathNode& node, const LowerOptions& options = {});

// The names in a tree, in the order they first appear: the order results show them in.
std::vector<std::string> namesInOrder(const MathNode& node);

struct ExprDisplayOptions {
    std::vector<std::string> order; // from namesInOrder()
    FormatOptions format;           // for decimals
    // The main variable (section 11), when an operation names one: diff, x.
    std::string main;
    // Out: the result's identifiers that are units, to show them upright.
    std::vector<int>* upright = nullptr;
};

// A tree that shows the expression, following the display rules (section 11):
// w L⁴/(8 E I), not (1/8)·w·L⁴·E⁻¹·I⁻¹.
MathNode mathFromExpr(const Expr& e, const ExprDisplayOptions& options = {});

// solve's solutions for x, as a list: x = …, x = … (section 9.5). Two that differ only
// in a root's sign are shown as one, with ±: x = (−b ± √(b² − 4 a c))/(2 a).
MathNode mathFromSolutions(const std::string& x, const std::vector<Expr>& solutions,
                           const ExprDisplayOptions& options = {});

} // namespace lemma
