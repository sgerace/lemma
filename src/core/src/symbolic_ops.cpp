#include "lemma/symbolic_ops.h"

#include <algorithm>

namespace lemma {

namespace {

bool isSquareOf(const ExprFactor& f, MathFunction function)
{
    return f.base.kind() == Expr::Kind::Function && f.base.function() == function && f.exponent == Expr(2);
}

// a sin²u + a cos²u is a, with any common factors: 2 x sin²u + 2 x cos²u is 2 x.
bool pythagoras(std::vector<ExprTerm>& terms)
{
    for (std::size_t i = 0; i < terms.size(); ++i) {
        const Expr& term = terms[i].expr;
        const std::vector<ExprFactor> factors =
            term.kind() == Expr::Kind::Product ? term.factors() : std::vector<ExprFactor>{{term, Expr(1)}};
        for (const ExprFactor& f : factors) {
            if (!isSquareOf(f, MathFunction::Sin)) {
                continue;
            }
            const Expr square = Expr::pow(f.base, Expr(2));
            const Expr rest = term / square;
            const Expr partner = rest * Expr::pow(Expr::apply(MathFunction::Cos, f.base.argument()), Expr(2));
            const auto j = std::find_if(terms.begin(), terms.end(), [&](const ExprTerm& t) {
                return t.expr == partner && t.coefficient == terms[i].coefficient;
            });
            if (j != terms.end()) {
                const Number c = terms[i].coefficient;
                const auto first = std::min(i, static_cast<std::size_t>(j - terms.begin()));
                const auto second = std::max(i, static_cast<std::size_t>(j - terms.begin()));
                terms.erase(terms.begin() + static_cast<std::ptrdiff_t>(second));
                terms.erase(terms.begin() + static_cast<std::ptrdiff_t>(first));
                terms.push_back({rest, c});
                return true;
            }
        }
    }
    return false;
}

// The denominator of a term: its factors with negative exponents, inverted.
Expr denominator(const Expr& term)
{
    std::vector<Expr> parts;
    if (term.kind() == Expr::Kind::Product) {
        for (const ExprFactor& f : term.factors()) {
            if (f.exponent.isNumber() && f.exponent.number().isNegative()) {
                parts.push_back(Expr::pow(f.base, -f.exponent));
            }
        }
    }
    return Expr::product(parts);
}

// Terms over the same denominator combine: a/b + c/b is (a + c)/b.
void commonDenominators(std::vector<ExprTerm>& terms)
{
    std::vector<ExprTerm> result;
    std::vector<bool> used(terms.size(), false);
    for (std::size_t i = 0; i < terms.size(); ++i) {
        if (used[i]) {
            continue;
        }
        const Expr d = denominator(terms[i].expr);
        std::vector<Expr> numerators;
        if (d != Expr(1)) {
            for (std::size_t j = i; j < terms.size(); ++j) {
                if (!used[j] && denominator(terms[j].expr) == d) {
                    numerators.push_back(Expr(terms[j].coefficient) * terms[j].expr * d);
                    used[j] = true;
                }
            }
        }
        if (numerators.size() < 2) {
            used[i] = true;
            result.push_back(terms[i]);
            continue;
        }
        result.push_back({Expr::sum(numerators) / d, Number(1)});
    }
    terms = std::move(result);
}

Expr simplifySum(const Expr& sum)
{
    std::vector<ExprTerm> terms;
    for (const ExprTerm& t : sum.terms()) {
        terms.push_back({simplify(t.expr), t.coefficient});
    }
    while (pythagoras(terms)) {
    }
    commonDenominators(terms);

    std::vector<Expr> parts{Expr(sum.constant())};
    for (const ExprTerm& t : terms) {
        parts.push_back(Expr(t.coefficient) * t.expr);
    }
    return Expr::sum(parts);
}

} // namespace

Expr simplify(const Expr& e)
{
    switch (e.kind()) {
    case Expr::Kind::Number:
    case Expr::Kind::Symbol:
        return e;
    case Expr::Kind::Sum:
        return simplifySum(e);
    case Expr::Kind::Product: {
        std::vector<Expr> parts{Expr(e.coefficient())};
        for (const ExprFactor& f : e.factors()) {
            parts.push_back(Expr::pow(simplify(f.base), simplify(f.exponent)));
        }
        return Expr::product(parts);
    }
    case Expr::Kind::Function:
        return Expr::apply(e.function(), simplify(e.argument()));
    case Expr::Kind::Call: {
        std::vector<Expr> args;
        for (const Expr& a : e.arguments()) {
            args.push_back(simplify(a));
        }
        return Expr::call(e.name(), std::move(args));
    }
    }
    return e;
}

} // namespace lemma
