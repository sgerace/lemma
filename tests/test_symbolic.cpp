// Symbolic expressions (docs/symbolic-spec.md): canonical form, the bridge to and from
// the editor tree, and the display rules. Results are compared as linear text, the
// form Copy writes.

#include "lemma/error.h"
#include "lemma/evaluator.h"
#include "lemma/symbolic.h"
#include "lemma/symbolic_bridge.h"
#include "lemma/symbolic_ops.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>
#include <random>
#include <string>

using namespace lemma;

namespace {

Expr parse(std::string_view source)
{
    return exprFromMath(mathFromLinear(source));
}

ExprDisplayOptions orderOf(const MathNode& from)
{
    ExprDisplayOptions options;
    options.order = namesInOrder(from);
    return options;
}

MathNode show(const Expr& e, const MathNode& from)
{
    return mathFromExpr(e, orderOf(from));
}

// What `source →` shows.
std::string simplified(std::string_view source)
{
    const MathNode tree = mathFromLinear(source);
    return toLinear(show(simplify(exprFromMath(tree)), tree));
}

// The node an error names.
int errorNode(const MathNode& tree)
{
    try {
        simplify(exprFromMath(tree));
    } catch (const Error& e) {
        return e.node();
    }
    return 0;
}

} // namespace

TEST_CASE("Numbers fold, and like terms and like factors combine")
{
    CHECK(simplified("2 + 3") == "5");
    CHECK(simplified("x*1") == "x");
    CHECK(simplified("x^0") == "1");
    CHECK(simplified("x - x") == "0");
    CHECK(simplified("2*x + 3*x") == "5 x");
    CHECK(simplified("x^2*x^3") == "x^5");
    CHECK(simplified("sqrt(x)*sqrt(x)") == "x");
    CHECK(simplified("(x + 1)*(x + 1)") == "(x + 1)^2");
    CHECK(simplified("x*(x + 1)/x") == "x + 1");
    CHECK(simplified("2*(x + y) - (x + y)") == "x + y");
    CHECK(simplified("1 - (z + 3)") == "-(z + 2)"); // a multiple of a sum, in a sum, is multiplied out
    CHECK(simplified("2*(x + 1)") == "2 (x + 1)");  // but not on its own
}

TEST_CASE("Equal expressions are identical, whatever order they were written in")
{
    CHECK(parse("a*b + c") == parse("c + b*a"));
    CHECK(parse("x^2*y/x") == parse("y*x"));
    CHECK(parse("(a + b)^2") == parse("(b + a)*(a + b)"));
    CHECK(parse("a + b") != parse("a - b"));
    CHECK(parse("2^(1/2)") != parse("2"));
}

TEST_CASE("Results are fractions with the coefficient in front")
{
    CHECK(simplified("w*L^4/(8*E*I)") == "w L^4/(8 E I)");
    CHECK(simplified("(1/8)*w*L^4*E^(-1)*I^(-1)") == "w L^4/(8 E I)");
    CHECK(simplified("3*w*L/(8*E)") == "3 w L/(8 E)");
    CHECK(simplified("3/(2*x)") == "3/(2 x)");
    CHECK(simplified("x/2") == "x/2");
    CHECK(simplified("1/x") == "1/x");
    CHECK(simplified("1/sqrt(x)") == "1/sqrt(x)");
    CHECK(simplified("x^(-2/3)") == "1/x^(2/3)");
}

TEST_CASE("Factors and terms are in the order their names first appear")
{
    CHECK(simplified("y*x") == "y x");
    CHECK(simplified("b + a") == "b + a");
    CHECK(simplified("2*pi*r") == "2 π r");
    CHECK(simplified("r*pi*2") == "2 π r");
    CHECK(simplified("P*x*(2*L - x)/(2*E*I)") == "P x*(2 L - x)/(2 E I)"); // juxtaposed; Copy writes "*" before "("
    CHECK(simplified("x*sin(k*x)") == "x sin(k x)");
}

TEST_CASE("A polynomial is in descending powers of its main variable")
{
    CHECK(simplified("6 - 5*x + x^2") == "x^2 - 5 x + 6");
    CHECK(simplified("c + b*x + a*x^2") == "a x^2 + b x + c");
    CHECK(simplified("1 + x") == "x + 1");
}

TEST_CASE("Negative terms are subtractions, and an all-negative sum is negated")
{
    CHECK(simplified("a + (-1)*b") == "a - b");
    CHECK(simplified("-a - b") == "-(a + b)");
    CHECK(simplified("-a + b") == "-a + b");
    CHECK(simplified("-(x + 1)") == "-(x + 1)");
    CHECK(simplified("a - b/c") == "a - b/c");
    CHECK(simplified("-3") == "-3");
    CHECK(simplified("-x/2") == "-(x/2)"); // as Copy writes a negated fraction
}

TEST_CASE("Roots are exact")
{
    CHECK(simplified("sqrt(4)") == "2");
    CHECK(simplified("sqrt(8)") == "2 sqrt(2)");
    CHECK(simplified("sqrt(2)*sqrt(6)") == "2 sqrt(3)");
    CHECK(simplified("8^(1/3)") == "2");
    CHECK(simplified("4^(3/2)") == "8");
    CHECK(simplified("sqrt(1/2)") == "sqrt(2)/2");
    CHECK(simplified("(-8)^(1/3)") == "-2");
    CHECK(simplified("sqrt(x^2)") == "x");                // symbols are positive (5.3)
    CHECK(simplified("sqrt(4*x^2*y)") == "2 x sqrt(y)");
    CHECK(simplified("sqrt((x - y)^2)") == "abs(x - y)"); // but a difference may not be
    CHECK(simplified("sqrt(x*(y - 1)*(y - 2))") == "sqrt(x) sqrt((y - 2) (y - 1))");
    CHECK(simplified("x^(2/3)") == "x^(2/3)");
}

TEST_CASE("Decimals stay decimals; fractions stay exact")
{
    CHECK(simplified("1/3 + 1/2") == "5/6");
    CHECK(simplified("1/3 + 0.5") == "0.833");
    CHECK(simplified("2.5*x") == "2.5 x");
    CHECK(simplified("x/2.5") == "0.4 x");
    CHECK(simplified("2.5*x + x/2") == "3 x"); // exact underneath: 5/2 + 1/2 is 3
    CHECK(simplified("0.1 + 0.2") == "0.3");
    CHECK(simplified("1.5*sqrt(2)") == "2.121");
    CHECK(simplified("sqrt(2.25)") == "1.5");
    CHECK(simplified("sin(0.5)") == "0.479");
    CHECK(simplified("1e-3*x") == "0.001 x");
}

TEST_CASE("Numbers too large to be exact become decimals")
{
    CHECK(simplified("10^30*x") == "1*10^30 x");
    CHECK(simplified("2^100") == "1.268*10^30");
    CHECK(simplified("123456789012345678901234567890") == "1.235*10^29");
}

TEST_CASE("Elementary functions at known values, and their symmetries")
{
    CHECK(simplified("sin(pi/6)") == "1/2");
    CHECK(simplified("cos(pi/4)") == "sqrt(2)/2");
    CHECK(simplified("sin(pi)") == "0");
    CHECK(simplified("cos(2*pi/3)") == "-(1/2)");
    CHECK(simplified("tan(pi/3)") == "sqrt(3)");
    CHECK(simplified("sin(7*pi/6)") == "-(1/2)");
    CHECK(simplified("sin(-x)") == "-sin(x)");
    CHECK(simplified("cos(-x)") == "cos(x)");
    CHECK(simplified("asin(1/2)") == "π/6");
    CHECK(simplified("atan(1)") == "π/4");
    CHECK(simplified("sin(1)") == "sin(1)");
}

TEST_CASE("Logarithms and exponentials undo each other")
{
    CHECK(simplified("ln(exp(x))") == "x");
    CHECK(simplified("exp(ln(x))") == "x");
    CHECK(simplified("exp(2*ln(x))") == "x^2");
    CHECK(simplified("ln(e)") == "1");
    CHECK(simplified("ln(1)") == "0");
    CHECK(simplified("log(1000)") == "3");
    CHECK(simplified("log(1/100)") == "-2");
    CHECK(simplified("exp(k*t)") == "e^(k t)");
    CHECK(simplified("exp(-k*t)") == "e^-(k t)");
    CHECK(simplified("abs(-x)") == "x");
}

TEST_CASE("Simplify: sin² + cos² is 1, and terms over one denominator combine")
{
    CHECK(simplified("sin(x)^2 + cos(x)^2") == "1");
    CHECK(simplified("2*a*sin(t)^2 + 2*a*cos(t)^2 + 1") == "2 a + 1");
    CHECK(simplified("sin(x)^2 + 2*cos(x)^2") == "sin(x)^2 + 2 cos(x)^2");
    CHECK(simplified("a/b + c/b") == "(a + c)/b");
    CHECK(simplified("a/b + c/b + 1") == "(a + c)/b + 1");
    CHECK(simplified("a/b + c/d") == "a/b + c/d");
}

TEST_CASE("Undefined functions are kept as calls")
{
    CHECK(simplified("f(x) + f(x)") == "2 f(x)");
    CHECK(simplified("max(a, b)") == "max(a, b)");
}

TEST_CASE("Errors name the node they come from")
{
    const MathNode zero = mathFromLinear("1/(x - x)");
    CHECK(errorNode(zero) == zero.id);

    const MathNode negative = mathFromLinear("a + sqrt(-4)");
    CHECK(errorNode(negative) == negative.children[1].id);

    const MathNode log = mathFromLinear("ln(0)");
    CHECK(errorNode(log) == log.id);

    const MathNode arity = mathFromLinear("sin(x, y)");
    CHECK(errorNode(arity) == arity.id);

    MathNode hole = mathFromLinear("x + 1");
    hole.children[1] = MathNode();
    CHECK(errorNode(hole) == hole.children[1].id);
}

TEST_CASE("A result's text parses back to the same expression")
{
    for (const char* source : {"w*L^4/(8*E*I)", "x^2 - 5*x + 6", "-(a + b)", "sqrt(8)*x", "a - b/c",
                               "P*x*(2*L - x)/(2*E*I)", "exp(-k*t)", "x^(-2/3)", "abs(x - y)", "3/(2*x)",
                               "sin(x)^2/cos(x)", "2.5*x", "pi*r^2"}) {
        const Expr e = simplify(parse(source));
        const std::string text = toLinear(show(e, mathFromLinear(source)));
        INFO(source << " -> " << text);
        CHECK(simplify(parse(text)) == e);
    }
}

namespace {

// Random expressions in x, y and z, as linear text.
struct RandomExpressions {
    std::mt19937 rng{16};

    int pick(int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); }
    double value() { return 0.5 + std::uniform_real_distribution<double>(0, 2.5)(rng); }

    std::string generate(int depth)
    {
        // No π: sin(π) is exactly 0 here but not numerically, so ln(sin(π)) would differ.
        static const std::vector<std::string> atoms = {"x", "y", "z", "2", "3", "1/2", "0.5", "x", "y", "z"};
        static const std::vector<std::string> exponents = {"2", "3", "-1", "1/2", "-1/2", "(1/3)", "0", "2.5"};
        static const std::vector<std::string> functions = {"sqrt", "sin", "cos", "ln", "exp", "abs"};
        if (depth == 0 || pick(4) == 0) {
            return atoms[static_cast<std::size_t>(pick(static_cast<int>(atoms.size())))];
        }
        switch (pick(7)) {
        case 0: return "(" + generate(depth - 1) + " + " + generate(depth - 1) + ")";
        case 1: return "(" + generate(depth - 1) + " - " + generate(depth - 1) + ")";
        case 2: return "(" + generate(depth - 1) + "*" + generate(depth - 1) + ")";
        case 3: return "(" + generate(depth - 1) + "/" + generate(depth - 1) + ")";
        case 4: return "(" + generate(depth - 1) + ")^" + exponents[static_cast<std::size_t>(pick(8))];
        case 5: return functions[static_cast<std::size_t>(pick(6))] + "(" + generate(depth - 1) + ")";
        default: return "-" + generate(depth - 1);
        }
    }
};

// An expression's value, shown to 12 places and evaluated numerically.
double valueOf(const Expr& e, const MathNode& from, const Environment& env)
{
    ExprDisplayOptions options = orderOf(from);
    options.format.decimals = 12;
    options.format.exponentialThreshold = 15;
    return evaluate(*lowerExpression(mathFromExpr(e, options)), env).value;
}

} // namespace

// Random expressions, checked against numeric evaluation: the symbolic result must
// have the same value as the expression typed, at random positive values.
TEST_CASE("Results have the same value as what was typed")
{
    RandomExpressions random;
    const auto generate = [&](int depth) { return random.generate(depth); };
    auto& rng = random.rng;

    int checked = 0;
    for (int run = 0; run < 3000; ++run) {
        const std::string source = generate(4);
        const MathNode tree = mathFromLinear(source);

        Environment env;
        env.define("x", Quantity::scalar(0.5 + std::uniform_real_distribution<double>(0, 2.5)(rng)));
        env.define("y", Quantity::scalar(0.5 + std::uniform_real_distribution<double>(0, 2.5)(rng)));
        env.define("z", Quantity::scalar(0.5 + std::uniform_real_distribution<double>(0, 2.5)(rng)));

        double expected = 0.0;
        try {
            expected = evaluate(*lowerExpression(tree), env).value;
        } catch (const Error&) {
            continue; // no value numerically (a division by zero, or a root of a negative)
        }
        if (!std::isfinite(expected) || std::abs(expected) > 1e12) {
            continue;
        }

        INFO(source);
        std::string text;
        double actual = 0.0;
        try {
            // Decimals to 12 places, so the shown result can be compared closely.
            ExprDisplayOptions options = orderOf(tree);
            options.format.decimals = 12;
            options.format.exponentialThreshold = 15;
            const MathNode result = mathFromExpr(simplify(exprFromMath(tree)), options);
            text = toLinear(result);
            actual = evaluate(*lowerExpression(result), env).value;
        } catch (const Error& e) {
            INFO("-> " << text << ": " << e.what());
            CHECK(false);
            continue;
        }
        INFO("-> " << text);
        CHECK(std::abs(actual - expected) <= 1e-6 * std::max(1.0, std::abs(expected)));
        ++checked;
    }
    CHECK(checked > 1000);
}

// expand and diff, checked numerically: an expansion has the expression's value, and a
// derivative matches the slope from a central difference.
TEST_CASE("Expansions and derivatives have the right values")
{
    RandomExpressions random;
    int checked = 0;
    for (int run = 0; run < 2000; ++run) {
        const std::string source = random.generate(3);
        const MathNode tree = mathFromLinear(source);
        const double x = random.value();
        const double y = random.value();
        const double h = 1e-5;
        const auto at = [&](double dx) {
            Environment env;
            env.define("x", Quantity::scalar(x + dx));
            env.define("y", Quantity::scalar(y));
            env.define("z", Quantity::scalar(1.3));
            return env;
        };
        double f = 0.0;
        double slope = 0.0;
        try {
            f = evaluate(*lowerExpression(tree), at(0)).value;
            slope = (evaluate(*lowerExpression(tree), at(h)).value - evaluate(*lowerExpression(tree), at(-h)).value)
                    / (2 * h);
        } catch (const Error&) {
            continue;
        }
        if (!std::isfinite(f) || !std::isfinite(slope) || std::abs(f) > 1e6 || std::abs(slope) > 1e6) {
            continue;
        }
        INFO(source);
        const Expr e = exprFromMath(tree);
        try {
            CHECK(std::abs(valueOf(expand(e), tree, at(0)) - f) <= 1e-6 * std::max(1.0, std::abs(f)));
            const Expr d = differentiate(e, "x");
            INFO("d/dx -> " << d.debugString());
            CHECK(std::abs(valueOf(d, tree, at(0)) - slope) <= 1e-4 * std::max(1.0, std::abs(slope)));
        } catch (const Error& err) {
            INFO(err.what());
            CHECK(false);
        }
        ++checked;
    }
    CHECK(checked > 500);
}
