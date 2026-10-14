#include "lemma/error.h"
#include "lemma/parser.h"

#include <catch2/catch_test_macros.hpp>

using namespace lemma;

namespace {

// Renders an expression tree as fully parenthesized text, to check precedence.
std::string show(const Node& n)
{
    switch (n.kind) {
    case Node::Kind::Number: {
        std::string s = std::to_string(n.number);
        s.erase(s.find_last_not_of('0') + 1);
        if (s.back() == '.') s.pop_back();
        return s;
    }
    case Node::Kind::Identifier:
        return n.name;
    case Node::Kind::Unary:
        return "(-" + show(*n.args[0]) + ")";
    case Node::Kind::Binary:
        return "(" + show(*n.args[0]) + " " + n.op + " " + show(*n.args[1]) + ")";
    case Node::Kind::Call: {
        std::string s = n.name + "(";
        for (std::size_t i = 0; i < n.args.size(); ++i) {
            s += (i ? ", " : "") + show(*n.args[i]);
        }
        return s + ")";
    }
    }
    return "?";
}

std::string expr(std::string_view src)
{
    return show(*parseStatement(src).expr);
}

} // namespace

TEST_CASE("Operator precedence and associativity")
{
    CHECK(expr("1 + 2 * 3") == "(1 + (2 * 3))");
    CHECK(expr("1 - 2 - 3") == "((1 - 2) - 3)");
    CHECK(expr("2 ^ 3 ^ 2") == "(2 ^ (3 ^ 2))");
    CHECK(expr("-2 ^ 2") == "(-(2 ^ 2))");
    CHECK(expr("2 ^ -1") == "(2 ^ (-1))");
    CHECK(expr("(1 + 2) * 3") == "((1 + 2) * 3)");
}

TEST_CASE("Juxtaposition multiplies, binding like '*'")
{
    CHECK(expr("10 N") == "(10 * N)");
    CHECK(expr("10 N/m") == "((10 * N) / m)");
    CHECK(expr("5 m^2") == "(5 * (m ^ 2))");
    CHECK(expr("2(x + 1)") == "(2 * (x + 1))");
    CHECK(expr("2.5e3 kg") == "(2500 * kg)");
    CHECK(expr("2 e") == "(2 * e)");
}

TEST_CASE("Unicode operators")
{
    CHECK(expr("a·b") == "(a * b)");
    CHECK(expr("a×b÷c") == "((a * b) / c)");
    CHECK(expr("a − b") == "(a - b)");
    CHECK(expr("π r^2") == "(π * (r ^ 2))");
}

TEST_CASE("Statement kinds")
{
    CHECK(parseStatement("").kind == Statement::Kind::Empty);
    CHECK(parseStatement("   ").kind == Statement::Kind::Empty);
    CHECK(parseStatement("1 + 2").kind == Statement::Kind::Expression);

    const Statement def = parseStatement("F := 10 N");
    CHECK(def.kind == Statement::Kind::Definition);
    CHECK(def.name == "F");

    const Statement fn = parseStatement("f(x, y) := x y");
    CHECK(fn.kind == Statement::Kind::FunctionDefinition);
    CHECK(fn.name == "f");
    CHECK(fn.params == std::vector<std::string>{"x", "y"});

    const Statement call = parseStatement("f(2, 3) =");
    CHECK(call.kind == Statement::Kind::Evaluation);
    CHECK(show(*call.expr) == "f(2, 3)");
    CHECK(call.unit == nullptr);

    const Statement withUnit = parseStatement("L F = kN m");
    CHECK(withUnit.kind == Statement::Kind::Evaluation);
    CHECK(withUnit.equalsPos == 4);
    CHECK(withUnit.unitText == "kN m");
}

TEST_CASE("Malformed input reports where the problem is")
{
    CHECK_THROWS_AS(parseStatement("1 +"), Error);
    CHECK_THROWS_AS(parseStatement("(1 + 2"), Error);
    CHECK_THROWS_AS(parseStatement("2 := 3"), Error);
    CHECK_THROWS_AS(parseStatement("1 $ 2"), Error);

    try {
        parseStatement("x := 1 $ 2");
        FAIL("expected an error");
    } catch (const Error& e) {
        CHECK(e.begin() == 7);
        CHECK(e.end() == 8);
    }
}
