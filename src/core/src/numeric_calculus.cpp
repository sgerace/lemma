#include "lemma/numeric_calculus.h"

#include "lemma/error.h"
#include "lemma/format.h"
#include "lemma/symbolic_ops.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace lemma {

namespace {

Error errorAt(const Node& node, const std::string& message)
{
    return Error(message, node.begin, node.end, node.origin);
}

using Bindings = std::vector<std::pair<std::string, Expr>>;

const Expr* bound(const Bindings& bindings, const std::string& name)
{
    for (auto it = bindings.rbegin(); it != bindings.rend(); ++it) {
        if (it->first == name) {
            return &it->second;
        }
    }
    return nullptr;
}

// An operand as an engine expression in the variables bound to it: an operator's
// variable, and in an inlined function, its parameters. What doesn't depend on those is
// evaluated numerically, as it would be anyway, and stands in as a constant, so units,
// definitions and closures mean what they do numerically.
class Operand {
public:
    explicit Operand(const Node& at) : at_(at) {}

    Expr lower(const Node& n, const Environment& env, const Bindings& bindings)
    {
        if (!dependsOn(n, env, bindings)) {
            return constant(evaluate(n, env));
        }
        switch (n.kind) {
        case Node::Kind::Number:
            break;
        case Node::Kind::Identifier:
            return *bound(bindings, n.name);
        case Node::Kind::Unary:
            return -lower(*n.args[0], env, bindings);
        case Node::Kind::Binary: {
            if (n.op == '=') {
                throw errorAt(n, "An equation (Ctrl+=) only has a meaning in a symbolic evaluation (→)");
            }
            const Expr a = lower(*n.args[0], env, bindings);
            const Expr b = lower(*n.args[1], env, bindings);
            switch (n.op) {
            case '+': return a + b;
            case '-': return a - b;
            case '*': return a * b;
            case '/': return a / b;
            case '^': return Expr::pow(a, b);
            default: break;
            }
            break;
        }
        case Node::Kind::Call:
            return call(n, env, bindings);
        }
        throw errorAt(n, "Unknown expression");
    }

    // The value of a result, with the variable `x` at `value`.
    Quantity value(const Expr& e, const std::string& x, const Quantity& at) const
    {
        switch (e.kind()) {
        case Expr::Kind::Number:
            return Quantity::scalar(e.number().toDouble());
        case Expr::Kind::Symbol: {
            const Symbol& s = e.symbol();
            if (s.kind != Symbol::Kind::Variable) {
                return {s.size, s.dimension}; // π, e, g, or a unit
            }
            if (s.name == x) {
                return at;
            }
            return constants_.at(s.name);
        }
        case Expr::Kind::Sum: {
            Quantity total = Quantity::scalar(e.constant().toDouble());
            bool first = e.constant().isZero();
            for (const ExprTerm& t : e.terms()) {
                Quantity term = value(t.expr, x, at);
                term.value *= t.coefficient.toDouble();
                if (first) {
                    total = term;
                    first = false;
                } else if (!(term.dim == total.dim)) {
                    throw errorAt(at_, "Units don't match: " + describeDimension(total.dim) + " and "
                                           + describeDimension(term.dim));
                } else {
                    total.value += term.value;
                }
            }
            return total;
        }
        case Expr::Kind::Product: {
            Quantity product = Quantity::scalar(e.coefficient().toDouble());
            for (const ExprFactor& f : e.factors()) {
                const Quantity base = value(f.base, x, at);
                const Quantity exponent = value(f.exponent, x, at);
                const Quantity factor = power(base, exponent);
                product = {product.value * factor.value, product.dim * factor.dim};
            }
            return product;
        }
        case Expr::Kind::Function: {
            const Quantity arg = value(e.argument(), x, at);
            if (e.function() == MathFunction::Abs) {
                return {std::abs(arg.value), arg.dim};
            }
            if (!arg.dim.isDimensionless()) {
                throw errorAt(at_, std::string(functionName(e.function())) + " needs a unitless argument (it has "
                                       + describeDimension(arg.dim) + ")");
            }
            const Expr applied = Expr::apply(e.function(), Expr(Number::approximate(arg.value)));
            if (!applied.isNumber()) {
                throw errorAt(at_, "The result is not a finite number");
            }
            return Quantity::scalar(applied.number().toDouble());
        }
        case Expr::Kind::Call:
            break;
        }
        throw errorAt(at_, "The result is not a finite number");
    }

private:
    // A value that doesn't depend on the bound variables. A unitless one that is a simple
    // fraction exactly is a number, so the engine's rules for powers apply (x^2); any
    // other is a name of its own, which keeps its units out of the engine.
    Expr constant(const Quantity& q)
    {
        if (q.dim.isDimensionless() && std::isfinite(q.value)) {
            for (std::int64_t d = 1; d <= 1000; ++d) {
                const double n = std::round(q.value * static_cast<double>(d));
                if (std::abs(n) < 1e15 && n / static_cast<double>(d) == q.value) {
                    return Expr(Number(Rational(static_cast<std::int64_t>(n), d)));
                }
            }
        }
        const std::string name = "#" + std::to_string(constants_.size()); // a name no one can type
        constants_.emplace(name, q);
        return Expr::variable(name, q.value > 0); // its sign is known; a variable's isn't
    }

    bool dependsOn(const Node& n, const Environment& env, const Bindings& bindings) const
    {
        switch (n.kind) {
        case Node::Kind::Number:
            return false;
        case Node::Kind::Identifier:
            return bound(bindings, n.name) != nullptr;
        case Node::Kind::Call:
            if (isOperator(n)) {
                // An inner operator's variable is its own in its operand, which shadows an
                // outer one; a derivative or indefinite integral is then taken at it.
                const std::string& y = n.args[1]->name;
                Bindings outer;
                std::copy_if(bindings.begin(), bindings.end(), std::back_inserter(outer),
                             [&](const auto& b) { return b.first != y; });
                const bool atVariable = n.args.size() <= 3 && bound(bindings, y);
                return atVariable || dependsOn(*n.args[0], env, outer)
                       || std::any_of(n.args.begin() + 2, n.args.end(),
                                      [&](const NodePtr& a) { return dependsOn(*a, env, bindings); });
            }
            [[fallthrough]];
        default:
            return std::any_of(n.args.begin(), n.args.end(),
                               [&](const NodePtr& a) { return dependsOn(*a, env, bindings); });
        }
    }

    static bool isOperator(const Node& n)
    {
        return (n.name == "diff" || n.name == "integrate") && n.args.size() >= 2
               && n.args[1]->kind == Node::Kind::Identifier;
    }

    Expr call(const Node& n, const Environment& env, const Bindings& bindings)
    {
        if (isOperator(n)) {
            return inner(n, env, bindings);
        }
        std::vector<Expr> args;
        for (const NodePtr& a : n.args) {
            args.push_back(lower(*a, env, bindings));
        }
        if (const Function* fn = env.function(n.name)) {
            // Inlined: its body, in the definitions above it, with its parameters bound.
            if (args.size() != fn->params.size()) {
                throw errorAt(n, n.name + " takes " + std::to_string(fn->params.size())
                                     + (fn->params.size() == 1 ? " argument" : " arguments"));
            }
            Bindings params;
            for (std::size_t i = 0; i < args.size(); ++i) {
                params.emplace_back(fn->params[i], args[i]);
            }
            try {
                return lower(*fn->body, *fn->closure, params);
            } catch (const Error& e) {
                throw errorAt(n, "In " + n.name + ": " + e.what());
            }
        }
        if (const Quantity* q = env.variable(n.name); q && args.size() == 1) {
            return constant(*q) * args[0]; // "M (h/2)" on a value is a product
        }
        if (args.size() == 1) {
            if (n.name == "sqrt") {
                return Expr::pow(args[0], Expr(Number(Rational(1, 2))));
            }
            if (n.name == "exp") {
                return Expr::pow(Expr::e(), args[0]);
            }
            if (const auto f = functionNamed(n.name)) {
                return Expr::apply(*f, args[0]);
            }
        }
        // min and max: no rules for the engine. An integral is then computed numerically.
        throw BeyondScope("Couldn't differentiate " + n.name);
    }

    // An operator inside the operand, computed symbolically in turn.
    Expr inner(const Node& n, const Environment& env, const Bindings& bindings)
    {
        const std::string& y = n.args[1]->name;
        Bindings scope = bindings;
        const Expr variable = Expr::variable(y, false);
        scope.emplace_back(y, variable);
        const Expr f = lower(*n.args[0], env, scope);
        try {
            if (n.name == "diff") {
                const Expr d = differentiate(f, y, order(n));
                return replaceSymbols(d, [&](const Symbol& s) -> std::optional<Expr> {
                    return s.kind == Symbol::Kind::Variable && s.name == y ? std::optional(lower(*n.args[1], env, bindings))
                                                                           : std::nullopt;
                });
            }
            if (n.args.size() == 4) {
                return integrate(f, y, lower(*n.args[2], env, bindings), lower(*n.args[3], env, bindings));
            }
            const Expr i = integrate(f, y);
            return replaceSymbols(i, [&](const Symbol& s) -> std::optional<Expr> {
                return s.kind == Symbol::Kind::Variable && s.name == y ? std::optional(lower(*n.args[1], env, bindings))
                                                                       : std::nullopt;
            });
        } catch (const BeyondScope& beyond) {
            throw errorAt(n, beyond.what());
        }
    }

    Quantity power(const Quantity& base, const Quantity& exponent) const
    {
        if (!exponent.dim.isDimensionless()) {
            throw errorAt(at_, "An exponent can't have units (it has " + describeDimension(exponent.dim) + ")");
        }
        if (base.dim.isDimensionless()) {
            return Quantity::scalar(std::pow(base.value, exponent.value));
        }
        const auto rational = Rational::fromDouble(exponent.value);
        if (!rational) {
            throw errorAt(at_, "A quantity with units can only be raised to a simple fraction");
        }
        return {std::pow(base.value, exponent.value), base.dim.pow(*rational)};
    }

public:
    // A derivative's order: a whole number from 1 to 99.
    static int order(const Node& call)
    {
        if (call.args.size() < 3) {
            return 1;
        }
        const Node& n = *call.args[2];
        if (n.kind != Node::Kind::Number || n.number != std::floor(n.number) || n.number < 1 || n.number > 99) {
            throw errorAt(n, "The order is a whole number from 1 to 99");
        }
        return static_cast<int>(n.number);
    }

private:
    const Node& at_;
    std::unordered_map<std::string, Quantity> constants_;
};

Quantity finite(Quantity q, const Node& at)
{
    if (!std::isfinite(q.value)) {
        throw errorAt(at, "The result is not a finite number");
    }
    return q;
}

// ---------------------------------------------------------------------------
// Quadrature

// The 15-point Kronrod rule and the 7-point Gauss rule within it, on [−1, 1].
constexpr double kNodes[8] = {0.991455371120812639, 0.949107912342758525, 0.864864423359769073,
                              0.741531185599394440, 0.586087235467691130, 0.405845151377397167,
                              0.207784955007898468, 0.000000000000000000};
constexpr double kKronrod[8] = {0.022935322010529225, 0.063092092629978553, 0.104790010322250184,
                                0.140653259715525919, 0.169004726639267903, 0.190350578064785410,
                                0.204432940075298892, 0.209482141084727828};
constexpr double kGauss[4] = {0.129484966168869693, 0.279705391489276668, 0.381830050505118945,
                              0.417959183673469388}; // at kNodes[1], [3], [5], [7]

struct Segment {
    double a = 0.0;
    double b = 0.0;
    double value = 0.0;
    double error = 0.0;
};

// ∫ₐᵇ f by adaptive Gauss–Kronrod (7 and 15 points), bisecting the segment with the
// largest error until the estimate is within a relative 10⁻¹⁰. False if it doesn't get
// there, or meets a value that isn't finite.
template <typename F>
bool gaussKronrod(F&& f, double a, double b, double& result)
{
    const auto rule = [&](double lo, double hi) {
        const double centre = (lo + hi) / 2;
        const double half = (hi - lo) / 2;
        double kronrod = 0.0;
        double gauss = 0.0;
        for (int i = 0; i < 8; ++i) {
            const double dx = half * kNodes[i];
            const double sum = i == 7 ? f(centre) : f(centre - dx) + f(centre + dx);
            kronrod += kKronrod[i] * sum;
            if (i % 2 == 1) {
                gauss += kGauss[i / 2] * sum;
            }
        }
        return Segment{lo, hi, kronrod * half, std::abs((kronrod - gauss) * half)};
    };
    std::vector<Segment> segments{rule(a, b)};
    for (int step = 0; step < 2000; ++step) {
        double value = 0.0;
        double error = 0.0;
        for (const Segment& s : segments) {
            value += s.value;
            error += s.error;
        }
        if (!std::isfinite(value) || !std::isfinite(error)) {
            return false;
        }
        if (error <= std::max(1e-10 * std::abs(value), 1e-300)) {
            result = value;
            return true;
        }
        const auto worst = std::max_element(segments.begin(), segments.end(),
                                            [](const Segment& p, const Segment& q) { return p.error < q.error; });
        const Segment s = *worst;
        const double middle = (s.a + s.b) / 2;
        if (middle <= s.a || middle >= s.b) {
            return false; // no room left to bisect: a singularity
        }
        *worst = rule(s.a, middle);
        segments.push_back(rule(middle, s.b));
    }
    return false;
}

} // namespace

Quantity evaluateDerivative(const Node& call, const Environment& env)
{
    const std::string& x = call.args[1]->name;
    const int order = Operand::order(call);
    Operand operand(call);
    Expr d;
    try {
        d = differentiate(operand.lower(*call.args[0], env, {{x, Expr::variable(x, false)}}), x, order);
    } catch (const BeyondScope& beyond) {
        throw errorAt(call, beyond.what());
    }
    // Then taken where the operator stands, at x's value there (15.1).
    const Quantity at = evaluate(*call.args[1], env);
    return finite(operand.value(d, x, at), call);
}

Quantity evaluateIntegral(const Node& call, const Environment& env)
{
    const std::string& x = call.args[1]->name;
    const Expr variable = Expr::variable(x, false);
    if (call.args.size() == 2) {
        // Indefinite: the engine's antiderivative, at x's value where it stands.
        Operand operand(call);
        Expr antiderivative;
        try {
            antiderivative = integrate(operand.lower(*call.args[0], env, {{x, variable}}), x);
        } catch (const BeyondScope&) {
            throw errorAt(call, "Couldn't integrate this; give it limits to integrate numerically");
        }
        return finite(operand.value(antiderivative, x, evaluate(*call.args[1], env)), call);
    }

    Quantity from = evaluate(*call.args[2], env);
    Quantity to = evaluate(*call.args[3], env);
    // A limit of exactly 0 takes the other's units: ∫₀ᴸ with L a length (15.1).
    if (from.value == 0.0 && from.dim.isDimensionless()) {
        from.dim = to.dim;
    } else if (to.value == 0.0 && to.dim.isDimensionless()) {
        to.dim = from.dim;
    }
    if (!(from.dim == to.dim)) {
        throw errorAt(call, "Units don't match: the limits are " + describeDimension(from.dim) + " and "
                                + describeDimension(to.dim));
    }

    // Exactly, where the engine can. Errors in the integrand itself (a name with no
    // value) are reported as they are.
    try {
        Operand operand(call);
        const Expr antiderivative = integrate(operand.lower(*call.args[0], env, {{x, variable}}), x);
        try {
            const Quantity upper = operand.value(antiderivative, x, to);
            const Quantity lower = operand.value(antiderivative, x, from);
            if (std::isfinite(upper.value) && std::isfinite(lower.value) && upper.dim == lower.dim) {
                return {upper.value - lower.value, upper.dim};
            }
        } catch (const Error&) {
            // The antiderivative has no value at a limit: numerically instead.
        }
    } catch (const BeyondScope&) {
    }

    // Numerically: the integrand at points of the interval, its units the same throughout.
    Environment local(&env);
    std::optional<Dimension> dim;
    const auto integrand = [&](double t) {
        local.define(x, {t, from.dim});
        const Quantity q = evaluate(*call.args[0], local);
        if (dim && !(q.dim == *dim)) {
            throw errorAt(call, "Units don't match: the integrand is " + describeDimension(*dim) + " and "
                                    + describeDimension(q.dim));
        }
        dim = q.dim;
        return q.value;
    };
    double value = 0.0;
    if (from.value == to.value) {
        return {0.0, from.dim};
    }
    try {
        if (!gaussKronrod(integrand, from.value, to.value, value)) {
            throw errorAt(call, "This integral doesn't converge");
        }
    } catch (const Error& e) {
        // A point where the integrand has no value, like 1/x at 0; other errors as they are.
        const std::string what = e.what();
        if (what == "Division by zero" || what == "The result is not a finite number") {
            throw errorAt(call, "This integral doesn't converge");
        }
        throw;
    }
    return {value, (dim ? *dim : Dimension()) * from.dim};
}

} // namespace lemma
