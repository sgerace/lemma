// `factor` (docs/symbolic-spec.md, section 9.3): common factors, then polynomials by
// their rational roots.

#include "lemma/symbolic_ops.h"

#include "symbolic_internal.h"

#include "lemma/error.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <numeric>

namespace lemma {

namespace {

constexpr int kMaxDegree = 30;
constexpr std::int64_t kMaxDivisorSearch = 1'000'000'000'000LL;

// A sum's parts, each a coefficient times what's left: the constant, then the terms.
std::vector<Expr> termsOf(const Expr& sum)
{
    std::vector<Expr> out;
    if (!sum.constant().isZero()) {
        out.emplace_back(sum.constant());
    }
    for (const ExprTerm& t : sum.terms()) {
        out.push_back(Expr(t.coefficient) * t.expr);
    }
    return out;
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

bool isWholeAtLeast(const Expr& e, std::int64_t least)
{
    return e.isNumber() && e.number().isInteger() && !e.number().decimal() && e.number().exact().num() >= least;
}

// True if `e` has `v` anywhere in it.
bool contains(const Expr& e, const Expr& v)
{
    if (e == v) {
        return true;
    }
    switch (e.kind()) {
    case Expr::Kind::Number:
    case Expr::Kind::Symbol:
        return false;
    case Expr::Kind::Sum:
        return std::any_of(e.terms().begin(), e.terms().end(), [&](const ExprTerm& t) { return contains(t.expr, v); });
    case Expr::Kind::Product:
        return std::any_of(e.factors().begin(), e.factors().end(), [&](const ExprFactor& f) {
            return contains(f.base, v) || contains(f.exponent, v);
        });
    case Expr::Kind::Function:
        return contains(e.argument(), v);
    case Expr::Kind::Call:
        return std::any_of(e.arguments().begin(), e.arguments().end(), [&](const Expr& a) { return contains(a, v); });
    }
    return false;
}

// What a sum could be a polynomial in: its variables, and functions like sin(x), that
// appear to a whole power of two or more somewhere.
std::vector<Expr> generators(const Expr& sum)
{
    std::vector<Expr> out;
    for (const ExprTerm& t : sum.terms()) {
        for (const ExprFactor& f : factorsOf(t.expr)) {
            const bool variable = f.base.kind() == Expr::Kind::Symbol && f.base.symbol().kind == Symbol::Kind::Variable;
            const bool function = f.base.kind() == Expr::Kind::Function || f.base.kind() == Expr::Kind::Call;
            if ((variable || function) && isWholeAtLeast(f.exponent, 1)
                && std::find(out.begin(), out.end(), f.base) == out.end()) {
                out.push_back(f.base);
            }
        }
    }
    // Those of highest degree first: x² y is a quadratic in x before it's linear in y.
    const auto degree = [&](const Expr& v) {
        std::int64_t d = 0;
        for (const ExprTerm& t : sum.terms()) {
            for (const ExprFactor& f : factorsOf(t.expr)) {
                if (f.base == v && isWholeAtLeast(f.exponent, 1)) {
                    d = std::max(d, f.exponent.number().exact().num());
                }
            }
        }
        return d;
    };
    std::stable_sort(out.begin(), out.end(), [&](const Expr& a, const Expr& b) { return degree(a) > degree(b); });
    return out;
}

} // namespace

std::optional<std::vector<Expr>> detail::coefficientsIn(const Expr& sum, const Expr& v)
{
    std::vector<Expr> coefficients;
    for (const Expr& term : termsOf(sum)) {
        std::int64_t k = 0;
        Expr rest = term;
        for (const ExprFactor& f : factorsOf(term)) {
            if (f.base == v) {
                if (!isWholeAtLeast(f.exponent, 0)) {
                    return std::nullopt;
                }
                k = f.exponent.number().exact().num();
                rest = term / Expr::pow(v, f.exponent);
            }
        }
        if (k > kMaxDegree || contains(rest, v)) {
            return std::nullopt;
        }
        if (coefficients.size() <= static_cast<std::size_t>(k)) {
            coefficients.resize(static_cast<std::size_t>(k) + 1);
        }
        coefficients[static_cast<std::size_t>(k)] = coefficients[static_cast<std::size_t>(k)] + rest;
    }
    return coefficients;
}

namespace {

using detail::coefficientsIn;

std::vector<std::int64_t> divisors(std::int64_t n)
{
    n = n < 0 ? -n : n;
    std::vector<std::int64_t> out;
    for (std::int64_t d = 1; d <= n / d; ++d) {
        if (n % d == 0) {
            out.push_back(d);
            if (d != n / d) {
                out.push_back(n / d);
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// p(r), exactly, or nothing if the numbers got too big to be exact.
std::optional<Number> valueAt(const std::vector<Number>& p, const Number& r)
{
    Number value(0);
    for (std::size_t i = p.size(); i-- > 0;) {
        value = value * r + p[i];
    }
    return value.isExact() ? std::optional(value) : std::nullopt;
}

// p divided by (q v − p0), which divides it exactly: the quotient's coefficients.
std::vector<Number> divide(const std::vector<Number>& p, const Number& q, const Number& p0)
{
    // Synthetic division by the root r = p0/q, then by q.
    const Number r = p0 / q;
    std::vector<Number> quotient(p.size() - 1);
    Number carry(0);
    for (std::size_t i = p.size(); i-- > 1;) {
        carry = carry * r + p[i];
        quotient[i - 1] = carry;
    }
    for (Number& c : quotient) {
        c = c / q;
    }
    return quotient;
}

// Σ c_k v^k m^(d−k): a polynomial in v, homogeneous with m.
Expr polynomial(const std::vector<Number>& c, const Expr& v, const Expr& m)
{
    const std::int64_t d = static_cast<std::int64_t>(c.size()) - 1;
    std::vector<Expr> parts;
    for (std::int64_t k = 0; k <= d; ++k) {
        parts.push_back(Expr(c[static_cast<std::size_t>(k)]) * Expr::pow(v, Expr(k)) * Expr::pow(m, Expr(d - k)));
    }
    return Expr::sum(parts);
}

// A polynomial in v (in v and m, homogeneously) with exact coefficients, factored by
// its rational roots, and a quadratic left over by its real roots. Nothing if it
// doesn't factor.
std::optional<Expr> factorNumeric(std::vector<Number> c, const Expr& v, const Expr& m)
{
    // Whole coefficients, with the leading one positive: the rest goes in front.
    std::int64_t den = 1;
    for (const Number& n : c) {
        den = std::lcm(den, n.exact().den());
        if (den > 1'000'000'000) {
            return std::nullopt;
        }
    }
    std::int64_t content = 0;
    for (Number& n : c) {
        n = n * Number(den);
        if (!n.isExact()) {
            return std::nullopt;
        }
        content = std::gcd(content, std::abs(n.exact().num()));
    }
    if (c.back().isNegative()) {
        content = -content;
    }
    for (Number& n : c) {
        n = n / Number(content);
    }
    std::vector<Expr> factors{Expr(Number(Rational(content, den)))};

    bool factored = false;
    while (c.size() > 2) {
        const std::int64_t a0 = c.front().exact().num();
        const std::int64_t an = c.back().exact().num();
        if (std::abs(a0) > kMaxDivisorSearch || std::abs(an) > kMaxDivisorSearch) {
            break;
        }
        std::optional<std::pair<std::int64_t, std::int64_t>> root; // p/q
        if (a0 == 0) {
            root = std::pair<std::int64_t, std::int64_t>{0, 1};
        }
        for (const std::int64_t q : divisors(an)) {
            for (const std::int64_t p : divisors(a0)) {
                for (const std::int64_t signed_p : {p, -p}) {
                    if (!root && std::gcd(p, q) == 1) {
                        if (const auto value = valueAt(c, Number(Rational(signed_p, q))); value && value->isZero()) {
                            root = std::pair{signed_p, q};
                        }
                    }
                }
            }
        }
        if (!root) {
            break;
        }
        const auto [p, q] = *root;
        factors.push_back(Expr(q) * v - Expr(p) * m); // q v − p m
        c = divide(c, Number(q), Number(p));
        factored = true;
    }

    if (c.size() == 3) {
        // A quadratic left: by the formula, when its roots are real and not rational.
        const Number a = c[2];
        const Number b = c[1];
        const Number k = c[0];
        const Number discriminant = b * b - Number(4) * a * k;
        if (!discriminant.isNegative() && !discriminant.isZero()) {
            const Expr root = Expr::pow(Expr(discriminant), Expr(Number(Rational(1, 2))));
            if (!root.isNumber()) {
                const Expr r1 = (Expr(-b) + root) / Expr(Number(2) * a);
                const Expr r2 = (Expr(-b) - root) / Expr(Number(2) * a);
                factors.push_back(Expr(a));
                factors.push_back(v - r1 * m);
                factors.push_back(v - r2 * m);
                return Expr::product(factors);
            }
        }
    }
    if (!factored) {
        return std::nullopt;
    }
    factors.push_back(polynomial(c, v, m));
    return Expr::product(factors);
}

// A sum without a common factor, factored as a polynomial in one of its generators.
std::optional<Expr> factorPolynomial(const Expr& sum)
{
    for (const Expr& v : generators(sum)) {
        const auto coefficients = coefficientsIn(sum, v);
        if (!coefficients || coefficients->size() < 3 || !coefficients->back().isNumber()) {
            continue;
        }
        // Homogeneous in v and some m: c_k = n_k m^(d − k), as in x² − 5 x y + 6 y².
        const std::size_t d = coefficients->size() - 1;
        Expr m(1);
        const Expr& c0 = coefficients->front();
        if (!c0.isNumber()) {
            const auto [content, monomial] = c0.kind() == Expr::Kind::Product
                                                 ? std::pair{Expr(c0.coefficient()), c0 / Expr(c0.coefficient())}
                                                 : std::pair{Expr(1), c0};
            m = Expr::pow(monomial, Expr(Number(Rational(1, static_cast<std::int64_t>(d)))));
        }
        std::vector<Number> numbers;
        bool homogeneous = true;
        for (std::size_t k = 0; k <= d && homogeneous; ++k) {
            const Expr n = (*coefficients)[k] / Expr::pow(m, Expr(static_cast<std::int64_t>(d - k)));
            homogeneous = n.isNumber() && n.number().isExact() && !n.number().decimal();
            if (homogeneous) {
                numbers.push_back(n.number());
            }
        }
        if (!homogeneous) {
            continue;
        }
        if (auto f = factorNumeric(numbers, v, m)) {
            return f;
        }
    }
    return std::nullopt;
}

Expr factorSum(const Expr& sum)
{
    Expr expanded = sum;
    try {
        expanded = expand(sum, 500);
    } catch (const Error&) {
        return sum;
    }
    const auto [outside, inside] = splitCommonFactor(expanded);
    if (inside.kind() != Expr::Kind::Sum) {
        return outside * inside;
    }
    if (auto f = factorPolynomial(inside)) {
        // Each factor may factor further: x⁴ − y⁴ is (x − y)(x + y)(x² + y²).
        std::vector<Expr> parts{outside};
        const Expr& product = *f;
        parts.emplace_back(product.coefficient());
        for (const ExprFactor& p : product.factors()) {
            const Expr base = p.base.kind() == Expr::Kind::Sum && !(p.base == inside) ? factorSum(p.base) : p.base;
            parts.push_back(Expr::pow(base, p.exponent));
        }
        return Expr::product(parts);
    }
    return Expr::product({outside, inside});
}

// A polynomial of degree two or more in one of the sum's generators.
bool isHigherDegree(const Expr& sum)
{
    for (const Expr& v : generators(sum)) {
        if (const auto c = coefficientsIn(sum, v); c && c->size() >= 3) {
            return true;
        }
    }
    return false;
}

} // namespace

std::optional<Expr> detail::factorInHouse(const Expr& e)
{
    switch (e.kind()) {
    case Expr::Kind::Sum:
        return factorSum(e);
    case Expr::Kind::Product: {
        std::vector<Expr> parts{Expr(e.coefficient())};
        for (const ExprFactor& f : e.factors()) {
            const Expr base = f.base.kind() == Expr::Kind::Sum ? factorSum(f.base) : f.base;
            parts.push_back(Expr::pow(base, f.exponent));
        }
        return Expr::product(parts);
    }
    default:
        return e;
    }
}

Expr factor(const Expr& e)
{
    return inHouseBackend().factor(e).value_or(e);
}

bool hasUnfactored(const Expr& factored)
{
    for (const ExprFactor& f : factorsOf(factored)) {
        if (f.base.kind() == Expr::Kind::Sum && isHigherDegree(f.base)) {
            return true;
        }
    }
    return factored.kind() == Expr::Kind::Sum && isHigherDegree(factored);
}

} // namespace lemma
