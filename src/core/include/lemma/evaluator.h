#pragma once

#include "lemma/ast.h"
#include "lemma/dimension.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lemma {

class Environment;

struct Function {
    std::vector<std::string> params;
    NodePtr body;
    // The definitions in effect where the function was defined. As in Mathcad, a
    // function sees what is above it on the worksheet, not what is above the call.
    std::shared_ptr<const Environment> closure;
};

// The names defined so far while evaluating a worksheet top to bottom.
class Environment {
public:
    explicit Environment(const Environment* parent = nullptr) : parent_(parent) {}

    void define(const std::string& name, Quantity value);
    void defineFunction(const std::string& name, Function function);

    const Quantity* variable(std::string_view name) const;
    const Function* function(std::string_view name) const;

private:
    const Environment* parent_;
    std::unordered_map<std::string, Quantity> variables_;
    std::unordered_map<std::string, Function> functions_;
};

enum class Lookup {
    VariablesFirst, // normal expressions: user names shadow units
    UnitsFirst,     // display units: "m" is metres even if m was defined
};

// Evaluates an expression. Throws lemma::Error for undefined names, unit mismatches, etc.
Quantity evaluate(const Node& node, const Environment& env, Lookup lookup = Lookup::VariablesFirst);

} // namespace lemma
