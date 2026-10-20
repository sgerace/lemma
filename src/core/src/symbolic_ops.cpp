#include "lemma/symbolic_ops.h"

#include <algorithm>
#include <cmath>
#include <limits>

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

bool isUnit(const Expr& e)
{
    return e.kind() == Expr::Kind::Symbol && e.symbol().kind == Symbol::Kind::Unit;
}

// A term split into its units, with number exponents, and the rest: kN·m x is kN·m and x.
struct UnitSplit {
    Expr units;
    Expr rest;
};

UnitSplit splitUnits(const Expr& term)
{
    std::vector<Expr> units;
    std::vector<Expr> rest;
    const std::vector<ExprFactor> factors =
        term.kind() == Expr::Kind::Product ? term.factors() : std::vector<ExprFactor>{{term, Expr(1)}};
    for (const ExprFactor& f : factors) {
        (isUnit(f.base) && f.exponent.isNumber() ? units : rest).push_back(Expr::pow(f.base, f.exponent));
    }
    return {Expr::product(units), Expr::product(rest)};
}

// The size of a product of units, in SI base units.
Number sizeOfUnits(const Expr& units)
{
    const Expr size = replaceSymbols(units, [](const Symbol& s) -> std::optional<Expr> {
        return s.kind == Symbol::Kind::Unit ? std::optional(sizeOf(s)) : std::nullopt;
    });
    if (size.isNumber()) {
        return size.number();
    }
    // Not a plain number, as for a degree (π/180): as a decimal.
    double value = 1.0;
    const std::vector<ExprFactor> factors =
        units.kind() == Expr::Kind::Product ? units.factors() : std::vector<ExprFactor>{{units, Expr(1)}};
    for (const ExprFactor& f : factors) {
        value *= std::pow(f.base.symbol().size, f.exponent.number().toDouble());
    }
    return Number::approximate(value);
}

std::size_t firstAppearance(const Expr& units, const std::vector<std::string>& order)
{
    std::size_t first = std::numeric_limits<std::size_t>::max();
    replaceSymbols(units, [&](const Symbol& s) -> std::optional<Expr> {
        const auto it = std::find(order.begin(), order.end(), s.name);
        first = std::min(first, static_cast<std::size_t>(it - order.begin()));
        return std::nullopt;
    });
    return first;
}

// The unit symbols in a product of units, in the order they appear.
std::vector<Symbol> unitSymbols(const Expr& units, const std::vector<std::string>& order)
{
    std::vector<Symbol> symbols;
    replaceSymbols(units, [&](const Symbol& s) -> std::optional<Expr> {
        symbols.push_back(s);
        return std::nullopt;
    });
    const auto at = [&](const Symbol& s) { return std::find(order.begin(), order.end(), s.name) - order.begin(); };
    std::stable_sort(symbols.begin(), symbols.end(), [&](const Symbol& a, const Symbol& b) { return at(a) < at(b); });
    return symbols;
}

// A term's units, tidied (section 8): units of one dimension become the one that
// appears first (m·mm is m², with a factor), and units whose product is the dimension
// of one of them become that one, as kN·m³/(GPa·mm⁴) is m. Units with no dimension
// left go: m/mm is 1000. Converted coefficients show as decimals.
Expr simplifyUnits(const Expr& term, const std::vector<std::string>& order)
{
    if (term.kind() != Expr::Kind::Product) {
        return term;
    }
    const UnitSplit split = splitUnits(term); // the coefficient is in neither part
    if (split.units == Expr(1)) {
        return term;
    }
    const std::vector<Symbol> symbols = unitSymbols(split.units, order);
    const auto dimension = split.units.dimension();
    Expr units = split.units;

    // The dimension of one of the units: become it.
    std::optional<Expr> target;
    if (dimension && dimension->isDimensionless()) {
        target = Expr(1);
    } else {
        for (const Symbol& s : symbols) {
            if (dimension && s.dimension == *dimension) {
                target = Expr::symbol(s);
                break;
            }
        }
    }
    if (!target) {
        // Units of one dimension: the first of them, for all.
        units = replaceSymbols(split.units, [&](const Symbol& s) -> std::optional<Expr> {
            for (const Symbol& first : symbols) {
                if (first.dimension == s.dimension) {
                    return first == s ? std::nullopt : std::optional(Expr::symbol(first));
                }
            }
            return std::nullopt;
        });
        if (units == split.units) {
            return term;
        }
    } else {
        units = *target;
        if (units == split.units) {
            return term;
        }
    }
    const Number factor = sizeOfUnits(split.units) / sizeOfUnits(units);
    Number coefficient = term.coefficient() * factor;
    coefficient = coefficient.isExact() ? Number(coefficient.exact(), true) : coefficient;
    return Expr(coefficient) * split.rest * units;
}

} // namespace

Expr combineUnits(const Expr& e, const std::vector<std::string>& order)
{
    switch (e.kind()) {
    case Expr::Kind::Number:
    case Expr::Kind::Symbol:
        return e;
    case Expr::Kind::Product: {
        std::vector<Expr> parts{Expr(e.coefficient())};
        for (const ExprFactor& f : e.factors()) {
            parts.push_back(Expr::pow(combineUnits(f.base, order), combineUnits(f.exponent, order)));
        }
        return simplifyUnits(Expr::product(parts), order);
    }
    case Expr::Kind::Function:
        return Expr::apply(e.function(), combineUnits(e.argument(), order));
    case Expr::Kind::Call: {
        std::vector<Expr> args;
        for (const Expr& a : e.arguments()) {
            args.push_back(combineUnits(a, order));
        }
        return Expr::call(e.name(), std::move(args));
    }
    case Expr::Kind::Sum:
        break;
    }

    struct Term {
        Number coefficient;
        UnitSplit split;
    };
    std::vector<Term> terms;
    for (const ExprTerm& t : e.terms()) {
        terms.push_back({t.coefficient, splitUnits(combineUnits(t.expr, order))});
    }
    for (std::size_t i = 0; i < terms.size(); ++i) {
        // The units to convert to: among the terms like this one, those named first.
        const UnitSplit& a = terms[i].split;
        if (a.units == Expr(1)) {
            continue;
        }
        Expr target = a.units;
        for (const Term& t : terms) {
            if (t.split.rest == a.rest && !(t.split.units == Expr(1)) && t.split.units.dimension() == a.units.dimension()
                && firstAppearance(t.split.units, order) < firstAppearance(target, order)) {
                target = t.split.units;
            }
        }
        if (!(target == a.units)) {
            const Number factor = sizeOfUnits(a.units) / sizeOfUnits(target);
            const Number converted = terms[i].coefficient * factor;
            terms[i].coefficient = converted.isExact() ? Number(converted.exact(), true) : converted;
            terms[i].split.units = target;
        }
    }

    std::vector<Expr> parts{Expr(e.constant())};
    for (const Term& t : terms) {
        parts.push_back(Expr(t.coefficient) * t.split.rest * t.split.units);
    }
    return Expr::sum(parts);
}

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
