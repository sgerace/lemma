#pragma once

#include "lemma/symbolic.h"

#include <cstddef>
#include <stdexcept>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lemma {

// What `→` does with no keyword (docs/symbolic-spec.md, section 9.1): the canonical
// form, plus sin²u + cos²u = 1 and terms over the same denominator combined
// (a/b + c/b is (a + c)/b).
Expr simplify(const Expr& e);

// An operation's input is beyond its scope (section 9). Not an error: the region shows
// its input, simplified, with this as a note.
class BeyondScope : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// `expand` (section 9.2): products and integer powers of sums multiplied out, fractions
// split over the sum in their numerator, and ln of a product split. Throws lemma::Error
// past `maxTerms` terms.
Expr expand(const Expr& e, std::size_t maxTerms = 5000);

// `diff, x, n` (section 9.6): the nth derivative with respect to the variable `x`,
// simplified. Throws BeyondScope for a call to an undefined function of x.
Expr differentiate(const Expr& e, const std::string& x, int n = 1);

// `factor` (section 9.3): common factors taken out, and polynomials factored where
// they have rational roots (in one variable, or homogeneous in two: x² − y²), or are
// quadratics with real roots. Sums it can't factor are left multiplied out.
Expr factor(const Expr& e);

// True if `factor` would leave a polynomial of degree two or more unfactored in `e`:
// "Couldn't factor this further".
bool hasUnfactored(const Expr& factored);

// What a sum's terms have in common, and what's left, with whole coefficients:
// P x L − P x²/2 is P x/2 and 2 L − x. For anything else, 1 and the expression.
std::pair<Expr, Expr> splitCommonFactor(const Expr& sum);

// The back end for the operations that are hard in general (section 3). Each returns
// nothing when the expression is beyond its scope. The in-house engine is the first;
// solve and integrate join it in a later step.
class SymbolicBackend {
public:
    virtual ~SymbolicBackend() = default;
    virtual std::optional<Expr> factor(const Expr& e) const = 0;
};

const SymbolicBackend& inHouseBackend();

// True if the expression has the variable `name` in it.
bool dependsOn(const Expr& e, const std::string& name);

// Terms alike but for units of the same dimension are converted to the units that
// appear first in `order`, and combined: 2 m + 3 mm is 2.003 m (section 8). Converted
// coefficients show as decimals.
Expr combineUnits(const Expr& e, const std::vector<std::string>& order);

} // namespace lemma
