#include "lemma/symbolic_ops.h"

#include "lemma/error.h"
#include "lemma/units.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

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

// How big an expression is, to choose the shorter of two forms (section 9.1).
std::size_t exprSize(const Expr& e)
{
    switch (e.kind()) {
    case Expr::Kind::Number:
    case Expr::Kind::Symbol:
        return 1;
    case Expr::Kind::Sum: {
        std::size_t n = e.constant().isZero() ? 0 : 1;
        for (const ExprTerm& t : e.terms()) {
            n += exprSize(t.expr) + (t.coefficient.isOne() || t.coefficient == Number(-1) ? 0 : 1);
        }
        return n + e.terms().size();
    }
    case Expr::Kind::Product: {
        std::size_t n = e.coefficient().isOne() || e.coefficient() == Number(-1) ? 0 : 1;
        for (const ExprFactor& f : e.factors()) {
            // A sum as a factor costs its parentheses too.
            n += exprSize(f.base) + (f.exponent == Expr(1) ? 0 : exprSize(f.exponent))
                 + (f.base.kind() == Expr::Kind::Sum ? 1 : 0);
        }
        return n;
    }
    case Expr::Kind::Function:
        return 1 + exprSize(e.argument());
    case Expr::Kind::Call: {
        std::size_t n = 1;
        for (const Expr& a : e.arguments()) {
            n += exprSize(a);
        }
        return n;
    }
    }
    return 1;
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

std::int64_t gcd64(std::int64_t a, std::int64_t b)
{
    return std::gcd(a < 0 ? -a : a, b < 0 ? -b : b);
}

// A sum with what its terms have in common taken out: the factors in every term, to
// their lowest power, and the coefficients' common part, so the sum left has whole
// coefficients: P x L − P x²/2 is (P x/2)(2 L − x).
} // namespace

std::pair<Expr, Expr> splitCommonFactor(const Expr& sum)
{
    if (sum.kind() != Expr::Kind::Sum || !sum.constant().isZero()) {
        return {Expr(1), sum};
    }
    const std::vector<ExprTerm>& terms = sum.terms();
    std::vector<ExprFactor> common = factorsOf(terms[0].expr);
    for (std::size_t i = 1; i < terms.size(); ++i) {
        const std::vector<ExprFactor> here = factorsOf(terms[i].expr);
        std::vector<ExprFactor> kept;
        for (const ExprFactor& c : common) {
            const auto match = std::find_if(here.begin(), here.end(), [&](const ExprFactor& f) { return f.base == c.base; });
            if (match != here.end() && !c.exponent.isNumber() && c.exponent == match->exponent) {
                kept.push_back(c); // the same power, like eˣ in x² eˣ − 2 x eˣ
                continue;
            }
            if (match == here.end() || !c.exponent.isNumber() || !match->exponent.isNumber()
                || !c.exponent.number().isExact() || !match->exponent.number().isExact()) {
                continue;
            }
            const Rational a = c.exponent.number().exact();
            const Rational b = match->exponent.number().exact();
            if ((a < Rational(0)) != (b < Rational(0))) {
                continue;
            }
            // The power nearer zero: x² and x³ share x², 1/x and 1/x² share 1/x.
            kept.push_back({c.base, Expr(Number(a < Rational(0) ? (a > b ? a : b) : (a < b ? a : b)))});
        }
        common = std::move(kept);
    }

    // The coefficients' common part, when they're exact.
    Number content(1);
    bool exact = std::all_of(terms.begin(), terms.end(), [](const ExprTerm& t) {
        return t.coefficient.isExact() && !t.coefficient.decimal();
    });
    if (exact) {
        std::int64_t num = 0;
        std::int64_t den = 1;
        for (const ExprTerm& t : terms) {
            num = gcd64(num, t.coefficient.exact().num());
            den = std::lcm(den, t.coefficient.exact().den());
            if (den > 1'000'000'000) {
                exact = false;
                break;
            }
        }
        if (exact && num != 0) {
            const bool negative = std::all_of(terms.begin(), terms.end(), [](const ExprTerm& t) {
                return t.coefficient.isNegative();
            });
            content = Number(Rational(negative ? -num : num, den));
        }
    }

    std::vector<Expr> monomial{Expr(content)};
    for (const ExprFactor& f : common) {
        monomial.push_back(Expr::pow(f.base, f.exponent));
    }
    const Expr outside = Expr::product(monomial);
    if (outside == Expr(1)) {
        return {outside, sum};
    }
    std::vector<Expr> inside;
    for (const ExprTerm& t : terms) {
        inside.push_back(Expr(t.coefficient) * t.expr / outside);
    }
    return {outside, Expr::sum(inside)};
}

namespace {

Expr commonFactor(const Expr& sum)
{
    const auto [outside, inside] = splitCommonFactor(sum);
    // A product, not multiplied back in.
    return outside == Expr(1) ? sum : Expr::product({outside, inside});
}

// A fraction with the polynomial factors its numerator and denominator share cancelled:
// (x² − 1)/(x − 1) is x + 1 (section 9.1). Unchanged when they share none, so
// (x² − 5 x + 6)/(x − 1) stays multiplied out.
Expr cancel(const Expr& product)
{
    if (product.kind() != Expr::Kind::Product) {
        return product;
    }
    const auto whole = [](const Expr& e) { return e.isNumber() && e.number().isInteger(); };
    std::vector<Expr> up;
    std::vector<Expr> down;
    std::vector<Expr> others{Expr(product.coefficient())};
    for (const ExprFactor& f : product.factors()) {
        if (f.base.kind() == Expr::Kind::Sum && whole(f.exponent)) {
            (f.exponent.number().isNegative() ? down : up).push_back(Expr::pow(f.base, f.exponent));
        } else {
            others.push_back(Expr::pow(f.base, f.exponent));
        }
    }
    if (up.empty() || down.empty()) {
        return product;
    }
    const Expr top = factor(Expr::product(up));
    const Expr bottom = factor(Expr::pow(Expr::product(down), Expr(-1)));
    const auto bases = [](const Expr& e) {
        std::vector<Expr> out;
        for (const ExprFactor& f : e.kind() == Expr::Kind::Product ? e.factors() : std::vector<ExprFactor>{{e, Expr(1)}}) {
            if (f.base.kind() == Expr::Kind::Sum) {
                out.push_back(f.base);
            }
        }
        return out;
    };
    const std::vector<Expr> topBases = bases(top);
    const std::vector<Expr> bottomBases = bases(bottom);
    const bool shared = std::any_of(topBases.begin(), topBases.end(), [&](const Expr& b) {
        return std::find(bottomBases.begin(), bottomBases.end(), b) != bottomBases.end();
    });
    if (!shared) {
        return product;
    }
    // What's left of the numerator and denominator multiplied out again, as typed.
    const Expr left = top / bottom;
    std::vector<Expr> numerator;
    std::vector<Expr> denominator;
    if (left.kind() == Expr::Kind::Product) {
        others.emplace_back(left.coefficient());
    }
    for (const ExprFactor& f : left.kind() == Expr::Kind::Product ? left.factors() : std::vector<ExprFactor>{{left, Expr(1)}}) {
        if (f.base.kind() == Expr::Kind::Sum && whole(f.exponent)) {
            if (f.exponent.number().isNegative()) {
                denominator.push_back(Expr::pow(f.base, -f.exponent));
            } else {
                numerator.push_back(Expr::pow(f.base, f.exponent));
            }
        } else {
            others.push_back(Expr::pow(f.base, f.exponent));
        }
    }
    const auto multipliedOut = [](const std::vector<Expr>& part) {
        try {
            return expand(Expr::product(part), 200);
        } catch (const Error&) {
            return Expr::product(part);
        }
    };
    others.push_back(multipliedOut(numerator));
    others.push_back(Expr::pow(multipliedOut(denominator), Expr(-1)));
    return Expr::product(others);
}

// A sum's terms over the same denominator combined, as an expression.
Expr overCommonDenominators(const Expr& sum)
{
    if (sum.kind() != Expr::Kind::Sum) {
        return sum;
    }
    std::vector<ExprTerm> terms = sum.terms();
    commonDenominators(terms);
    std::vector<Expr> parts{Expr(sum.constant())};
    for (const ExprTerm& t : terms) {
        parts.push_back(Expr(t.coefficient) * t.expr);
    }
    return Expr::sum(parts);
}

// The shortest of a sum's forms (section 9.1): over common denominators, as it is,
// multiplied out, and with a common factor taken out. On a tie, the first.
Expr shortest(const Expr& sum)
{
    std::vector<Expr> candidates{overCommonDenominators(sum), sum, commonFactor(sum)};
    try {
        const Expr expanded = expand(sum, 200);
        candidates.push_back(expanded);
        candidates.push_back(overCommonDenominators(expanded));
        candidates.push_back(commonFactor(expanded));
    } catch (const Error&) {
        // Too big to multiply out.
    }
    Expr best = candidates.front();
    for (const Expr& candidate : candidates) {
        if (exprSize(candidate) < exprSize(best)) {
            best = candidate;
        }
    }
    return best;
}

Expr simplifySum(const Expr& sum)
{
    std::vector<ExprTerm> terms;
    for (const ExprTerm& t : sum.terms()) {
        terms.push_back({simplify(t.expr), t.coefficient});
    }
    while (pythagoras(terms)) {
    }

    std::vector<Expr> parts{Expr(sum.constant())};
    for (const ExprTerm& t : terms) {
        parts.push_back(Expr(t.coefficient) * t.expr);
    }
    const Expr result = Expr::sum(parts);
    return result.kind() == Expr::Kind::Sum ? shortest(result) : result;
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

// A dimension in units: its named unit (N, J, Pa), or SI base units (kg·m/s²).
Expr siUnits(const Dimension& d)
{
    if (const Unit* unit = UnitRegistry::instance().preferredFor(d)) {
        return Expr::unit(unit->name, unit->value.dim, unit->value.value);
    }
    static const char* const kBase[] = {"m", "kg", "s", "A", "K", "mol", "cd"};
    std::vector<Expr> parts;
    for (std::size_t b = 0; b < Dimension::kCount; ++b) {
        const Rational e = d[static_cast<BaseDimension>(b)];
        if (!e.isZero()) {
            const Unit* unit = UnitRegistry::instance().find(kBase[b]);
            parts.push_back(Expr::pow(Expr::unit(unit->name, unit->value.dim, unit->value.value), Expr(Number(e))));
        }
    }
    return Expr::product(parts);
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
    if (!target && dimension) {
        // Three or more units that partly cancel, like kN/(GPa·mm⁴): SI units for what's
        // left, or its named unit (m⁻² here). kN·m and kN/m stay as they are.
        int named = 0;
        int total = 0;
        std::vector<std::string> seen;
        replaceSymbols(split.units, [&](const Symbol& s) -> std::optional<Expr> {
            if (std::find(seen.begin(), seen.end(), s.name) == seen.end()) {
                seen.push_back(s.name);
            }
            return std::nullopt;
        });
        for (const ExprFactor& f : split.units.kind() == Expr::Kind::Product
                                      ? split.units.factors()
                                      : std::vector<ExprFactor>{{split.units, Expr(1)}}) {
            const Rational e = f.exponent.number().exact();
            for (std::size_t b = 0; b < Dimension::kCount; ++b) {
                const Rational x = f.base.symbol().dimension[static_cast<BaseDimension>(b)] * e;
                total += static_cast<int>(std::abs(x.toDouble()) * 1000);
            }
        }
        for (std::size_t b = 0; b < Dimension::kCount; ++b) {
            named += static_cast<int>(std::abs((*dimension)[static_cast<BaseDimension>(b)].toDouble()) * 1000);
        }
        if (seen.size() >= 3 && total > named) {
            target = siUnits(*dimension);
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
        return cancel(Expr::product(parts));
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

namespace lemma {

bool dependsOn(const Expr& e, const std::string& name)
{
    switch (e.kind()) {
    case Expr::Kind::Number:
        return false;
    case Expr::Kind::Symbol:
        return e.symbol().kind == Symbol::Kind::Variable && e.symbol().name == name;
    case Expr::Kind::Sum:
        return std::any_of(e.terms().begin(), e.terms().end(), [&](const ExprTerm& t) { return dependsOn(t.expr, name); });
    case Expr::Kind::Product:
        return std::any_of(e.factors().begin(), e.factors().end(), [&](const ExprFactor& f) {
            return dependsOn(f.base, name) || dependsOn(f.exponent, name);
        });
    case Expr::Kind::Function:
        return dependsOn(e.argument(), name);
    case Expr::Kind::Call:
        return std::any_of(e.arguments().begin(), e.arguments().end(), [&](const Expr& a) { return dependsOn(a, name); });
    }
    return false;
}

// ---------------------------------------------------------------------------
// expand

namespace {

class Expander {
public:
    explicit Expander(std::size_t maxTerms) : maxTerms_(maxTerms) {}

    Expr expand(const Expr& e)
    {
        switch (e.kind()) {
        case Expr::Kind::Number:
        case Expr::Kind::Symbol:
            return e;
        case Expr::Kind::Sum: {
            std::vector<Expr> parts{Expr(e.constant())};
            for (const ExprTerm& t : e.terms()) {
                for (const Expr& term : terms(Expr(t.coefficient) * expand(t.expr))) {
                    parts.push_back(term);
                }
            }
            check(parts.size());
            return Expr::sum(parts);
        }
        case Expr::Kind::Product:
            return Expr::sum(product(e));
        case Expr::Kind::Function: {
            const Expr argument = expand(e.argument());
            if (e.function() == MathFunction::Ln && argument.kind() == Expr::Kind::Product && argument.isPositive()) {
                // ln(a b²) is ln(a) + 2 ln(b), for positive a and b.
                std::vector<Expr> parts{Expr::apply(MathFunction::Ln, Expr(argument.coefficient()))};
                for (const ExprFactor& f : argument.factors()) {
                    parts.push_back(f.exponent * Expr::apply(MathFunction::Ln, f.base));
                }
                return Expr::sum(parts);
            }
            return Expr::apply(e.function(), argument);
        }
        case Expr::Kind::Call: {
            std::vector<Expr> args;
            for (const Expr& a : e.arguments()) {
                args.push_back(expand(a));
            }
            return Expr::call(e.name(), std::move(args));
        }
        }
        return e;
    }

private:
    static std::vector<Expr> terms(const Expr& e)
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

    void check(std::size_t n) const
    {
        if (n > maxTerms_) {
            throw Error("This expands to too many terms");
        }
    }

    // Every pair of terms multiplied: (a + b)(c + d) is a c + a d + b c + b d.
    std::vector<Expr> times(const std::vector<Expr>& a, const std::vector<Expr>& b) const
    {
        check(a.size() * b.size());
        std::vector<Expr> out;
        out.reserve(a.size() * b.size());
        for (const Expr& x : a) {
            for (const Expr& y : b) {
                out.push_back(x * y);
            }
        }
        // Like terms collect as they go, which keeps the count down.
        return terms(Expr::sum(out));
    }

    // A product's terms, multiplied out.
    std::vector<Expr> product(const Expr& e)
    {
        std::vector<Expr> result{Expr(e.coefficient())};
        for (const ExprFactor& f : e.factors()) {
            const Expr base = expand(f.base);
            const Expr exponent = expand(f.exponent);
            const auto n = exponent.isNumber() && exponent.number().isInteger() && !exponent.number().decimal()
                               ? std::optional(exponent.number().exact().num())
                               : std::nullopt;
            if (base.kind() == Expr::Kind::Sum && n && *n != 0) {
                // (a + b)³ multiplied out; a denominator's power too: 1/(x + 1)² is 1/(x² + 2 x + 1).
                const std::int64_t k = *n < 0 ? -*n : *n;
                if (k > 1000) {
                    throw Error("This expands to too many terms");
                }
                std::vector<Expr> power{Expr(1)};
                const std::vector<Expr> sum = terms(base);
                for (std::int64_t i = 0; i < k; ++i) {
                    power = times(power, sum);
                }
                if (*n > 0) {
                    result = times(result, power);
                } else {
                    result = times(result, {Expr::pow(Expr::sum(power), Expr(-1))});
                }
                continue;
            }
            result = times(result, terms(Expr::pow(base, exponent)));
        }
        return result;
    }

    std::size_t maxTerms_;
};

} // namespace

Expr expand(const Expr& e, std::size_t maxTerms)
{
    return Expander(maxTerms).expand(e);
}

// ---------------------------------------------------------------------------
// diff

namespace {

Expr derivative(const Expr& e, const std::string& x)
{
    if (!dependsOn(e, x)) {
        return Expr(0);
    }
    switch (e.kind()) {
    case Expr::Kind::Number:
        return Expr(0);
    case Expr::Kind::Symbol:
        return Expr(1);
    case Expr::Kind::Sum: {
        std::vector<Expr> parts;
        for (const ExprTerm& t : e.terms()) {
            parts.push_back(Expr(t.coefficient) * derivative(t.expr, x));
        }
        return Expr::sum(parts);
    }
    case Expr::Kind::Product: {
        // The product rule: each factor differentiated in turn, times the others.
        const std::vector<ExprFactor>& factors = e.factors();
        std::vector<Expr> parts;
        for (std::size_t i = 0; i < factors.size(); ++i) {
            const ExprFactor& f = factors[i];
            if (!dependsOn(f.base, x) && !dependsOn(f.exponent, x)) {
                continue;
            }
            const Expr power = Expr::pow(f.base, f.exponent);
            Expr d;
            if (!dependsOn(f.exponent, x)) {
                d = f.exponent * Expr::pow(f.base, f.exponent - Expr(1)) * derivative(f.base, x); // the power rule
            } else if (!dependsOn(f.base, x)) {
                d = power * Expr::apply(MathFunction::Ln, f.base) * derivative(f.exponent, x); // aˣ ln a
            } else {
                d = power * (derivative(f.exponent, x) * Expr::apply(MathFunction::Ln, f.base)
                             + f.exponent * derivative(f.base, x) / f.base);
            }
            std::vector<Expr> others{Expr(e.coefficient()), d};
            for (std::size_t j = 0; j < factors.size(); ++j) {
                if (j != i) {
                    others.push_back(Expr::pow(factors[j].base, factors[j].exponent));
                }
            }
            parts.push_back(Expr::product(others));
        }
        return Expr::sum(parts);
    }
    case Expr::Kind::Function: {
        // The chain rule.
        const Expr& u = e.argument();
        const Expr du = derivative(u, x);
        const Expr half(Number(Rational(1, 2)));
        switch (e.function()) {
        case MathFunction::Sin: return Expr::apply(MathFunction::Cos, u) * du;
        case MathFunction::Cos: return -Expr::apply(MathFunction::Sin, u) * du;
        case MathFunction::Tan: return du / Expr::pow(Expr::apply(MathFunction::Cos, u), Expr(2));
        case MathFunction::Asin: return du / Expr::pow(Expr(1) - Expr::pow(u, Expr(2)), half);
        case MathFunction::Acos: return -du / Expr::pow(Expr(1) - Expr::pow(u, Expr(2)), half);
        case MathFunction::Atan: return du / (Expr(1) + Expr::pow(u, Expr(2)));
        case MathFunction::Ln: return du / u;
        case MathFunction::Log: return du / (u * Expr::apply(MathFunction::Ln, Expr(10)));
        case MathFunction::Abs: return u / Expr::apply(MathFunction::Abs, u) * du;
        }
        break;
    }
    case Expr::Kind::Call:
        throw BeyondScope("Couldn't differentiate " + e.name() + ", a function with no definition");
    }
    return Expr(0);
}

} // namespace

Expr differentiate(const Expr& e, const std::string& x, int n)
{
    Expr result = e;
    for (int i = 0; i < n; ++i) {
        result = simplify(derivative(result, x));
    }
    return result;
}

} // namespace lemma
