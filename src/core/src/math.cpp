#include "lemma/math.h"

#include "lemma/error.h"
#include "lemma/parser.h"

#include <fast_float/fast_float.h>

#include <atomic>
#include <system_error>
#include <utility>

namespace lemma {

int newMathNodeId()
{
    static std::atomic<int> next{1};
    return next++;
}

MathNode::MathNode() : id(newMathNodeId()) {}

MathNode::MathNode(Kind k, std::vector<MathNode> c) : kind(k), id(newMathNodeId()), children(std::move(c)) {}

MathNode MathNode::number(std::string text)
{
    MathNode n(Kind::Number);
    n.text = std::move(text);
    return n;
}

MathNode MathNode::identifier(std::string text)
{
    MathNode n(Kind::Identifier);
    n.text = std::move(text);
    return n;
}

MathNode MathNode::binary(char op, MathNode lhs, MathNode rhs, bool juxtaposed)
{
    MathNode n(Kind::Binary, {std::move(lhs), std::move(rhs)});
    n.op = op;
    n.juxtaposed = juxtaposed;
    return n;
}

bool MathNode::isBox() const
{
    switch (kind) {
    case Kind::Fraction:
    case Kind::Power:
    case Kind::Radical:
    case Kind::Group:
    case Kind::Call:
        return true;
    default:
        return false;
    }
}

bool MathNode::sameAs(const MathNode& other) const
{
    if (kind != other.kind || text != other.text || op != other.op || juxtaposed != other.juxtaposed
        || children.size() != other.children.size()) {
        return false;
    }
    for (std::size_t i = 0; i < children.size(); ++i) {
        if (!children[i].sameAs(other.children[i])) {
            return false;
        }
    }
    return true;
}

const MathNode* findMathNode(const MathNode& root, int id)
{
    if (root.id == id) {
        return &root;
    }
    for (const MathNode& child : root.children) {
        if (const MathNode* found = findMathNode(child, id)) {
            return found;
        }
    }
    return nullptr;
}

MathNode* findMathNode(MathNode& root, int id)
{
    return const_cast<MathNode*>(findMathNode(std::as_const(root), id));
}

const MathNode* parentOf(const MathNode& root, int id, int* index)
{
    for (std::size_t i = 0; i < root.children.size(); ++i) {
        if (root.children[i].id == id) {
            if (index) {
                *index = static_cast<int>(i);
            }
            return &root;
        }
        if (const MathNode* found = parentOf(root.children[i], id, index)) {
            return found;
        }
    }
    return nullptr;
}

MathNode* parentOf(MathNode& root, int id, int* index)
{
    return const_cast<MathNode*>(parentOf(std::as_const(root), id, index));
}

bool contains(const MathNode& ancestor, int id)
{
    return findMathNode(ancestor, id) != nullptr;
}

bool needsParentheses(const MathNode& parent, int index)
{
    const MathNode& child = parent.children[static_cast<std::size_t>(index)];
    const bool sum = child.kind == MathNode::Kind::Binary && (child.op == '+' || child.op == '-');
    const bool product = child.kind == MathNode::Kind::Binary && !sum;
    switch (parent.kind) {
    case MathNode::Kind::Binary:
        if (parent.op == '+' || parent.op == '-') {
            return index == 1 && sum;
        }
        return sum || (index == 1 && (product || child.kind == MathNode::Kind::Negate));
    case MathNode::Kind::Negate:
        return sum || child.kind == MathNode::Kind::Negate;
    case MathNode::Kind::Power:
        return index == 0
               && (child.kind == MathNode::Kind::Binary || child.kind == MathNode::Kind::Negate
                   || child.kind == MathNode::Kind::Fraction || child.kind == MathNode::Kind::Power);
    default:
        return false;
    }
}

const MathNode* firstPlaceholder(const MathNode& root)
{
    if (root.kind == MathNode::Kind::Placeholder || (root.kind == MathNode::Kind::Binary && root.op == 0)) {
        return &root;
    }
    for (const MathNode& child : root.children) {
        if (const MathNode* found = firstPlaceholder(child)) {
            return found;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// From linear syntax

namespace {

MathNode fromAst(const Node& n, std::string_view source)
{
    switch (n.kind) {
    case Node::Kind::Number:
        // Keep the number as it was typed, not as the double it parsed to.
        return MathNode::number(std::string(source.substr(n.begin, n.end - n.begin)));
    case Node::Kind::Identifier:
        return MathNode::identifier(n.name);
    case Node::Kind::Unary:
        return MathNode(MathNode::Kind::Negate, {fromAst(*n.args[0], source)});
    case Node::Kind::Binary: {
        MathNode lhs = fromAst(*n.args[0], source);
        MathNode rhs = fromAst(*n.args[1], source);
        switch (n.op) {
        case '/':
            return MathNode(MathNode::Kind::Fraction, {std::move(lhs), std::move(rhs)});
        case '^':
            return MathNode(MathNode::Kind::Power, {std::move(lhs), std::move(rhs)});
        default:
            return MathNode::binary(n.op, std::move(lhs), std::move(rhs), n.juxtaposed);
        }
    }
    case Node::Kind::Call: {
        if (n.name == "sqrt" && n.args.size() == 1) {
            return MathNode(MathNode::Kind::Radical, {fromAst(*n.args[0], source)});
        }
        MathNode call(MathNode::Kind::Call, {MathNode::identifier(n.name)});
        for (const NodePtr& arg : n.args) {
            call.children.push_back(fromAst(*arg, source));
        }
        return call;
    }
    }
    return MathNode();
}

} // namespace

MathNode mathFromStatement(const Statement& s, std::string_view source)
{
    switch (s.kind) {
    case Statement::Kind::Empty:
        return MathNode();
    case Statement::Kind::Expression:
        return fromAst(*s.expr, source);
    case Statement::Kind::Definition:
        return MathNode(MathNode::Kind::Definition, {MathNode::identifier(s.name), fromAst(*s.expr, source)});
    case Statement::Kind::FunctionDefinition: {
        MathNode lhs(MathNode::Kind::Call, {MathNode::identifier(s.name)});
        for (const std::string& param : s.params) {
            lhs.children.push_back(MathNode::identifier(param));
        }
        if (s.params.empty()) {
            lhs.children.emplace_back(); // f() has no linear form in the tree; leave a slot to fill
        }
        return MathNode(MathNode::Kind::Definition, {std::move(lhs), fromAst(*s.expr, source)});
    }
    case Statement::Kind::Evaluation:
        return MathNode(MathNode::Kind::Evaluation,
                        {fromAst(*s.expr, source), s.unit ? fromAst(*s.unit, source) : MathNode()});
    }
    return MathNode();
}

MathNode mathFromLinear(std::string_view source)
{
    return mathFromStatement(parseStatement(source), source);
}

// ---------------------------------------------------------------------------
// To linear syntax

namespace {

// How tightly a node binds in linear syntax, loosest first.
int precedence(const MathNode& n)
{
    switch (n.kind) {
    case MathNode::Kind::Binary:
        return n.op == '+' || n.op == '-' ? 1 : 2;
    case MathNode::Kind::Fraction:
        return 2;
    case MathNode::Kind::Negate:
        return 3;
    case MathNode::Kind::Power:
        return 4;
    default:
        return 5;
    }
}

std::string linear(const MathNode& n);

std::string wrapped(const MathNode& n, bool parens)
{
    return parens ? "(" + linear(n) + ")" : linear(n);
}

// Juxtaposition only parses back as multiplication before a name or a parenthesis.
bool juxtaposes(const MathNode& rhs)
{
    switch (rhs.kind) {
    case MathNode::Kind::Identifier:
    case MathNode::Kind::Group:
    case MathNode::Kind::Call:
    case MathNode::Kind::Radical:
        return true;
    case MathNode::Kind::Power:
        return juxtaposes(rhs.children[0]);
    default:
        return false;
    }
}

std::string linear(const MathNode& n)
{
    const auto& c = n.children;
    switch (n.kind) {
    case MathNode::Kind::Number:
    case MathNode::Kind::Identifier:
        return n.text;
    case MathNode::Kind::Placeholder:
        return "■";
    case MathNode::Kind::Binary: {
        const int p = precedence(n);
        const std::string lhs = wrapped(c[0], precedence(c[0]) < p);
        // The tree is explicit, so a right operand of equal precedence keeps its grouping.
        const bool rhsParens = precedence(c[1]) <= p && c[1].kind != MathNode::Kind::Negate;
        const std::string rhs = wrapped(c[1], rhsParens);
        switch (n.op) {
        case '+': return lhs + " + " + rhs;
        case '-': return lhs + " - " + rhs;
        case '*':
            if (n.juxtaposed && (rhsParens || juxtaposes(c[1]))) {
                return lhs + " " + rhs;
            }
            return lhs + "*" + rhs;
        default: return lhs + " □ " + rhs;
        }
    }
    case MathNode::Kind::Negate:
        return "-" + wrapped(c[0], precedence(c[0]) < 3);
    case MathNode::Kind::Fraction:
        return wrapped(c[0], precedence(c[0]) < 2) + "/" + wrapped(c[1], precedence(c[1]) <= 2);
    case MathNode::Kind::Power:
        return wrapped(c[0], precedence(c[0]) < 5) + "^" + wrapped(c[1], precedence(c[1]) < 3);
    case MathNode::Kind::Radical:
        return "sqrt(" + linear(c[0]) + ")";
    case MathNode::Kind::Group:
        return "(" + linear(c[0]) + ")";
    case MathNode::Kind::Call: {
        std::string s = linear(c[0]) + "(";
        for (std::size_t i = 1; i < c.size(); ++i) {
            s += (i > 1 ? ", " : "") + linear(c[i]);
        }
        return s + ")";
    }
    case MathNode::Kind::Definition:
        return linear(c[0]) + " := " + linear(c[1]);
    case MathNode::Kind::Evaluation:
        return c[1].kind == MathNode::Kind::Placeholder ? linear(c[0]) + " =" : linear(c[0]) + " = " + linear(c[1]);
    }
    return {};
}

} // namespace

std::string toLinear(const MathNode& node)
{
    return linear(node);
}

// ---------------------------------------------------------------------------
// Lowering

namespace {

std::shared_ptr<Node> make(Node::Kind kind, const MathNode& from, std::vector<NodePtr> args = {})
{
    auto n = std::make_shared<Node>();
    n->kind = kind;
    n->origin = from.id;
    n->args = std::move(args);
    return n;
}

double parseNumber(const MathNode& n)
{
    double value = 0.0;
    const char* first = n.text.data();
    const char* last = first + n.text.size();
    const auto result = fast_float::from_chars(first, last, value);
    if (result.ec == std::errc::result_out_of_range) {
        throw Error("Number is out of range", 0, 0, n.id);
    }
    if (n.text.empty() || result.ec != std::errc() || result.ptr != last) {
        throw Error("This number is incomplete", 0, 0, n.id);
    }
    return value;
}

} // namespace

NodePtr lowerExpression(const MathNode& m)
{
    const auto& c = m.children;
    switch (m.kind) {
    case MathNode::Kind::Number: {
        auto n = make(Node::Kind::Number, m);
        n->number = parseNumber(m);
        return n;
    }
    case MathNode::Kind::Identifier: {
        auto n = make(Node::Kind::Identifier, m);
        n->name = m.text;
        return n;
    }
    case MathNode::Kind::Binary: {
        if (m.op == 0) {
            throw Error("Fill in the placeholder", 0, 0, m.id);
        }
        auto n = make(Node::Kind::Binary, m, {lowerExpression(c[0]), lowerExpression(c[1])});
        n->op = m.op;
        n->juxtaposed = m.juxtaposed;
        return n;
    }
    case MathNode::Kind::Negate: {
        auto n = make(Node::Kind::Unary, m, {lowerExpression(c[0])});
        n->op = '-';
        return n;
    }
    case MathNode::Kind::Fraction:
    case MathNode::Kind::Power: {
        auto n = make(Node::Kind::Binary, m, {lowerExpression(c[0]), lowerExpression(c[1])});
        n->op = m.kind == MathNode::Kind::Fraction ? '/' : '^';
        return n;
    }
    case MathNode::Kind::Radical: {
        auto n = make(Node::Kind::Call, m, {lowerExpression(c[0])});
        n->name = "sqrt";
        return n;
    }
    case MathNode::Kind::Group:
        return lowerExpression(c[0]);
    case MathNode::Kind::Call: {
        if (c[0].kind != MathNode::Kind::Identifier) {
            throw Error("Fill in the placeholder", 0, 0, c[0].id);
        }
        std::vector<NodePtr> args;
        for (std::size_t i = 1; i < c.size(); ++i) {
            args.push_back(lowerExpression(c[i]));
        }
        auto n = make(Node::Kind::Call, m, std::move(args));
        n->name = c[0].text;
        return n;
    }
    case MathNode::Kind::Placeholder:
        throw Error("Fill in the placeholder", 0, 0, m.id);
    case MathNode::Kind::Definition:
    case MathNode::Kind::Evaluation:
        break;
    }
    throw Error("Only a whole region can be a definition or evaluation", 0, 0, m.id);
}

Statement lowerMath(const MathNode& root)
{
    Statement s;
    const auto& c = root.children;
    switch (root.kind) {
    case MathNode::Kind::Placeholder:
        return s; // an empty region
    case MathNode::Kind::Definition: {
        const MathNode& lhs = c[0];
        if (lhs.kind == MathNode::Kind::Identifier) {
            s.kind = Statement::Kind::Definition;
            s.name = lhs.text;
        } else if (lhs.kind == MathNode::Kind::Call && lhs.children[0].kind == MathNode::Kind::Identifier) {
            s.kind = Statement::Kind::FunctionDefinition;
            s.name = lhs.children[0].text;
            for (std::size_t i = 1; i < lhs.children.size(); ++i) {
                const MathNode& param = lhs.children[i];
                if (param.kind == MathNode::Kind::Placeholder) {
                    throw Error("Fill in the placeholder", 0, 0, param.id);
                }
                if (param.kind != MathNode::Kind::Identifier) {
                    throw Error("A function's parameters must be names", 0, 0, param.id);
                }
                s.params.push_back(param.text);
            }
        } else if (const MathNode* hole = firstPlaceholder(lhs)) {
            throw Error("Fill in the placeholder", 0, 0, hole->id);
        } else {
            throw Error("Only a name or a function can be defined with :=", 0, 0, lhs.id);
        }
        s.expr = lowerExpression(c[1]);
        return s;
    }
    case MathNode::Kind::Evaluation:
        s.kind = Statement::Kind::Evaluation;
        s.expr = lowerExpression(c[0]);
        if (c[1].kind != MathNode::Kind::Placeholder) {
            s.unit = lowerExpression(c[1]);
            s.unitText = toLinear(c[1]);
        }
        return s;
    default:
        s.kind = Statement::Kind::Expression;
        s.expr = lowerExpression(root);
        return s;
    }
}

} // namespace lemma
