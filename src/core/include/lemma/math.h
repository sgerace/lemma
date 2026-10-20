#pragma once

#include "lemma/ast.h"

#include <string>
#include <string_view>
#include <vector>

namespace lemma {

// A math region as the 2D editor sees it (docs/editor-spec.md, section 2). Unlike the
// AST it can hold placeholders, keeps parentheses the user typed, and has the
// statement (`:=`, `=`) as its root. It is what worksheets store; the engine gets a
// Statement lowered from it.
//
// Nodes are values, so copying a tree (for undo) copies it deeply. Every node has an
// id, unique in the process and kept by copies, which carets and errors refer to.
struct MathNode {
    enum class Kind {
        Number,      // text, as typed: "9.81", "1e-3"
        Identifier,  // text: "L", "x1", "kN"
        Placeholder, // an empty slot
        Binary,      // children: lhs, rhs; op: '+', '-', '*', or 0 for an operator placeholder; in
                     // results only, '~' for ± and ',' for a list of solutions
        Negate,      // children: operand; op '~' (in results only) for ±
        Fraction,    // children: numerator, denominator
        Power,       // children: base, exponent
        Radical,     // children: radicand
        Group,       // children: contents, in parentheses the user typed
        Call,        // children: name (an Identifier or Placeholder), then one or more arguments
        Definition,  // children: lhs, rhs. Only ever the root.
        Evaluation,  // children: expression, unit (a Placeholder when none). Only ever the root.
        // A symbolic evaluation, expression → result (docs/symbolic-spec.md, section 6).
        // children: expression, then one or more keyword slots, each a Placeholder, an
        // Identifier (simplify) or a Keyword (solve, σ). Only ever the root.
        Symbolic,
        Keyword,     // children: name (an Identifier or Placeholder), then one or more arguments
        Equation,    // children: lhs, rhs; drawn with a bold "=", written "=="
    };

    Kind kind = Kind::Placeholder;
    int id = 0;
    std::string text;        // Number, Identifier
    char op = 0;             // Binary
    bool juxtaposed = false; // Binary '*' written "2 m": shown as a thin space
    std::vector<MathNode> children;

    MathNode();
    MathNode(Kind kind, std::vector<MathNode> children = {});

    static MathNode number(std::string text);
    static MathNode identifier(std::string text);
    static MathNode binary(char op, MathNode lhs, MathNode rhs, bool juxtaposed = false);

    bool isLeaf() const { return kind == Kind::Number || kind == Kind::Identifier || kind == Kind::Placeholder; }
    // Typesets its children in slots of their own. Typing never climbs out of one.
    bool isBox() const;
    bool isStatement() const
    {
        return kind == Kind::Definition || kind == Kind::Evaluation || kind == Kind::Symbolic;
    }

    // Same structure and text, whatever the ids.
    bool sameAs(const MathNode& other) const;
};

// A fresh id for a new node.
int newMathNodeId();

MathNode* findMathNode(MathNode& root, int id);
const MathNode* findMathNode(const MathNode& root, int id);
// The parent of node `id`, and the node's index among its children; nullptr for the root.
MathNode* parentOf(MathNode& root, int id, int* index = nullptr);
const MathNode* parentOf(const MathNode& root, int id, int* index = nullptr);
// True if `id` is `ancestor` or below it.
bool contains(const MathNode& ancestor, int id);

// True if child `index` of `parent` is drawn in parentheses that precedence needs (the
// implied parentheses of section 2): a·(b + c), but not a + b·c.
bool needsParentheses(const MathNode& parent, int index);

// The first placeholder or operator placeholder in reading order, or nullptr. An empty
// unit or keyword slot is a placeholder too.
const MathNode* firstPlaceholder(const MathNode& root);
// The same, but ignoring an evaluation's empty unit and a symbolic evaluation's empty
// keyword slots, which may stay empty.
const MathNode* firstRequiredPlaceholder(const MathNode& root);

// True if node `id` is a name slot: a function's name, or a keyword's. Only a name can
// go there, and operators can't be typed in it.
bool isNameSlot(const MathNode& root, int id);

// Builds a tree from linear syntax, as pasting does (section 11.3): "/" becomes a
// fraction, "sqrt(x)" a radical. Throws lemma::Error, with byte offsets, if it doesn't
// parse.
MathNode mathFromLinear(std::string_view source);
MathNode mathFromStatement(const Statement& statement, std::string_view source);

// Linear syntax for a tree, as Copy writes it: it parses back to the same value. A
// placeholder is written "■" and an operator placeholder "□", which don't parse.
std::string toLinear(const MathNode& node);

// The statement a region's tree means, for the evaluator. Each lemma::Node's origin is
// the MathNode it came from. Throws lemma::Error, naming the node, for an incomplete
// tree or one that can't be evaluated (a definition of something that isn't a name).
// An empty unit placeholder in an evaluation is not an error.
Statement lowerMath(const MathNode& root);
NodePtr lowerExpression(const MathNode& node);

} // namespace lemma
