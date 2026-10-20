#pragma once

#include "lemma/symbolic.h"

#include <cstddef>
#include <stdexcept>
#include <string>
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

// True if the expression has the variable `name` in it.
bool dependsOn(const Expr& e, const std::string& name);

// Terms alike but for units of the same dimension are converted to the units that
// appear first in `order`, and combined: 2 m + 3 mm is 2.003 m (section 8). Converted
// coefficients show as decimals.
Expr combineUnits(const Expr& e, const std::vector<std::string>& order);

} // namespace lemma
