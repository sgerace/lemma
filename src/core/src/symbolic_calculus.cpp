// `solve` and `integrate` (docs/symbolic-spec.md, sections 9.5 and 9.7), and the
// in-house back end that offers them with `factor`.

#include "lemma/symbolic_ops.h"

#include "lemma/error.h"
#include "symbolic_internal.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace lemma {

namespace {

constexpr int kMaxDepth = 8;

const Expr kHalf(Number(Rational(1, 2)));

// The variable `name` as it appears in `e` (positive, or real), or a positive one.
Expr variableIn(const Expr& e, const std::string& name)
{
    std::optional<Expr> found;
    replaceSymbols(e, [&](const Symbol& s) -> std::optional<Expr> {
        if (!found && s.kind == Symbol::Kind::Variable && s.name == name) {
            found = Expr::symbol(s);
        }
        return std::nullopt;
    });
    return found.value_or(Expr::variable(name));
}

std::vector<ExprFactor> factorsOf(const Expr& e)
{
    if (e.kind() == Expr::Kind::Product) {
        return e.factors();
    }
    if (e.isNumber()) {
        return {};
    }
    return {{e, Expr(1)}};
}

std::vector<Expr> termsOf(const Expr& e)
{
    if (e.kind() != Expr::Kind::Sum) {
        return {e};
    }
    std::vector<Expr> out;
    if (!e.constant().isZero()) {
        out.emplace_back(e.constant());
    }
    for (const ExprTerm& t : e.terms()) {
        out.push_back(Expr(t.coefficient) * t.expr);
    }
    return out;
}

// a and b, if e is a x + b with a and b free of x.
std::optional<std::pair<Expr, Expr>> linearIn(const Expr& e, const Expr& x)
{
    const std::string& name = x.symbol().name;
    const Expr a = differentiate(e, name);
    if (dependsOn(a, name) || a == Expr(0)) {
        return std::nullopt;
    }
    const Expr b = simplify(e - a * x);
    if (dependsOn(b, name)) {
        return std::nullopt;
    }
    return std::pair{a, b};
}

// The value of an expression with no variables in it, as a double: cos(3) is −0.99.
std::optional<double> numericValue(const Expr& e)
{
    switch (e.kind()) {
    case Expr::Kind::Number:
        return e.number().toDouble();
    case Expr::Kind::Symbol:
        return e.symbol().kind == Symbol::Kind::Variable ? std::nullopt : std::optional(e.symbol().size);
    case Expr::Kind::Sum: {
        double v = e.constant().toDouble();
        for (const ExprTerm& t : e.terms()) {
            const auto x = numericValue(t.expr);
            if (!x) {
                return std::nullopt;
            }
            v += t.coefficient.toDouble() * *x;
        }
        return v;
    }
    case Expr::Kind::Product: {
        double v = e.coefficient().toDouble();
        for (const ExprFactor& f : e.factors()) {
            const auto b = numericValue(f.base);
            const auto x = numericValue(f.exponent);
            if (!b || !x) {
                return std::nullopt;
            }
            v *= std::pow(*b, *x);
        }
        return v;
    }
    case Expr::Kind::Function: {
        const auto x = numericValue(e.argument());
        if (!x) {
            return std::nullopt;
        }
        try {
            return Expr::apply(e.function(), Expr(Number::approximate(*x))).number().toDouble();
        } catch (const Error&) {
            return std::nullopt;
        }
    }
    case Expr::Kind::Call:
        return std::nullopt;
    }
    return std::nullopt;
}

// True if e can't be zero: a nonzero number, positive, negative, or with no variables
// and a value away from zero.
bool clearlyNonzero(const Expr& e)
{
    if ((e.isNumber() && !e.number().isZero()) || e.isPositive() || (-e).isPositive()) {
        return true;
    }
    const auto v = numericValue(e);
    return v && std::isfinite(*v) && std::abs(*v) > 1e-9;
}

// f with x replaced by the value: does it put a nonzero value back? Undoing a root, an
// abs or a fractional power can give values that don't solve the original: √x = −3
// gives x = 9, which puts 6 back. No value there counts too.
bool fails(const Expr& f, const std::string& x, const Expr& value)
{
    try {
        const Expr back = simplify(replaceSymbols(f, [&](const Symbol& s) -> std::optional<Expr> {
            return s.kind == Symbol::Kind::Variable && s.name == x ? std::optional(value) : std::nullopt;
        }));
        return clearlyNonzero(back);
    } catch (const Error&) {
        return true;
    }
}

bool isWhole(const Expr& e)
{
    return e.isNumber() && e.number().isInteger() && !e.number().decimal();
}

// ---------------------------------------------------------------------------
// solve

class Solver {
public:
    Solver(const Expr& x) : x_(x), name_(x.symbol().name) {}

    // The solutions of f = 0, or nothing beyond the scope.
    std::optional<std::vector<Expr>> zeros(const Expr& given, int depth) const
    {
        if (depth > kMaxDepth) {
            return std::nullopt;
        }
        const Expr f = simplify(given);
        if (!dependsOn(f, name_)) {
            return f == Expr(0) ? std::nullopt : std::optional(std::vector<Expr>{});
        }
        if (f.kind() == Expr::Kind::Product) {
            // Each factor that depends on x may be zero; a denominator never is.
            std::vector<Expr> all;
            bool any = false;
            for (const ExprFactor& factor : f.factors()) {
                if (!dependsOn(factor.base, name_) && !dependsOn(factor.exponent, name_)) {
                    continue;
                }
                if (factor.exponent.isNumber() && !factor.exponent.number().isNegative()) {
                    const auto some = zeros(factor.base, depth + 1);
                    if (!some) {
                        return std::nullopt;
                    }
                    for (const Expr& s : *some) {
                        if (!fails(factor.base, name_, s)) {
                            all.push_back(s); // each factor's own, checked against it
                        }
                    }
                    any = true;
                } else if (!(factor.exponent.isNumber() && factor.exponent.number().isNegative())) {
                    return invert(f, Expr(0), depth + 1); // x in an exponent: e^(k x) never is zero
                }
            }
            return any ? std::optional(all) : std::optional(std::vector<Expr>{});
        }
        if (f.kind() == Expr::Kind::Sum) {
            if (auto cleared = clearDenominators(f)) {
                return zeros(*cleared, depth + 1);
            }
            if (auto polynomial = polynomialZeros(f, depth)) {
                return polynomial;
            }
            if (auto isolated = isolate(f, depth)) {
                return isolated;
            }
            return trigZeros(f, depth);
        }
        return invert(f, Expr(0), depth + 1);
    }

private:
    // A sum with x in a denominator, multiplied through by the denominators: 1/x − a
    // is solved as 1 − a x.
    std::optional<Expr> clearDenominators(const Expr& f) const
    {
        std::vector<Expr> denominators;
        for (const Expr& term : termsOf(f)) {
            for (const ExprFactor& factor : factorsOf(term)) {
                if (dependsOn(factor.base, name_) && factor.exponent.isNumber() && factor.exponent.number().isNegative()) {
                    const Expr d = Expr::pow(factor.base, -factor.exponent);
                    if (std::find(denominators.begin(), denominators.end(), d) == denominators.end()) {
                        denominators.push_back(d);
                    }
                }
            }
        }
        if (denominators.empty()) {
            return std::nullopt;
        }
        try {
            return expand(f * Expr::product(denominators), 500);
        } catch (const Error&) {
            return std::nullopt;
        }
    }

    // A polynomial in x: factored when it factors, then by degree.
    std::optional<std::vector<Expr>> polynomialZeros(const Expr& f, int depth) const
    {
        Expr expanded = f;
        try {
            expanded = expand(f, 500);
        } catch (const Error&) {
            return std::nullopt;
        }
        if (expanded.kind() != Expr::Kind::Sum) {
            return zeros(expanded, depth + 1);
        }
        const auto c = detail::coefficientsIn(expanded, x_);
        if (!c || c->size() < 2) {
            return std::nullopt;
        }
        const std::size_t degree = c->size() - 1;
        std::size_t nonzero = 0;
        for (const Expr& k : *c) {
            nonzero += k == Expr(0) ? 0 : 1;
        }
        if (degree == 1) {
            return std::vector<Expr>{simplify(-(*c)[0] / (*c)[1])};
        }
        if (nonzero == 2 && !((*c)[0] == Expr(0))) {
            return isolate(expanded, depth); // a x^n + b: a root, as F = k d² gives d = ±√(F/k)
        }
        const Expr factored = factor(expanded);
        if (factored.kind() == Expr::Kind::Product) {
            const auto sums = std::count_if(factored.factors().begin(), factored.factors().end(), [&](const ExprFactor& p) {
                return dependsOn(p.base, name_);
            });
            if (sums > 1 || factored.factors().size() > 1) {
                return zeros(factored, depth + 1);
            }
        }
        if (degree == 2) {
            // The quadratic formula; no real roots, no solutions.
            const Expr& a = (*c)[2];
            const Expr& b = (*c)[1];
            const Expr& k = (*c)[0];
            const Expr discriminant = simplify(b * b - Expr(4) * a * k);
            if (discriminant.isNumber() && discriminant.number().isNegative()) {
                return std::vector<Expr>{};
            }
            if (discriminant == Expr(0)) {
                return std::vector<Expr>{simplify(-b / (Expr(2) * a))};
            }
            const Expr root = Expr::pow(discriminant, kHalf);
            return std::vector<Expr>{simplify((-b + root) / (Expr(2) * a)), simplify((-b - root) / (Expr(2) * a))};
        }
        return std::nullopt;
    }

    // f = 0 with x in one term only: that term is the rest, moved across and undone.
    std::optional<std::vector<Expr>> isolate(const Expr& f, int depth) const
    {
        std::vector<Expr> with;
        std::vector<Expr> without;
        for (const Expr& term : termsOf(f)) {
            (dependsOn(term, name_) ? with : without).push_back(term);
        }
        if (with.size() != 1) {
            return std::nullopt;
        }
        return invert(with[0], -Expr::sum(without), depth + 1);
    }

    // A sum of sines and cosines of one argument g, with x nowhere else, rewritten so x
    // is in one function: a sin g + b cos g is tan g = −b/a, and one of degree two goes
    // to double angles, p + q sin 2g + r cos 2g, then one of sin, cos or tan of 2g.
    std::optional<std::vector<Expr>> trigZeros(const Expr& f, int depth) const
    {
        const Expr s = Expr::variable("sin ·", false);
        const Expr c = Expr::variable("cos ·", false);
        std::optional<Expr> g;
        bool one = true; // a single argument
        const std::function<Expr(const Expr&)> swap = [&](const Expr& e) -> Expr {
            switch (e.kind()) {
            case Expr::Kind::Sum: {
                std::vector<Expr> parts{Expr(e.constant())};
                for (const ExprTerm& t : e.terms()) {
                    parts.push_back(Expr(t.coefficient) * swap(t.expr));
                }
                return Expr::sum(parts);
            }
            case Expr::Kind::Product: {
                std::vector<Expr> parts{Expr(e.coefficient())};
                for (const ExprFactor& p : e.factors()) {
                    parts.push_back(Expr::pow(swap(p.base), swap(p.exponent)));
                }
                return Expr::product(parts);
            }
            case Expr::Kind::Function:
                if ((e.function() == MathFunction::Sin || e.function() == MathFunction::Cos)
                    && dependsOn(e.argument(), name_)) {
                    one = one && (!g || *g == e.argument());
                    g = g.value_or(e.argument());
                    return e.function() == MathFunction::Sin ? s : c;
                }
                return e;
            default:
                return e;
            }
        };
        Expr p = swap(f);
        if (!g || !one || dependsOn(p, name_)) {
            return std::nullopt;
        }
        std::vector<std::vector<Expr>> k(3, std::vector<Expr>(3)); // k[i][j]: of sin^i cos^j, degree two at most
        try {
            for (const Expr& term : termsOf(expand(p, 500))) {
                std::size_t i = 0;
                std::size_t j = 0;
                Expr rest = term;
                for (const ExprFactor& factor : factorsOf(term)) {
                    if (factor.base == s || factor.base == c) {
                        if (!isWhole(factor.exponent) || factor.exponent.number().exact().num() > 2) {
                            return std::nullopt;
                        }
                        (factor.base == s ? i : j) = static_cast<std::size_t>(factor.exponent.number().exact().num());
                        rest = rest / Expr::pow(factor.base, factor.exponent);
                    }
                }
                if (i + j > 2 || dependsOn(rest, s.symbol().name) || dependsOn(rest, c.symbol().name)) {
                    return std::nullopt;
                }
                k[i][j] = k[i][j] + rest;
            }
        } catch (const Error&) {
            return std::nullopt;
        }
        const auto at = [&](std::size_t i, std::size_t j) { return simplify(k[i][j]); };
        const bool first = !(at(1, 0) == Expr(0)) || !(at(0, 1) == Expr(0));
        const bool second = !(at(2, 0) == Expr(0)) || !(at(1, 1) == Expr(0)) || !(at(0, 2) == Expr(0));
        Expr h = *g;
        Expr q; // of sin h
        Expr r; // of cos h
        if (first && !second && at(0, 0) == Expr(0)) {
            p = Expr(0);
            q = at(1, 0);
            r = at(0, 1);
        } else if (second && !first) {
            // sin² = (1 − cos 2g)/2, cos² = (1 + cos 2g)/2, sin cos = (sin 2g)/2
            h = Expr(2) * *g;
            p = simplify(at(0, 0) + (at(2, 0) + at(0, 2)) * kHalf);
            q = simplify(at(1, 1) * kHalf);
            r = simplify((at(0, 2) - at(2, 0)) * kHalf);
        } else {
            return std::nullopt;
        }
        if (q == Expr(0) && r == Expr(0)) {
            return zeros(p, depth + 1);
        }
        if (r == Expr(0)) {
            return invert(Expr::apply(MathFunction::Sin, h), -p / q, depth + 1);
        }
        if (q == Expr(0)) {
            return invert(Expr::apply(MathFunction::Cos, h), -p / r, depth + 1);
        }
        if (p == Expr(0)) {
            // Where cos h is zero, sin h isn't, so dividing by it loses nothing.
            return invert(Expr::apply(MathFunction::Tan, h), -r / q, depth + 1);
        }
        return std::nullopt;
    }

    // The solutions of u = value, undoing what's done to x in u. A value with no real
    // inverse (ln of a negative, asin of 2) means none on this branch.
    std::optional<std::vector<Expr>> invert(const Expr& u, const Expr& value, int depth) const
    {
        try {
            return undo(u, value, depth);
        } catch (const Error&) {
            return std::vector<Expr>{};
        }
    }

    std::optional<std::vector<Expr>> undo(const Expr& u, const Expr& value, int depth) const
    {
        if (depth > kMaxDepth) {
            return std::nullopt;
        }
        if (u == x_) {
            return std::vector<Expr>{simplify(value)};
        }
        const auto then = [&](const Expr& inner, const Expr& v) { return invert(inner, simplify(v), depth + 1); };
        switch (u.kind()) {
        case Expr::Kind::Sum:
            return zeros(u - value, depth + 1);
        case Expr::Kind::Product: {
            std::vector<ExprFactor> dependent;
            std::vector<Expr> constant{Expr(u.coefficient())};
            for (const ExprFactor& f : u.factors()) {
                if (dependsOn(f.base, name_) || dependsOn(f.exponent, name_)) {
                    dependent.push_back(f);
                } else {
                    constant.push_back(Expr::pow(f.base, f.exponent));
                }
            }
            if (dependent.size() != 1) {
                return zeros(u - value, depth + 1); // several: as a sum, if it is one
            }
            const Expr v = value / Expr::product(constant);
            const ExprFactor& f = dependent[0];
            if (!dependsOn(f.exponent, name_)) {
                if (f.exponent == Expr(1)) {
                    return then(f.base, v);
                }
                // A power: its root, both signs for an even power (5.3).
                const Expr root = Expr::pow(v, Expr(1) / f.exponent);
                const bool even = isWhole(f.exponent) && f.exponent.number().exact().num() % 2 == 0;
                std::vector<Expr> all;
                for (const Expr& r : even ? std::vector<Expr>{root, -root} : std::vector<Expr>{root}) {
                    const auto some = then(f.base, r);
                    if (!some) {
                        return std::nullopt;
                    }
                    all.insert(all.end(), some->begin(), some->end());
                }
                return all;
            }
            if (!dependsOn(f.base, name_)) {
                // aˣ = v: x = ln(v)/ln(a)
                return then(f.exponent, Expr::apply(MathFunction::Ln, v) / Expr::apply(MathFunction::Ln, f.base));
            }
            return std::nullopt;
        }
        case Expr::Kind::Function: {
            const Expr& g = u.argument();
            switch (u.function()) {
            case MathFunction::Ln: return then(g, Expr::pow(Expr::e(), value));
            case MathFunction::Log: return then(g, Expr::pow(Expr(10), value));
            case MathFunction::Sin: return then(g, Expr::apply(MathFunction::Asin, value));
            case MathFunction::Cos: return then(g, Expr::apply(MathFunction::Acos, value));
            case MathFunction::Tan: return then(g, Expr::apply(MathFunction::Atan, value));
            case MathFunction::Asin: return then(g, Expr::apply(MathFunction::Sin, value));
            case MathFunction::Acos: return then(g, Expr::apply(MathFunction::Cos, value));
            case MathFunction::Atan: return then(g, Expr::apply(MathFunction::Tan, value));
            case MathFunction::Abs: {
                std::vector<Expr> all;
                for (const Expr& v : {value, -value}) {
                    const auto some = then(g, v);
                    if (!some) {
                        return std::nullopt;
                    }
                    all.insert(all.end(), some->begin(), some->end());
                }
                return all;
            }
            }
            return std::nullopt;
        }
        default:
            return std::nullopt;
        }
    }

    Expr x_;
    std::string name_;
};

std::vector<Expr> distinct(const std::vector<Expr>& solutions)
{
    std::vector<Expr> out;
    for (const Expr& s : solutions) {
        if (std::find(out.begin(), out.end(), s) == out.end()) {
            out.push_back(s);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// integrate

class Integrator {
public:
    explicit Integrator(const Expr& x) : x_(x), name_(x.symbol().name) {}

    std::optional<Expr> antiderivative(const Expr& e, int depth) const
    {
        if (depth > kMaxDepth) {
            return std::nullopt;
        }
        if (!dependsOn(e, name_)) {
            return e * x_;
        }
        switch (e.kind()) {
        case Expr::Kind::Sum: {
            // Linearity.
            std::vector<Expr> parts;
            for (const Expr& term : termsOf(e)) {
                const auto a = antiderivative(term, depth + 1);
                if (!a) {
                    return std::nullopt;
                }
                parts.push_back(*a);
            }
            return Expr::sum(parts);
        }
        case Expr::Kind::Symbol:
        case Expr::Kind::Function:
            return single({e, Expr(1)});
        case Expr::Kind::Product:
            return product(e, depth);
        default:
            return std::nullopt;
        }
    }

private:
    // ln|u|, or ln(u) for a u known to be positive.
    static Expr lnOf(const Expr& u)
    {
        return Expr::apply(MathFunction::Ln, u.isPositive() ? u : Expr::apply(MathFunction::Abs, u));
    }

    // One factor, base^exponent: a power of x or of a x + b, an exponential of one, or
    // an elementary function of one.
    std::optional<Expr> single(const ExprFactor& f) const
    {
        const Expr& base = f.base;
        const Expr& exponent = f.exponent;
        if (!dependsOn(exponent, name_) && exponent.isNumber()) {
            if (const auto ab = linearIn(base, x_)) {
                const Expr& a = ab->first;
                if (exponent == Expr(-1)) {
                    return lnOf(base) / a; // ∫ dx/(a x + b) = ln|a x + b|/a
                }
                const Expr n1 = exponent + Expr(1);
                return Expr::pow(base, n1) / (a * n1);
            }
            if (exponent == Expr(1) && base.kind() == Expr::Kind::Function) {
                const auto ab = linearIn(base.argument(), x_);
                if (!ab) {
                    return std::nullopt;
                }
                const Expr& u = base.argument();
                const Expr& a = ab->first;
                switch (base.function()) {
                case MathFunction::Sin: return -Expr::apply(MathFunction::Cos, u) / a;
                case MathFunction::Cos: return Expr::apply(MathFunction::Sin, u) / a;
                case MathFunction::Tan: return -lnOf(Expr::apply(MathFunction::Cos, u)) / a;
                case MathFunction::Ln: return (u * Expr::apply(MathFunction::Ln, u) - u) / a;
                default: return std::nullopt;
                }
            }
            return std::nullopt;
        }
        if (!dependsOn(base, name_)) {
            // b^(a x + c) / (a ln b); for e, ln e is 1.
            if (const auto ab = linearIn(exponent, x_)) {
                return Expr::pow(base, exponent) / (ab->first * Expr::apply(MathFunction::Ln, base));
            }
        }
        return std::nullopt;
    }

    static bool isPolynomialFactor(const ExprFactor& f, const std::string& x)
    {
        if (!isWhole(f.exponent) || f.exponent.number().isNegative()) {
            return false;
        }
        return f.base.kind() == Expr::Kind::Symbol
               || (f.base.kind() == Expr::Kind::Sum && detail::coefficientsIn(f.base, Expr::variable(x)).has_value());
    }

    // sin, cos and exp of a linear argument: what integration by parts integrates.
    bool isPartsFactor(const ExprFactor& f) const
    {
        if (f.exponent == Expr(1) && f.base.kind() == Expr::Kind::Function) {
            const MathFunction g = f.base.function();
            return (g == MathFunction::Sin || g == MathFunction::Cos) && linearIn(f.base.argument(), x_);
        }
        return !dependsOn(f.base, name_) && linearIn(f.exponent, x_).has_value();
    }

    std::optional<Expr> product(const Expr& e, int depth) const
    {
        std::vector<Expr> constant{Expr(e.coefficient())};
        std::vector<ExprFactor> dependent;
        for (const ExprFactor& f : e.factors()) {
            if (dependsOn(f.base, name_) || dependsOn(f.exponent, name_)) {
                dependent.push_back(f);
            } else {
                constant.push_back(Expr::pow(f.base, f.exponent));
            }
        }
        const Expr c = Expr::product(constant);
        if (dependent.size() == 1) {
            if (auto a = single(dependent[0])) {
                return c * *a;
            }
        }

        // By parts, for a polynomial times sin, cos or exp of a linear argument:
        // ∫ p g = p G − ∫ p′ G.
        const auto parts = std::find_if(dependent.begin(), dependent.end(), [&](const ExprFactor& f) { return isPartsFactor(f); });
        if (parts != dependent.end() && std::count_if(dependent.begin(), dependent.end(), [&](const ExprFactor& f) {
                                            return isPartsFactor(f);
                                        }) == 1) {
            std::vector<Expr> polynomial;
            bool all = true;
            for (const ExprFactor& f : dependent) {
                if (&f != &*parts) {
                    all = all && isPolynomialFactor(f, name_);
                    polynomial.push_back(Expr::pow(f.base, f.exponent));
                }
            }
            if (all) {
                const Expr p = Expr::product(polynomial);
                const auto g = single(*parts);
                if (g) {
                    const auto rest = antiderivative(simplify(differentiate(p, name_) * *g), depth + 1);
                    if (rest) {
                        return c * (p * *g - *rest);
                    }
                }
            }
        }

        // A polynomial over a power of one linear factor: in u = a x + b.
        if (auto r = overLinear(dependent, depth)) {
            return c * *r;
        }

        // Otherwise, multiplied out, if that changes it: x (x + 1) is x² + x.
        try {
            const Expr expanded = expand(e, 200);
            if (!(expanded == e)) {
                return antiderivative(expanded, depth + 1);
            }
        } catch (const Error&) {
        }
        return std::nullopt;
    }

    // ∫ p(x)/(a x + b)ⁿ: with u = a x + b, a sum of powers of u.
    std::optional<Expr> overLinear(const std::vector<ExprFactor>& dependent, int depth) const
    {
        std::optional<std::pair<Expr, Expr>> linear;
        Expr denominator;
        std::vector<Expr> numerator;
        for (const ExprFactor& f : dependent) {
            if (f.exponent.isNumber() && f.exponent.number().isNegative() && isWhole(f.exponent)) {
                if (linear) {
                    return std::nullopt;
                }
                linear = linearIn(f.base, x_);
                if (!linear) {
                    return std::nullopt;
                }
                denominator = f.base;
            } else if (isPolynomialFactor(f, name_)) {
                numerator.push_back(Expr::pow(f.base, f.exponent));
            } else {
                return std::nullopt;
            }
        }
        if (!linear || numerator.empty()) {
            return std::nullopt;
        }
        const auto [a, b] = *linear;
        const Expr u = Expr::variable("\x01u"); // a name no one can type
        std::vector<Expr> all = numerator;
        for (const ExprFactor& f : dependent) {
            if (f.base == denominator) {
                all.push_back(Expr::pow(u, f.exponent));
            }
        }
        const Expr inU = replaceSymbols(Expr::product(all), [&](const Symbol& s) -> std::optional<Expr> {
            return s.kind == Symbol::Kind::Variable && s.name == name_ ? std::optional((u - b) / a) : std::nullopt;
        });
        try {
            const auto antiInU = Integrator(u).antiderivative(expand(inU, 200), depth + 1);
            if (!antiInU) {
                return std::nullopt;
            }
            return replaceSymbols(*antiInU, [&](const Symbol& s) -> std::optional<Expr> {
                return s.name == "\x01u" ? std::optional(denominator) : std::nullopt;
            }) / a;
        } catch (const Error&) {
            return std::nullopt;
        }
    }

    Expr x_;
    std::string name_;
};

class InHouseBackend : public SymbolicBackend {
public:
    std::optional<Expr> factor(const Expr& e) const override { return detail::factorInHouse(e); }

    std::optional<std::vector<Expr>> solve(const Expr& lhs, const Expr& rhs, const std::string& x) const override
    {
        const Expr f = lhs - rhs;
        return Solver(variableIn(f, x)).zeros(f, 0);
    }

    std::optional<Expr> integrate(const Expr& e, const std::string& x) const override
    {
        return Integrator(variableIn(e, x)).antiderivative(e, 0);
    }
};

} // namespace

const SymbolicBackend& inHouseBackend()
{
    static const InHouseBackend backend;
    return backend;
}

std::vector<Expr> solve(const Expr& lhs, const Expr& rhs, const std::string& x)
{
    const Expr f = simplify(lhs - rhs);
    if (!dependsOn(f, x)) {
        throw BeyondScope(f == Expr(0) ? "Any " + x + " is a solution" : "There's no solution for " + x);
    }
    const auto solutions = inHouseBackend().solve(lhs, rhs, x);
    if (!solutions) {
        throw BeyondScope("Couldn't solve this for " + x);
    }
    if (solutions->empty()) {
        throw BeyondScope("There's no real solution for " + x);
    }
    std::vector<Expr> checked;
    for (const Expr& s : *solutions) {
        const Expr solution = simplify(s);
        if (!fails(f, x, solution)) {
            checked.push_back(solution);
        }
    }
    if (checked.empty()) {
        throw BeyondScope("There's no real solution for " + x);
    }
    return distinct(checked);
}

Expr integrate(const Expr& e, const std::string& x)
{
    const auto result = inHouseBackend().integrate(simplify(e), x);
    if (!result) {
        throw BeyondScope("Couldn't integrate this");
    }
    return simplify(*result);
}

Expr integrate(const Expr& e, const std::string& x, const Expr& from, const Expr& to)
{
    const Expr antiderivative = integrate(e, x);
    const auto at = [&](const Expr& limit) {
        return replaceSymbols(antiderivative, [&](const Symbol& s) -> std::optional<Expr> {
            return s.kind == Symbol::Kind::Variable && s.name == x ? std::optional(limit) : std::nullopt;
        });
    };
    return simplify(at(to) - at(from));
}

} // namespace lemma
