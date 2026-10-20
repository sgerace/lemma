#include "lemma/symbolic_bridge.h"

#include "lemma/error.h"
#include "lemma/evaluator.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <tuple>

namespace lemma {

// ---------------------------------------------------------------------------
// Tree to expression

namespace {

// A number as typed, exactly: "2.5" is 5/2, marked decimal (section 5.2), as is
// anything in scientific notation. Too many digits to be exact gives a double.
Number parseNumber(const MathNode& m)
{
    const std::string& t = m.text;
    std::size_t i = 0;
    std::string digits;
    std::int64_t fractionDigits = 0;
    bool point = false;
    for (; i < t.size() && (std::isdigit(static_cast<unsigned char>(t[i])) || t[i] == '.'); ++i) {
        if (t[i] == '.') {
            if (point) {
                throw Error("This number is incomplete", 0, 0, m.id);
            }
            point = true;
        } else {
            digits += t[i];
            fractionDigits += point ? 1 : 0;
        }
    }
    std::int64_t exponent = 0;
    bool scientific = false;
    if (i < t.size() && (t[i] == 'e' || t[i] == 'E')) {
        scientific = true;
        ++i;
        const bool negative = i < t.size() && t[i] == '-';
        if (i < t.size() && (t[i] == '-' || t[i] == '+')) {
            ++i;
        }
        const std::size_t start = i;
        for (; i < t.size() && std::isdigit(static_cast<unsigned char>(t[i])) && i - start < 6; ++i) {
            exponent = exponent * 10 + (t[i] - '0');
        }
        if (i == start) {
            throw Error("This number is incomplete", 0, 0, m.id);
        }
        exponent = negative ? -exponent : exponent;
    }
    if (digits.empty() || i != t.size()) {
        throw Error("This number is incomplete", 0, 0, m.id);
    }

    const bool decimal = point || scientific;
    const std::size_t first = std::min(digits.find_first_not_of('0'), digits.size());
    const std::string significant = digits.substr(first);
    exponent -= fractionDigits;
    if (significant.empty()) {
        return Number(Rational(0), decimal);
    }
    if (significant.size() <= 18 && std::abs(exponent) <= 18) {
        Number n(Rational(std::stoll(significant)), decimal);
        Number scale(Rational(1), decimal);
        for (std::int64_t k = 0; k < std::abs(exponent); ++k) {
            scale = scale * Number(10);
        }
        if (scale.isExact()) {
            return exponent < 0 ? n / scale : n * scale;
        }
    }
    const double value = std::strtod(t.c_str(), nullptr);
    if (!std::isfinite(value)) {
        throw Error("Number is out of range", 0, 0, m.id);
    }
    return Number::approximate(value);
}

Expr lowerName(const std::string& name)
{
    if (name == "π" || name == "pi") {
        return Expr::pi();
    }
    if (name == "e") {
        return Expr::e();
    }
    return Expr::variable(name);
}

Expr lower(const MathNode& m)
{
    const auto& c = m.children;
    try {
        switch (m.kind) {
        case MathNode::Kind::Number:
            return Expr(parseNumber(m));
        case MathNode::Kind::Identifier:
            return lowerName(m.text);
        case MathNode::Kind::Binary:
            switch (m.op) {
            case '+': return lower(c[0]) + lower(c[1]);
            case '-': return lower(c[0]) - lower(c[1]);
            case '*': return lower(c[0]) * lower(c[1]);
            default: throw Error("Fill in the placeholder", 0, 0, m.id);
            }
        case MathNode::Kind::Negate:
            return -lower(c[0]);
        case MathNode::Kind::Fraction:
            return lower(c[0]) / lower(c[1]);
        case MathNode::Kind::Power:
            return Expr::pow(lower(c[0]), lower(c[1]));
        case MathNode::Kind::Radical:
            return Expr::pow(lower(c[0]), Expr(Number(Rational(1, 2))));
        case MathNode::Kind::Group:
            return lower(c[0]);
        case MathNode::Kind::Call: {
            if (c[0].kind != MathNode::Kind::Identifier) {
                throw Error("Fill in the placeholder", 0, 0, c[0].id);
            }
            const std::string& name = c[0].text;
            std::vector<Expr> args;
            for (std::size_t i = 1; i < c.size(); ++i) {
                args.push_back(lower(c[i]));
            }
            const bool unary = name == "sqrt" || name == "exp" || functionNamed(name).has_value();
            if (unary && args.size() != 1) {
                throw Error(name + " takes 1 argument", 0, 0, m.id);
            }
            if (name == "sqrt") {
                return Expr::pow(args[0], Expr(Number(Rational(1, 2))));
            }
            if (name == "exp") {
                return Expr::pow(Expr::e(), args[0]);
            }
            if (const auto f = functionNamed(name)) {
                return Expr::apply(*f, args[0]);
            }
            return Expr::call(name, std::move(args)); // min, max, and undefined functions
        }
        case MathNode::Kind::Placeholder:
            throw Error("Fill in the placeholder", 0, 0, m.id);
        case MathNode::Kind::Definition:
        case MathNode::Kind::Evaluation:
            break;
        }
        throw Error("Only a whole region can be a definition or evaluation", 0, 0, m.id);
    } catch (const Error& e) {
        if (e.node() != 0) {
            throw;
        }
        // From building the expression: the error belongs to this node.
        throw Error(e.what(), 0, 0, m.id);
    }
}

void collectNames(const MathNode& m, std::vector<std::string>& names)
{
    if (m.kind == MathNode::Kind::Identifier) {
        const std::string name = m.text == "pi" ? "π" : m.text;
        if (std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(name);
        }
        return;
    }
    for (std::size_t i = 0; i < m.children.size(); ++i) {
        if (m.kind == MathNode::Kind::Call && i == 0) {
            continue; // a function's name
        }
        collectNames(m.children[i], names);
    }
}

} // namespace

Expr exprFromMath(const MathNode& node)
{
    return lower(node);
}

std::vector<std::string> namesInOrder(const MathNode& node)
{
    std::vector<std::string> names;
    collectNames(node, names);
    return names;
}

// ---------------------------------------------------------------------------
// Expression to tree

namespace {

// UTF-8 superscript digits back to an integer, for "×10⁻³".
int fromSuperscript(std::string_view s)
{
    static const char* const kDigits[] = {"⁰", "¹", "²", "³", "⁴", "⁵", "⁶", "⁷", "⁸", "⁹"};
    int sign = 1;
    int value = 0;
    while (!s.empty()) {
        if (s.starts_with("⁻")) {
            sign = -1;
            s.remove_prefix(std::string_view("⁻").size());
            continue;
        }
        bool matched = false;
        for (int d = 0; d < 10; ++d) {
            if (s.starts_with(kDigits[d])) {
                value = value * 10 + d;
                s.remove_prefix(std::string_view(kDigits[d]).size());
                matched = true;
                break;
            }
        }
        if (!matched) {
            break;
        }
    }
    return sign * value;
}

MathNode juxtapose(std::vector<MathNode> nodes)
{
    MathNode result = std::move(nodes.front());
    for (std::size_t i = 1; i < nodes.size(); ++i) {
        // "3 x", but "3·2^(1/3)": a number after another factor would read as one number.
        const MathNode& next = nodes[i];
        const bool number = next.kind == MathNode::Kind::Number
                            || (next.kind == MathNode::Kind::Power && next.children[0].kind == MathNode::Kind::Number);
        result = MathNode::binary('*', std::move(result), std::move(nodes[i]), !number);
    }
    return result;
}

// How often each variable occurs, and its highest degree, to choose the main variable.
struct Usage {
    std::string name;
    Rational degree;
    int count = 0;
};

void collectUsage(const Expr& e, std::vector<Usage>& usage)
{
    const auto note = [&](const std::string& name, Rational degree) {
        auto it = std::find_if(usage.begin(), usage.end(), [&](const Usage& u) { return u.name == name; });
        if (it == usage.end()) {
            usage.push_back({name, degree, 1});
        } else {
            it->degree = degree > it->degree ? degree : it->degree;
            ++it->count;
        }
    };
    switch (e.kind()) {
    case Expr::Kind::Number:
        break;
    case Expr::Kind::Symbol:
        if (e.symbol().kind == Symbol::Kind::Variable) {
            note(e.symbol().name, Rational(1));
        }
        break;
    case Expr::Kind::Sum:
        for (const ExprTerm& t : e.terms()) {
            collectUsage(t.expr, usage);
        }
        break;
    case Expr::Kind::Product:
        for (const ExprFactor& f : e.factors()) {
            if (f.base.kind() == Expr::Kind::Symbol && f.base.symbol().kind == Symbol::Kind::Variable
                && f.exponent.isNumber() && f.exponent.number().isExact()) {
                note(f.base.symbol().name, f.exponent.number().exact());
            } else {
                collectUsage(f.base, usage);
                collectUsage(f.exponent, usage);
            }
        }
        break;
    case Expr::Kind::Function:
        collectUsage(e.argument(), usage);
        break;
    case Expr::Kind::Call:
        for (const Expr& a : e.arguments()) {
            collectUsage(a, usage);
        }
        break;
    }
}

class Raiser {
public:
    Raiser(const Expr& e, const ExprDisplayOptions& options) : options_(options)
    {
        // The main variable: of highest degree, then most used, then written last. It
        // goes after the other names in a product: a x², k t.
        std::vector<Usage> usage;
        collectUsage(e, usage);
        const Usage* best = nullptr;
        for (const Usage& u : usage) {
            if (!best || u.degree > best->degree || (u.degree == best->degree && u.count > best->count)
                || (u.degree == best->degree && u.count == best->count && appearance(u.name) >= appearance(best->name))) {
                best = &u;
            }
        }
        if (best) {
            main_ = best->name;
        }
    }

    MathNode raise(const Expr& e) const
    {
        switch (e.kind()) {
        case Expr::Kind::Number:
            return number(e.number());
        case Expr::Kind::Symbol:
            return MathNode::identifier(e.symbol().name);
        case Expr::Kind::Sum:
            return sum(e);
        case Expr::Kind::Product: {
            const Number& c = e.coefficient();
            MathNode p = product(c.abs(), e.factors());
            return c.isNegative() ? MathNode(MathNode::Kind::Negate, {std::move(p)}) : p;
        }
        case Expr::Kind::Function:
            return MathNode(MathNode::Kind::Call,
                            {MathNode::identifier(functionName(e.function())), raise(e.argument())});
        case Expr::Kind::Call: {
            std::vector<MathNode> children{MathNode::identifier(e.name())};
            for (const Expr& a : e.arguments()) {
                children.push_back(raise(a));
            }
            return MathNode(MathNode::Kind::Call, std::move(children));
        }
        }
        return MathNode::number("0");
    }

private:
    MathNode number(const Number& n) const
    {
        if (n.isNegative()) {
            return MathNode(MathNode::Kind::Negate, {number(-n)});
        }
        if (!n.isExact() || n.decimal()) {
            const std::string text = formatNumber(n.toDouble(), options_.format);
            const auto times = text.find("×10");
            if (times == std::string::npos) {
                return MathNode::number(text);
            }
            const int exponent = fromSuperscript(std::string_view(text).substr(times + std::string("×10").size()));
            MathNode power(MathNode::Kind::Power,
                           {MathNode::number("10"),
                            exponent < 0 ? MathNode(MathNode::Kind::Negate, {MathNode::number(std::to_string(-exponent))})
                                         : MathNode::number(std::to_string(exponent))});
            return MathNode::binary('*', MathNode::number(text.substr(0, times)), std::move(power));
        }
        const Rational& r = n.exact();
        if (r.isInteger()) {
            return MathNode::number(std::to_string(r.num()));
        }
        return MathNode(MathNode::Kind::Fraction,
                        {MathNode::number(std::to_string(r.num())), MathNode::number(std::to_string(r.den()))});
    }

    // Where a name first appears in the region, for ordering.
    std::size_t appearance(const std::string& name) const
    {
        const auto it = std::find(options_.order.begin(), options_.order.end(), name);
        return static_cast<std::size_t>(it - options_.order.begin());
    }

    std::size_t firstAppearance(const Expr& e) const
    {
        switch (e.kind()) {
        case Expr::Kind::Number:
            return std::numeric_limits<std::size_t>::max();
        case Expr::Kind::Symbol:
            return appearance(e.symbol().name);
        case Expr::Kind::Sum: {
            std::size_t first = std::numeric_limits<std::size_t>::max();
            for (const ExprTerm& t : e.terms()) {
                first = std::min(first, firstAppearance(t.expr));
            }
            return first;
        }
        case Expr::Kind::Product: {
            std::size_t first = std::numeric_limits<std::size_t>::max();
            for (const ExprFactor& f : e.factors()) {
                first = std::min({first, firstAppearance(f.base), firstAppearance(f.exponent)});
            }
            return first;
        }
        case Expr::Kind::Function:
            return firstAppearance(e.argument());
        case Expr::Kind::Call: {
            std::size_t first = std::numeric_limits<std::size_t>::max();
            for (const Expr& a : e.arguments()) {
                first = std::min(first, firstAppearance(a));
            }
            return first;
        }
        }
        return 0;
    }

    // Numbers first, then constants, then names in the order they first appear (the main
    // variable last), then roots, functions and sums (section 11): 2 π r, a x², 2 x √y,
    // P x (2 L − x).
    std::tuple<int, std::size_t> rank(const ExprFactor& f) const
    {
        const Expr& base = f.base;
        switch (base.kind()) {
        case Expr::Kind::Number:
            return {0, 0};
        case Expr::Kind::Symbol: {
            if (base.symbol().kind == Symbol::Kind::Constant) {
                return {1, 0};
            }
            const bool root = f.exponent.isNumber() && !f.exponent.number().isInteger();
            const std::size_t order = base.symbol().name == main_ && !root ? std::numeric_limits<std::size_t>::max()
                                                                          : appearance(base.symbol().name);
            return {root ? 3 : 2, order};
        }
        case Expr::Kind::Sum:
            return {5, firstAppearance(base)};
        default:
            return {4, firstAppearance(base)};
        }
    }

    void sortFactors(std::vector<ExprFactor>& factors) const
    {
        std::stable_sort(factors.begin(), factors.end(), [this](const ExprFactor& a, const ExprFactor& b) {
            return rank(a) < rank(b);
        });
    }

    MathNode factor(const ExprFactor& f) const
    {
        if (f.exponent.isNumber() && f.exponent.number().isExact()) {
            const Rational& r = f.exponent.number().exact();
            if (r == Rational(1)) {
                return raise(f.base);
            }
            if (r == Rational(1, 2)) {
                return MathNode(MathNode::Kind::Radical, {raise(f.base)});
            }
        }
        return MathNode(MathNode::Kind::Power, {raise(f.base), raise(f.exponent)});
    }

    // coefficient · factors, for a coefficient that isn't negative: the factors with
    // negative exponents form the denominator.
    MathNode product(const Number& coefficient, const std::vector<ExprFactor>& factors) const
    {
        std::vector<ExprFactor> up;
        std::vector<ExprFactor> down;
        for (const ExprFactor& f : factors) {
            if (f.exponent.isNumber() && f.exponent.number().isNegative()) {
                down.push_back({f.base, Expr(-f.exponent.number())});
            } else {
                up.push_back(f);
            }
        }
        sortFactors(up);
        sortFactors(down);

        std::vector<MathNode> top;
        std::vector<MathNode> bottom;
        if (!coefficient.isExact() || coefficient.decimal()) {
            if (!coefficient.isOne() || up.empty()) {
                top.push_back(number(coefficient));
            }
        } else {
            const Rational& r = coefficient.exact();
            if (r.num() != 1 || up.empty()) {
                top.push_back(MathNode::number(std::to_string(r.num())));
            }
            if (r.den() != 1) {
                bottom.push_back(MathNode::number(std::to_string(r.den())));
            }
        }
        for (const ExprFactor& f : up) {
            top.push_back(factor(f));
        }
        for (const ExprFactor& f : down) {
            bottom.push_back(factor(f));
        }
        if (top.size() > 1 && top.front().kind == MathNode::Kind::Number && top.front().text == "1") {
            top.erase(top.begin());
        }
        MathNode numerator = juxtapose(std::move(top));
        if (bottom.empty()) {
            return numerator;
        }
        return MathNode(MathNode::Kind::Fraction, {std::move(numerator), juxtapose(std::move(bottom))});
    }

    // A term, c · expr, shown without its sign.
    MathNode term(const Number& c, const Expr& expr) const
    {
        if (expr.kind() == Expr::Kind::Product) {
            return product(c.abs() * expr.coefficient(), expr.factors());
        }
        return product(c.abs(), {{expr, Expr(1)}});
    }

    static bool mentions(const Expr& e, const std::string& name)
    {
        switch (e.kind()) {
        case Expr::Kind::Number:
            return false;
        case Expr::Kind::Symbol:
            return e.symbol().name == name;
        case Expr::Kind::Sum:
            return std::any_of(e.terms().begin(), e.terms().end(),
                               [&](const ExprTerm& t) { return mentions(t.expr, name); });
        case Expr::Kind::Product:
            return std::any_of(e.factors().begin(), e.factors().end(), [&](const ExprFactor& f) {
                return mentions(f.base, name) || mentions(f.exponent, name);
            });
        case Expr::Kind::Function:
            return mentions(e.argument(), name);
        case Expr::Kind::Call:
            return std::any_of(e.arguments().begin(), e.arguments().end(),
                               [&](const Expr& a) { return mentions(a, name); });
        }
        return false;
    }

    static Rational degree(const Expr& term, const std::string& name)
    {
        if (term.kind() == Expr::Kind::Symbol && term.symbol().name == name) {
            return Rational(1);
        }
        if (term.kind() == Expr::Kind::Product) {
            for (const ExprFactor& f : term.factors()) {
                if (f.base.kind() == Expr::Kind::Symbol && f.base.symbol().name == name && f.exponent.isNumber()
                    && f.exponent.number().isExact()) {
                    return f.exponent.number().exact();
                }
            }
        }
        return Rational(0);
    }

    // A polynomial in one main variable (the one of highest degree) in descending
    // powers: x² − 5 x + 6. Otherwise, terms in the order their names first appear,
    // those with the main variable last: 2 L − x. The constant goes last.
    MathNode sum(const Expr& e) const
    {
        std::vector<ExprTerm> terms = e.terms();
        std::string main;
        Rational highest(1);
        for (const std::string& name : options_.order) {
            for (const ExprTerm& t : terms) {
                if (degree(t.expr, name) > highest) {
                    highest = degree(t.expr, name);
                    main = name;
                }
            }
        }
        std::stable_sort(terms.begin(), terms.end(), [&](const ExprTerm& a, const ExprTerm& b) {
            if (!main.empty()) {
                const Rational da = degree(a.expr, main);
                const Rational db = degree(b.expr, main);
                if (!(da == db)) {
                    return da > db;
                }
            } else if (!main_.empty()) {
                const bool ma = mentions(a.expr, main_);
                const bool mb = mentions(b.expr, main_);
                if (ma != mb) {
                    return mb;
                }
            }
            return firstAppearance(a.expr) < firstAppearance(b.expr);
        });

        std::vector<std::pair<MathNode, bool>> parts; // a term and whether it's subtracted
        for (const ExprTerm& t : terms) {
            const bool negative = t.coefficient.isNegative() != (t.expr.kind() == Expr::Kind::Product
                                                                 && t.expr.coefficient().isNegative());
            parts.emplace_back(term(t.coefficient, t.expr), negative);
        }
        if (!e.constant().isZero()) {
            parts.emplace_back(number(e.constant().abs()), e.constant().isNegative());
        }

        // A sum of negative terms is negated as a whole: −(a + b).
        const bool allNegative =
            std::all_of(parts.begin(), parts.end(), [](const auto& p) { return p.second; });
        MathNode result = std::move(parts[0].first);
        if (parts[0].second && !allNegative) {
            result = MathNode(MathNode::Kind::Negate, {std::move(result)});
        }
        for (std::size_t i = 1; i < parts.size(); ++i) {
            const char op = parts[i].second && !allNegative ? '-' : '+';
            result = MathNode::binary(op, std::move(result), std::move(parts[i].first));
        }
        return allNegative ? MathNode(MathNode::Kind::Negate, {std::move(result)}) : result;
    }

    const ExprDisplayOptions& options_;
    std::string main_;
};

} // namespace

MathNode mathFromExpr(const Expr& e, const ExprDisplayOptions& options)
{
    return Raiser(e, options).raise(e);
}

} // namespace lemma
