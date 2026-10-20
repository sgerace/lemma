#pragma once

// Shared between the symbolic engine's own files; not part of lemma_core's interface.

#include "lemma/symbolic.h"

#include <optional>
#include <vector>

namespace lemma::detail {

// A sum as a polynomial in v: its coefficients, lowest degree first, free of v. Nothing
// if it isn't one (v under a root, in a function, or to a power that isn't whole).
std::optional<std::vector<Expr>> coefficientsIn(const Expr& sum, const Expr& v);

// The in-house factoring of a sum or product (section 9.3).
std::optional<Expr> factorInHouse(const Expr& e);

} // namespace lemma::detail
