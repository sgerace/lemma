#pragma once

#include "lemma/ast.h"
#include "lemma/dimension.h"
#include "lemma/evaluator.h"

namespace lemma {

// d/dx f and ∫ f dx, evaluated numerically (docs/symbolic-spec.md, section 15.3): the
// evaluator's calls diff(f, x, n) and integrate(f, x, a, b). A derivative is exact,
// through the symbolic engine; a definite integral is exact where the engine can find
// one, and otherwise computed by adaptive Gauss–Kronrod quadrature. Throws lemma::Error
// naming the call's node.
Quantity evaluateDerivative(const Node& call, const Environment& env);
Quantity evaluateIntegral(const Node& call, const Environment& env);

} // namespace lemma
