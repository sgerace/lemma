#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace lemma {

struct Node;
using NodePtr = std::shared_ptr<const Node>;

struct Node {
    enum class Kind { Number, Identifier, Unary, Binary, Call };

    Kind kind = Kind::Number;
    std::size_t begin = 0; // byte range in the source
    std::size_t end = 0;

    double number = 0.0;       // Number
    std::string name;          // Identifier, Call
    char op = 0;               // Unary: '-'; Binary: '+', '-', '*', '/', '^'
    std::vector<NodePtr> args; // Unary: operand; Binary: lhs, rhs; Call: arguments
};

// The content of one math region.
struct Statement {
    enum class Kind {
        Empty,
        Expression,         // 2 + 3            (nothing displayed)
        Definition,         // x := 2 m
        FunctionDefinition, // f(x) := x^2
        Evaluation,         // x + 1 =   or   x + 1 = ft
    };

    Kind kind = Kind::Empty;
    std::string name;                // Definition, FunctionDefinition
    std::vector<std::string> params; // FunctionDefinition
    NodePtr expr;
    NodePtr unit;                    // Evaluation: optional display unit
    std::size_t equalsPos = 0;       // Evaluation: byte offset of '='
    std::size_t assignPos = 0;       // Definition, FunctionDefinition: byte offset of ':='
    std::string unitText;            // Evaluation: display unit as typed
};

} // namespace lemma
