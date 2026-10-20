#pragma once

#include "lemma/dimension.h"
#include "lemma/rational.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lemma {

// A number in a symbolic expression (docs/symbolic-spec.md, section 5.2): exact while it
// fits in a Rational, otherwise a double. `decimal` marks one typed with a decimal point,
// or computed from one, so that it's shown as a decimal.
class Number {
public:
    Number() = default;
    Number(std::int64_t value) : exact_(Rational(value)) {}
    Number(Rational value, bool decimal = false) : exact_(value), decimal_(decimal) {}
    // An inexact number, always shown as a decimal.
    static Number approximate(double value);

    bool isExact() const { return exact_.has_value(); }
    const Rational& exact() const { return *exact_; } // requires isExact()
    double toDouble() const { return exact_ ? exact_->toDouble() : approx_; }
    bool decimal() const { return decimal_; }

    bool isZero() const { return exact_ ? exact_->isZero() : approx_ == 0.0; }
    bool isOne() const { return exact_ && *exact_ == Rational(1); }
    bool isNegative() const { return exact_ ? *exact_ < Rational(0) : approx_ < 0.0; }
    bool isInteger() const { return exact_ && exact_->isInteger(); }

    Number abs() const { return isNegative() ? -*this : *this; }

    friend Number operator+(const Number& a, const Number& b);
    friend Number operator-(const Number& a, const Number& b) { return a + -b; }
    friend Number operator*(const Number& a, const Number& b);
    // Throws lemma::Error when dividing by zero.
    friend Number operator/(const Number& a, const Number& b);
    Number operator-() const;

    // Ordered by value, then exact before inexact, then not decimal before decimal.
    friend int compare(const Number& a, const Number& b);
    friend bool operator==(const Number& a, const Number& b) { return compare(a, b) == 0; }

private:
    std::optional<Rational> exact_ = Rational(0);
    double approx_ = 0.0;
    bool decimal_ = false;
};

// A name in an expression. A variable is a positive real unless the `real` keyword
// says it may be negative (section 5.3). Units and constants have a dimension and a
// size in SI base units (section 8); a variable's dimension is unknown.
struct Symbol {
    enum class Kind {
        Variable,
        Constant, // π, e, and g (gravity)
        Unit,     // m, kN, ...
    };

    std::string name;
    Kind kind = Kind::Variable;
    bool positive = true;
    Dimension dimension;
    double size = 1.0; // a unit's or constant's value in SI base units

    friend bool operator==(const Symbol&, const Symbol&) = default;
};

// The elementary functions an expression knows rules for. sqrt and exp are powers.
enum class MathFunction { Sin, Cos, Tan, Asin, Acos, Atan, Ln, Log, Abs };

const char* functionName(MathFunction f);
std::optional<MathFunction> functionNamed(std::string_view name);

struct ExprTerm;
struct ExprFactor;

// A symbolic expression, always in canonical form (section 5.2): numbers fold, like
// terms and like factors combine, and terms and factors are kept in a fixed order, so
// equal expressions compare equal. Immutable, and cheap to copy.
//
// Construction throws lemma::Error (with no node) for what has no value: a division by
// zero, an even root of a negative number, or a sum of terms whose units don't match.
class Expr {
public:
    enum class Kind {
        Number,
        Symbol,
        Sum,      // constant() + Σ coefficient · expr, with two or more parts
        Product,  // coefficient() · Π base^exponent, and not just one base to the first power
        Function, // function() of argument()
        Call,     // an undefined function, kept as is: name() and arguments()
    };

    Expr(); // 0
    Expr(Number n);
    Expr(std::int64_t n) : Expr(Number(n)) {}

    static Expr symbol(Symbol s);
    // A positive variable, or with `positive` false, a real one of either sign.
    static Expr variable(std::string name, bool positive = true)
    {
        Symbol s;
        s.name = std::move(name);
        s.positive = positive;
        return symbol(std::move(s));
    }
    static Expr constant(std::string name, const Dimension& dimension, double size)
    {
        Symbol s{std::move(name), Symbol::Kind::Constant, true, dimension, size};
        return symbol(std::move(s));
    }
    static Expr pi() { return constant("π", {}, 3.141592653589793); }
    static Expr e() { return constant("e", {}, 2.718281828459045); }
    static Expr unit(std::string name, const Dimension& dimension, double size)
    {
        Symbol s{std::move(name), Symbol::Kind::Unit, true, dimension, size};
        return symbol(std::move(s));
    }

    static Expr sum(const std::vector<Expr>& operands);
    static Expr product(const std::vector<Expr>& operands);
    static Expr pow(const Expr& base, const Expr& exponent);
    static Expr apply(MathFunction f, const Expr& argument);
    static Expr call(std::string name, std::vector<Expr> arguments);

    friend Expr operator+(const Expr& a, const Expr& b) { return sum({a, b}); }
    friend Expr operator-(const Expr& a, const Expr& b) { return sum({a, -b}); }
    friend Expr operator*(const Expr& a, const Expr& b) { return product({a, b}); }
    friend Expr operator/(const Expr& a, const Expr& b) { return product({a, pow(b, Expr(-1))}); }
    Expr operator-() const { return product({Expr(-1), *this}); }

    Kind kind() const;
    bool isNumber() const { return kind() == Kind::Number; }

    const Number& number() const;                // Number
    const Symbol& symbol() const;                // Symbol
    const Number& constant() const;              // Sum
    const std::vector<ExprTerm>& terms() const;  // Sum, in canonical order
    const Number& coefficient() const;           // Product
    const std::vector<ExprFactor>& factors() const; // Product, in canonical order
    MathFunction function() const;                   // Function
    const Expr& argument() const;                // Function
    const std::string& name() const;             // Call
    const std::vector<Expr>& arguments() const;  // Call

    // A total order on expressions: the order canonical form keeps terms and factors in.
    friend int compare(const Expr& a, const Expr& b);
    friend bool operator==(const Expr& a, const Expr& b) { return compare(a, b) == 0; }

    // True when the expression is positive for every value of its symbols.
    bool isPositive() const;
    // Its dimension, if known: unknown when it has a variable in it.
    std::optional<Dimension> dimension() const;

    // Canonical linear text, for tests and messages: "3*x^2 + y".
    std::string debugString() const;

    struct Data;

    explicit Expr(std::shared_ptr<const Data> data) : data_(std::move(data)) {}

private:
    std::shared_ptr<const Data> data_;
};

// A term of a Sum: coefficient · expr, where expr is not a number and has coefficient 1.
struct ExprTerm {
    Expr expr;
    Number coefficient;
};

// The expression with each symbol `replace` gives a value for replaced, rebuilt in
// canonical form.
Expr replaceSymbols(const Expr& e, const std::function<std::optional<Expr>(const Symbol&)>& replace);

// A unit's or constant's size as a number: exact when it's a simple fraction (mm is
// 1/1000 m), otherwise a decimal. A degree is π/180.
Expr sizeOf(const Symbol& s);

// A factor of a Product: base^exponent. A number base is a positive integer with an
// exponent that isn't one.
struct ExprFactor {
    Expr base;
    Expr exponent;
};

} // namespace lemma
