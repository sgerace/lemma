#pragma once

#include "lemma/ast.h"

#include <string_view>

namespace lemma {

// Parses one region of linear math input. Throws lemma::Error on malformed input.
//
//   x := 10 N           definition (juxtaposition multiplies, binding like '*')
//   f(a, b) := a·b      function definition
//   x / 2 m =           evaluation, shown in the default unit
//   x / 2 m = kN/m      evaluation, shown in the given unit
Statement parseStatement(std::string_view source);

} // namespace lemma
