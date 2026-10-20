#include "lemma/symbolic.h"

#include "lemma/error.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <numeric>
#include <utility>

namespace lemma {

// ---------------------------------------------------------------------------
// Numbers

namespace {

constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();

// Integer and rational arithmetic that reports overflow instead of wrapping, so a
// number too large to be exact becomes a double.
bool addInt(std::int64_t a, std::int64_t b, std::int64_t& r)
{
    if ((b > 0 && a > kMax - b) || (b < 0 && a < kMin - b)) {
        return false;
    }
    r = a + b;
    return true;
}

bool mulInt(std::int64_t a, std::int64_t b, std::int64_t& r)
{
    if (a == 0 || b == 0) {
        r = 0;
        return true;
    }
    if (a == kMin || b == kMin || std::abs(a) > kMax / std::abs(b)) {
        return false;
    }
    r = a * b;
    return true;
}

std::optional<Rational> addExact(Rational a, Rational b)
{
    const std::int64_t g = std::gcd(a.den(), b.den());
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t n = 0;
    std::int64_t d = 0;
    if (!mulInt(a.num(), b.den() / g, x) || !mulInt(b.num(), a.den() / g, y) || !addInt(x, y, n)
        || !mulInt(a.den(), b.den() / g, d)) {
        return std::nullopt;
    }
    return Rational(n, d);
}

std::optional<Rational> mulExact(Rational a, Rational b)
{
    const std::int64_t g1 = std::gcd(a.num(), b.den());
    const std::int64_t g2 = std::gcd(b.num(), a.den());
    std::int64_t n = 0;
    std::int64_t d = 0;
    if (!mulInt(a.num() / g1, b.num() / g2, n) || !mulInt(a.den() / g2, b.den() / g1, d)) {
        return std::nullopt;
    }
    return Rational(n, d);
}

std::int64_t floorDiv(std::int64_t a, std::int64_t b)
{
    const std::int64_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

Rational floorOf(Rational r)
{
    return Rational(floorDiv(r.num(), r.den()));
}

// n to an integer power, exactly while it fits.
Number powInt(const Number& n, std::int64_t k)
{
    if (k < 0) {
        return Number(1) / powInt(n, -k);
    }
    Number result(Rational(1), n.decimal());
    Number square = n;
    while (k > 0) {
        if (k & 1) {
            result = result * square;
        }
        k >>= 1;
        if (k > 0) {
            square = square * square;
        }
    }
    return result;
}

} // namespace

Number Number::approximate(double value)
{
    if (!std::isfinite(value)) {
        throw Error("The result is not a finite number");
    }
    Number n;
    n.exact_.reset();
    n.approx_ = value;
    n.decimal_ = true;
    return n;
}

Number operator+(const Number& a, const Number& b)
{
    if (a.isExact() && b.isExact()) {
        if (const auto r = addExact(a.exact(), b.exact())) {
            return Number(*r, a.decimal_ || b.decimal_);
        }
    }
    return Number::approximate(a.toDouble() + b.toDouble());
}

Number operator*(const Number& a, const Number& b)
{
    if (a.isExact() && b.isExact()) {
        if (const auto r = mulExact(a.exact(), b.exact())) {
            return Number(*r, a.decimal_ || b.decimal_);
        }
    }
    return Number::approximate(a.toDouble() * b.toDouble());
}

Number operator/(const Number& a, const Number& b)
{
    if (b.isZero()) {
        throw Error("Division by zero");
    }
    if (b.isExact()) {
        const Rational& r = b.exact();
        return a * Number(Rational(r.den(), r.num()), b.decimal_);
    }
    return Number::approximate(a.toDouble() / b.toDouble());
}

Number Number::operator-() const
{
    Number n = *this;
    if (n.exact_) {
        n.exact_ = -*n.exact_;
    } else {
        n.approx_ = -n.approx_;
    }
    return n;
}

int compare(const Number& a, const Number& b)
{
    if (a.isExact() && b.isExact()) {
        if (const auto d = addExact(a.exact(), -b.exact())) {
            if (!d->isZero()) {
                return d->num() < 0 ? -1 : 1;
            }
        } else if (a.toDouble() != b.toDouble()) {
            return a.toDouble() < b.toDouble() ? -1 : 1;
        }
    } else if (a.toDouble() != b.toDouble()) {
        return a.toDouble() < b.toDouble() ? -1 : 1;
    } else if (a.isExact() != b.isExact()) {
        return a.isExact() ? -1 : 1;
    }
    if (a.decimal_ != b.decimal_) {
        return a.decimal_ ? 1 : -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Functions

const char* functionName(MathFunction f)
{
    switch (f) {
    case MathFunction::Sin: return "sin";
    case MathFunction::Cos: return "cos";
    case MathFunction::Tan: return "tan";
    case MathFunction::Asin: return "asin";
    case MathFunction::Acos: return "acos";
    case MathFunction::Atan: return "atan";
    case MathFunction::Ln: return "ln";
    case MathFunction::Log: return "log";
    case MathFunction::Abs: return "abs";
    }
    return "";
}

std::optional<MathFunction> functionNamed(std::string_view name)
{
    for (MathFunction f : {MathFunction::Sin, MathFunction::Cos, MathFunction::Tan, MathFunction::Asin, MathFunction::Acos, MathFunction::Atan,
                       MathFunction::Ln, MathFunction::Log, MathFunction::Abs}) {
        if (name == functionName(f)) {
            return f;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Expressions

struct Expr::Data {
    Kind kind = Kind::Number;
    Number number; // Number; Sum: the constant; Product: the coefficient
    Symbol symbol;
    std::vector<ExprTerm> terms;
    std::vector<ExprFactor> factors;
    MathFunction function = MathFunction::Sin;
    std::string name;
    std::vector<Expr> args; // Function: its argument; Call: the arguments
};

namespace {

std::shared_ptr<Expr::Data> data(Expr::Kind kind)
{
    auto d = std::make_shared<Expr::Data>();
    d->kind = kind;
    return d;
}

bool isValue(const Expr& e, std::int64_t value)
{
    return e.isNumber() && e.number().isExact() && e.number().exact() == Rational(value);
}

// The exact number e is, if it is one.
std::optional<Rational> exactNumber(const Expr& e)
{
    if (e.isNumber() && e.number().isExact()) {
        return e.number().exact();
    }
    return std::nullopt;
}

void addTerm(std::vector<ExprTerm>& terms, const Expr& e, const Number& c)
{
    const auto it = std::lower_bound(terms.begin(), terms.end(), e,
                                     [](const ExprTerm& t, const Expr& x) { return compare(t.expr, x) < 0; });
    if (it != terms.end() && it->expr == e) {
        it->coefficient = it->coefficient + c;
    } else {
        terms.insert(it, {e, c});
    }
}

void addFactor(std::vector<ExprFactor>& factors, const Expr& base, const Expr& exponent)
{
    const auto it = std::lower_bound(factors.begin(), factors.end(), base,
                                     [](const ExprFactor& f, const Expr& x) { return compare(f.base, x) < 0; });
    if (it != factors.end() && it->base == base) {
        it->exponent = it->exponent + exponent;
    } else {
        factors.insert(it, {base, exponent});
    }
}

// The prime factors of n > 0, by trial division. A large cofactor left over is kept
// whole, as if it were prime.
std::vector<std::pair<std::int64_t, std::int64_t>> primeFactors(std::int64_t n)
{
    std::vector<std::pair<std::int64_t, std::int64_t>> primes;
    for (std::int64_t p = 2; p <= 1'000'000 && p <= n / p; ++p) {
        if (n % p == 0) {
            std::int64_t k = 0;
            while (n % p == 0) {
                n /= p;
                ++k;
            }
            primes.emplace_back(p, k);
        }
    }
    if (n > 1) {
        primes.emplace_back(n, 1);
    }
    return primes;
}

Expr finishProduct(Number coefficient, std::vector<ExprFactor> factors);

} // namespace

Expr::Expr() : Expr(Number(0)) {}

Expr::Expr(Number n)
{
    auto d = data(Kind::Number);
    d->number = n;
    data_ = std::move(d);
}

Expr Expr::symbol(Symbol s)
{
    auto d = data(Kind::Symbol);
    d->symbol = std::move(s);
    return Expr(std::move(d));
}

Expr::Kind Expr::kind() const { return data_->kind; }
const Number& Expr::number() const { return data_->number; }
const Symbol& Expr::symbol() const { return data_->symbol; }
const Number& Expr::constant() const { return data_->number; }
const std::vector<ExprTerm>& Expr::terms() const { return data_->terms; }
const Number& Expr::coefficient() const { return data_->number; }
const std::vector<ExprFactor>& Expr::factors() const { return data_->factors; }
MathFunction Expr::function() const { return data_->function; }
const Expr& Expr::argument() const { return data_->args.front(); }
const std::string& Expr::name() const { return data_->name; }
const std::vector<Expr>& Expr::arguments() const { return data_->args; }

Expr Expr::sum(const std::vector<Expr>& operands)
{
    Number constant;
    std::vector<ExprTerm> terms;
    for (const Expr& op : operands) {
        switch (op.kind()) {
        case Kind::Number:
            constant = constant + op.number();
            break;
        case Kind::Sum:
            constant = constant + op.constant();
            for (const ExprTerm& t : op.terms()) {
                addTerm(terms, t.expr, t.coefficient);
            }
            break;
        case Kind::Product:
            if (op.factors().size() == 1 && op.factors()[0].base.kind() == Kind::Sum
                && isValue(op.factors()[0].exponent, 1)) {
                // A multiple of a sum, in a sum, is multiplied out: 1 − (z + 3) is −z − 2.
                const Expr& inner = op.factors()[0].base;
                constant = constant + op.coefficient() * inner.constant();
                for (const ExprTerm& t : inner.terms()) {
                    addTerm(terms, t.expr, op.coefficient() * t.coefficient);
                }
                break;
            }
            // Split off the coefficient: 3 x² is the term x² with coefficient 3.
            addTerm(terms, finishProduct(Number(1), op.factors()), op.coefficient());
            break;
        default:
            addTerm(terms, op, Number(1));
            break;
        }
    }
    std::erase_if(terms, [](const ExprTerm& t) { return t.coefficient.isZero(); });
    if (terms.empty()) {
        return Expr(constant);
    }
    if (constant.isZero()) {
        if (terms.size() == 1) {
            return product({Expr(terms[0].coefficient), terms[0].expr});
        }
        constant = Number(0);
    }
    auto d = data(Kind::Sum);
    d->number = constant;
    d->terms = std::move(terms);
    return Expr(std::move(d));
}

Expr Expr::product(const std::vector<Expr>& operands)
{
    Number coefficient(1);
    std::vector<ExprFactor> factors;
    for (const Expr& op : operands) {
        switch (op.kind()) {
        case Kind::Number:
            coefficient = coefficient * op.number();
            break;
        case Kind::Product:
            coefficient = coefficient * op.coefficient();
            for (const ExprFactor& f : op.factors()) {
                addFactor(factors, f.base, f.exponent);
            }
            break;
        default:
            addFactor(factors, op, Expr(1));
            break;
        }
    }
    return finishProduct(coefficient, std::move(factors));
}

namespace {

// Builds coefficient · Π factors in canonical form. Factors may have zero exponents,
// and number bases with any exact exponent, which are folded or regrouped here.
Expr finishProduct(Number coefficient, std::vector<ExprFactor> factors)
{
    if (coefficient.isZero()) {
        return Expr(coefficient.decimal() ? Number(Rational(0), true) : Number(0));
    }

    // Number bases with exact exponents, by prime: √2 · √6 is 2 √3.
    std::vector<std::pair<std::int64_t, Rational>> primes;
    std::vector<ExprFactor> kept;
    for (ExprFactor& f : factors) {
        const auto exponent = exactNumber(f.exponent);
        if (exponent && exponent->isZero()) {
            continue;
        }
        if (f.base.isNumber() && exponent) {
            const Number& n = f.base.number();
            if (coefficient.decimal() || !n.isExact()) {
                // A decimal coefficient means calculator results: fold the root in.
                coefficient = coefficient * Number::approximate(std::pow(n.toDouble(), exponent->toDouble()));
                continue;
            }
            for (const auto& [p, k] : primeFactors(n.exact().num())) {
                const Rational add = Rational(k) * *exponent;
                const auto it = std::find_if(primes.begin(), primes.end(), [&](const auto& q) { return q.first == p; });
                if (it != primes.end()) {
                    it->second = it->second + add;
                } else {
                    primes.emplace_back(p, add);
                }
            }
            continue;
        }
        kept.push_back(std::move(f));
    }

    std::vector<std::pair<Rational, std::int64_t>> roots; // fractional exponent -> product of its primes
    for (const auto& [p, t] : primes) {
        const Rational whole = floorOf(t);
        coefficient = coefficient * powInt(Number(p), whole.num());
        const Rational fraction = t - whole;
        if (fraction.isZero()) {
            continue;
        }
        const auto it = std::find_if(roots.begin(), roots.end(), [&](const auto& r) { return r.first == fraction; });
        if (it == roots.end()) {
            roots.emplace_back(fraction, p);
        } else if (std::int64_t m = 0; mulInt(it->second, p, m)) {
            it->second = m;
        } else {
            roots.emplace_back(fraction, p);
        }
    }
    for (const auto& [fraction, base] : roots) {
        addFactor(kept, Expr(base), Expr(Number(fraction)));
    }

    if (kept.empty()) {
        return Expr(coefficient);
    }
    if (coefficient.isOne() && kept.size() == 1 && isValue(kept[0].exponent, 1)) {
        return kept[0].base;
    }
    auto d = data(Expr::Kind::Product);
    d->number = coefficient;
    d->factors = std::move(kept);
    return Expr(std::move(d));
}

// q^r for an exact number and a fractional exponent.
Expr rootOf(const Number& n, Rational r)
{
    Rational q = n.exact();
    if (q.isZero()) {
        if (r < Rational(0)) {
            throw Error("Division by zero");
        }
        return Expr(0);
    }
    Number sign(1);
    if (q < Rational(0)) {
        if (r.den() % 2 == 0) {
            throw Error("An even root of a negative number isn't real");
        }
        if (r.num() % 2 != 0) {
            sign = Number(-1);
        }
        q = -q;
    }
    std::vector<ExprFactor> factors{{Expr(q.num()), Expr(Number(r))}};
    if (q.den() != 1) {
        factors.push_back({Expr(q.den()), Expr(Number(-r))});
    }
    const Expr root = finishProduct(sign, std::move(factors));
    if (n.decimal()) {
        // A calculator's answer: a decimal, whether or not the root is exact.
        if (root.isNumber()) {
            return Expr(Number(root.number().exact(), true));
        }
        return Expr(Number::approximate(sign.toDouble() * std::pow(q.toDouble(), r.toDouble())));
    }
    return root;
}

bool isLn(const Expr& e)
{
    return e.kind() == Expr::Kind::Function && e.function() == MathFunction::Ln;
}

bool isE(const Expr& e)
{
    return e.kind() == Expr::Kind::Symbol && e.symbol().kind == Symbol::Kind::Constant && e.symbol().name == "e";
}

} // namespace

Expr Expr::pow(const Expr& base, const Expr& exponent)
{
    if (const auto r = exactNumber(exponent)) {
        if (r->isZero()) {
            return Expr(1);
        }
        if (*r == Rational(1)) {
            return base;
        }
        if (base.isNumber()) {
            const Number& n = base.number();
            if (!n.isExact()) {
                return Expr(Number::approximate(std::pow(n.toDouble(), r->toDouble())));
            }
            if (r->isInteger()) {
                if (n.isZero() && *r < Rational(0)) {
                    throw Error("Division by zero");
                }
                return Expr(powInt(n, r->num()));
            }
            return rootOf(n, *r);
        }
        if (base.kind() == Kind::Product) {
            const Number& c = base.coefficient();
            if (r->isInteger()) {
                std::vector<ExprFactor> factors;
                for (const ExprFactor& f : base.factors()) {
                    factors.push_back({f.base, f.exponent * exponent});
                }
                return finishProduct(powInt(c, r->num()), std::move(factors));
            }
            if (!c.isNegative()) {
                // (c Π bᵉ)^r = c^r Π b^(e r) for the bases that are positive, and with |b|
                // for an even e: √(4 x² (x − y)²) is 2 x |x − y|. Bases that may be negative
                // stay together under the root, since √(a b) isn't √a √b when both are.
                // A negative c stays inside too: (−(x − 3))^(1/3) is real where x < 3.
                std::vector<Expr> parts{pow(Expr(c), exponent)};
                std::vector<ExprFactor> rest;
                for (const ExprFactor& f : base.factors()) {
                    const auto e = exactNumber(f.exponent);
                    const bool even = e && e->isInteger() && e->num() % 2 == 0;
                    if (f.base.isPositive() || even) {
                        const Expr b = f.base.isPositive() ? f.base : apply(MathFunction::Abs, f.base);
                        parts.push_back(finishProduct(Number(1), {{b, f.exponent * exponent}}));
                    } else {
                        rest.push_back(f);
                    }
                }
                if (!rest.empty()) {
                    const Expr inner = finishProduct(Number(1), rest);
                    parts.push_back(finishProduct(Number(1), {{inner, exponent}}));
                }
                return product(parts);
            }
        }
        return finishProduct(Number(1), {{base, exponent}});
    }

    // An exponent that isn't a number.
    if (base.isNumber()) {
        const Number& n = base.number();
        if (n.isZero() || n.isOne()) {
            return base;
        }
        if (n.isExact() && !n.isNegative() && !n.exact().isInteger()) {
            // (p/q)^x is p^x q^-x, so that number bases are integers.
            return finishProduct(Number(1), {{Expr(n.exact().num()), exponent},
                                             {Expr(n.exact().den()), -exponent}});
        }
    }
    if (isE(base)) {
        // e^(c ln y) is y^c.
        if (isLn(exponent)) {
            return exponent.argument();
        }
        if (exponent.kind() == Kind::Product && exponent.factors().size() == 1
            && isLn(exponent.factors()[0].base) && isValue(exponent.factors()[0].exponent, 1)) {
            return pow(exponent.factors()[0].base.argument(), Expr(exponent.coefficient()));
        }
    }
    if (base.kind() == Kind::Product && base.coefficient().isOne()
        && std::all_of(base.factors().begin(), base.factors().end(),
                       [](const ExprFactor& f) { return f.base.isPositive(); })) {
        // (x² y)^n is x^(2n) y^n, since x and y are positive.
        std::vector<ExprFactor> factors;
        for (const ExprFactor& f : base.factors()) {
            factors.push_back({f.base, f.exponent * exponent});
        }
        return finishProduct(Number(1), std::move(factors));
    }
    return finishProduct(Number(1), {{base, exponent}});
}

namespace {

// sin(k π) for the angles with a known exact value: multiples of π/6 and π/4.
std::optional<Expr> sinOfPiTimes(Rational k)
{
    k = k - Rational(2) * floorOf(k / Rational(2)); // into [0, 2)
    if (!(k < Rational(1))) {
        const auto s = sinOfPiTimes(k - Rational(1));
        return s ? std::optional<Expr>(-*s) : std::nullopt;
    }
    if (k > Rational(1, 2)) {
        k = Rational(1) - k;
    }
    const Expr half(Number(Rational(1, 2)));
    if (k == Rational(0)) return Expr(0);
    if (k == Rational(1, 6)) return half;
    if (k == Rational(1, 4)) return half * Expr::pow(Expr(2), half);
    if (k == Rational(1, 3)) return half * Expr::pow(Expr(3), half);
    if (k == Rational(1, 2)) return Expr(1);
    return std::nullopt;
}

// The k of an angle k π, if the expression is one.
std::optional<Rational> piMultiple(const Expr& e)
{
    if (isValue(e, 0)) {
        return Rational(0);
    }
    if (e.kind() == Expr::Kind::Symbol && e.symbol().kind == Symbol::Kind::Constant && e.symbol().name == "π") {
        return Rational(1);
    }
    if (e.kind() == Expr::Kind::Product && e.coefficient().isExact() && !e.coefficient().decimal()
        && e.factors().size() == 1 && isValue(e.factors()[0].exponent, 1)) {
        if (piMultiple(e.factors()[0].base) == Rational(1)) {
            return e.coefficient().exact();
        }
    }
    return std::nullopt;
}

// True for a number below zero, or a product with a negative coefficient: −x.
bool looksNegative(const Expr& e)
{
    return (e.isNumber() && e.number().isNegative())
           || (e.kind() == Expr::Kind::Product && e.coefficient().isNegative());
}

double evaluateAt(MathFunction f, double x)
{
    switch (f) {
    case MathFunction::Sin: return std::sin(x);
    case MathFunction::Cos: return std::cos(x);
    case MathFunction::Tan: return std::tan(x);
    case MathFunction::Asin: return std::asin(x);
    case MathFunction::Acos: return std::acos(x);
    case MathFunction::Atan: return std::atan(x);
    case MathFunction::Ln: return std::log(x);
    case MathFunction::Log: return std::log10(x);
    case MathFunction::Abs: return std::abs(x);
    }
    return x;
}

} // namespace

Expr Expr::apply(MathFunction f, const Expr& argument)
{
    if (argument.isNumber()) {
        const Number& n = argument.number();
        const double value = evaluateAt(f, n.toDouble());
        if (!std::isfinite(value) || ((f == MathFunction::Ln || f == MathFunction::Log) && !(n.toDouble() > 0))) {
            throw Error(std::string(functionName(f)) + " has no real value here");
        }
        if (!n.isExact() || n.decimal()) {
            return Expr(Number::approximate(value));
        }
    }

    const Rational one(1);
    const Rational half(1, 2);
    const Expr pi = Expr::pi();
    switch (f) {
    case MathFunction::Abs:
        if (argument.isPositive()) {
            return argument;
        }
        if (argument.isNumber()) {
            return Expr(argument.number().abs());
        }
        if (looksNegative(argument)) {
            return apply(f, -argument);
        }
        break;
    case MathFunction::Sin:
    case MathFunction::Cos:
    case MathFunction::Tan:
        if (const auto k = piMultiple(argument)) {
            const auto s = sinOfPiTimes(*k);
            const auto c = sinOfPiTimes(*k + half);
            if (s && c) {
                if (f == MathFunction::Sin) return *s;
                if (f == MathFunction::Cos) return *c;
                if (isValue(*c, 0)) {
                    throw Error("tan has no real value here");
                }
                return *s / *c;
            }
        }
        if (looksNegative(argument)) {
            // sin and tan are odd, cos is even.
            const Expr positive = apply(f, -argument);
            return f == MathFunction::Cos ? positive : -positive;
        }
        break;
    case MathFunction::Asin:
    case MathFunction::Atan:
        if (looksNegative(argument)) {
            return -apply(f, -argument);
        }
        [[fallthrough]];
    case MathFunction::Acos:
        if (const auto x = exactNumber(argument)) {
            if (x->isZero()) return f == MathFunction::Acos ? pi / Expr(2) : Expr(0);
            if (*x == one) return f == MathFunction::Asin ? pi / Expr(2) : f == MathFunction::Acos ? Expr(0) : pi / Expr(4);
            if (*x == half && f != MathFunction::Atan) return f == MathFunction::Asin ? pi / Expr(6) : pi / Expr(3);
        }
        break;
    case MathFunction::Ln:
        if (isValue(argument, 1)) {
            return Expr(0);
        }
        if (isE(argument)) {
            return Expr(1);
        }
        if (argument.kind() == Kind::Product && argument.coefficient().isOne() && argument.factors().size() == 1
            && isE(argument.factors()[0].base)) {
            return argument.factors()[0].exponent; // ln(eˣ) is x
        }
        break;
    case MathFunction::Log:
        if (const auto x = exactNumber(argument)) {
            // log of a power of ten
            for (const bool invert : {false, true}) {
                std::int64_t m = invert ? x->den() : x->num();
                if ((invert ? x->num() : x->den()) != 1) {
                    continue;
                }
                std::int64_t k = 0;
                while (m > 1 && m % 10 == 0) {
                    m /= 10;
                    ++k;
                }
                if (m == 1) {
                    return Expr(invert ? -k : k);
                }
            }
        }
        break;
    }

    auto d = data(Kind::Function);
    d->function = f;
    d->args = {argument};
    return Expr(std::move(d));
}

Expr Expr::call(std::string name, std::vector<Expr> arguments)
{
    auto d = data(Kind::Call);
    d->name = std::move(name);
    d->args = std::move(arguments);
    return Expr(std::move(d));
}

int compare(const Expr& a, const Expr& b)
{
    if (a.data_ == b.data_) {
        return 0;
    }
    if (a.kind() != b.kind()) {
        return a.kind() < b.kind() ? -1 : 1;
    }
    const auto sequence = [](const auto& xs, const auto& ys, auto item) {
        for (std::size_t i = 0; i < xs.size() && i < ys.size(); ++i) {
            if (const int c = item(xs[i], ys[i])) {
                return c;
            }
        }
        return xs.size() == ys.size() ? 0 : xs.size() < ys.size() ? -1 : 1;
    };
    switch (a.kind()) {
    case Expr::Kind::Number:
        return compare(a.number(), b.number());
    case Expr::Kind::Symbol:
        if (a.symbol().kind != b.symbol().kind) {
            return a.symbol().kind < b.symbol().kind ? -1 : 1;
        }
        return a.symbol().name.compare(b.symbol().name) < 0 ? -1 : a.symbol().name == b.symbol().name ? 0 : 1;
    case Expr::Kind::Sum:
        if (const int c = sequence(a.terms(), b.terms(), [](const ExprTerm& x, const ExprTerm& y) {
                if (const int e = compare(x.expr, y.expr)) {
                    return e;
                }
                return compare(x.coefficient, y.coefficient);
            })) {
            return c;
        }
        return compare(a.constant(), b.constant());
    case Expr::Kind::Product:
        if (const int c = sequence(a.factors(), b.factors(), [](const ExprFactor& x, const ExprFactor& y) {
                if (const int e = compare(x.base, y.base)) {
                    return e;
                }
                return compare(x.exponent, y.exponent);
            })) {
            return c;
        }
        return compare(a.coefficient(), b.coefficient());
    case Expr::Kind::Function:
        if (a.function() != b.function()) {
            return a.function() < b.function() ? -1 : 1;
        }
        return compare(a.argument(), b.argument());
    case Expr::Kind::Call:
        if (a.name() != b.name()) {
            return a.name() < b.name() ? -1 : 1;
        }
        return sequence(a.arguments(), b.arguments(), [](const Expr& x, const Expr& y) { return compare(x, y); });
    }
    return 0;
}

bool Expr::isPositive() const
{
    switch (kind()) {
    case Kind::Number:
        return !number().isNegative() && !number().isZero();
    case Kind::Symbol:
        return true;
    case Kind::Sum:
        return !constant().isNegative() && std::all_of(terms().begin(), terms().end(), [](const ExprTerm& t) {
                   return !t.coefficient.isNegative() && t.expr.isPositive();
               });
    case Kind::Product:
        return !coefficient().isNegative() && std::all_of(factors().begin(), factors().end(), [](const ExprFactor& f) {
                   const auto e = exactNumber(f.exponent);
                   return f.base.isPositive() || (e && e->isInteger() && e->num() % 2 == 0);
               });
    case Kind::Function:
        return function() == MathFunction::Abs;
    case Kind::Call:
        return false;
    }
    return false;
}

namespace {

std::string numberText(const Number& n)
{
    if (!n.isExact()) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.6g", n.toDouble());
        return buf;
    }
    return n.exact().toString();
}

std::string debugText(const Expr& e, bool tight)
{
    std::string s;
    switch (e.kind()) {
    case Expr::Kind::Number:
        s = numberText(e.number());
        return tight && (!e.number().isInteger() || e.number().isNegative()) ? "(" + s + ")" : s;
    case Expr::Kind::Symbol:
        return e.symbol().name;
    case Expr::Kind::Sum:
        for (const ExprTerm& t : e.terms()) {
            s += (s.empty() ? "" : " + ") + debugText(Expr::product({Expr(t.coefficient), t.expr}), false);
        }
        if (!e.constant().isZero()) {
            s += " + " + numberText(e.constant());
        }
        return tight ? "(" + s + ")" : s;
    case Expr::Kind::Product:
        if (!e.coefficient().isOne()) {
            s = debugText(Expr(e.coefficient()), true);
        }
        for (const ExprFactor& f : e.factors()) {
            s += (s.empty() ? "" : "*") + debugText(f.base, true);
            if (!isValue(f.exponent, 1)) {
                s += "^" + debugText(f.exponent, true);
            }
        }
        return tight ? "(" + s + ")" : s;
    case Expr::Kind::Function:
        return std::string(functionName(e.function())) + "(" + debugText(e.argument(), false) + ")";
    case Expr::Kind::Call:
        s = e.name() + "(";
        for (std::size_t i = 0; i < e.arguments().size(); ++i) {
            s += (i ? ", " : "") + debugText(e.arguments()[i], false);
        }
        return s + ")";
    }
    return s;
}

} // namespace

std::string Expr::debugString() const
{
    return debugText(*this, false);
}

} // namespace lemma
