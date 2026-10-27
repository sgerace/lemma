#include "lemma/symbolic_bridge.h"

#include "lemma/error.h"
#include "lemma/evaluator.h"
#include "lemma/symbolic_ops.h"
#include "lemma/units.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <tuple>
#include <unordered_set>

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

// A tree's leaves in reading order, with what joins each to the one before: a product
// (·, juxtaposition, a fraction or an exponent) or anything else (+, −, a comma).
struct Reading {
    const MathNode* leaf;
    bool multiplied; // joined to the leaf before by multiplication
};

void read(const MathNode& n, std::vector<Reading>& out, bool& multiplied)
{
    const auto& c = n.children;
    switch (n.kind) {
    case MathNode::Kind::Number:
    case MathNode::Kind::Identifier:
    case MathNode::Kind::Placeholder:
        out.push_back({&n, multiplied});
        multiplied = false;
        return;
    case MathNode::Kind::Binary:
        read(c[0], out, multiplied);
        multiplied = n.op == '*';
        read(c[1], out, multiplied);
        return;
    case MathNode::Kind::Fraction:
    case MathNode::Kind::Power:
        read(c[0], out, multiplied);
        multiplied = true;
        read(c[1], out, multiplied);
        return;
    case MathNode::Kind::Call:
    case MathNode::Kind::Keyword:
    case MathNode::Kind::Equation:
    case MathNode::Kind::Definition:
    case MathNode::Kind::Evaluation:
    case MathNode::Kind::Symbolic:
        // A function's name isn't read, and its arguments stand apart.
        for (std::size_t i = n.kind == MathNode::Kind::Call ? 1 : 0; i < c.size(); ++i) {
            multiplied = false;
            read(c[i], out, multiplied);
        }
        multiplied = false;
        return;
    default: // Negate, Radical, Group: what's inside, as it reads
        for (const MathNode& child : c) {
            read(child, out, multiplied);
        }
        return;
    }
}

// The names in a tree that may be units (section 8): a unit name multiplied right after
// a number, or after another such name: 2.5 m, 4 kN·m, 9.81 m/s², 2 m·3 m.
void collectUnitCandidates(const MathNode& n, std::unordered_set<int>& out)
{
    std::vector<Reading> leaves;
    bool multiplied = false;
    read(n, leaves, multiplied);
    for (std::size_t i = 1; i < leaves.size(); ++i) {
        const MathNode& leaf = *leaves[i].leaf;
        const MathNode& before = *leaves[i - 1].leaf;
        if (leaf.kind == MathNode::Kind::Identifier && leaves[i].multiplied
            && (before.kind == MathNode::Kind::Number || out.contains(before.id))
            && UnitRegistry::instance().find(leaf.text)) {
            out.insert(leaf.id);
        }
    }
}

// Lowers a region's tree, replacing the names defined above it by their definitions
// (section 7) and telling units from variables (section 8).
class Lowerer {
public:
    explicit Lowerer(const LowerOptions& options) : o_(options) {}

    Expr region(const MathNode& m)
    {
        Bindings params;
        for (const std::string& name : o_.parameters) {
            params.emplace_back(name, Expr::variable(name, !listed(o_.real, name)));
        }
        Scope scope{o_.definitions.size(), &params, 0, nullptr};
        const std::unordered_set<int> units = unitPositions(m, scope);
        scope.units = &units;
        return lower(m, scope);
    }

private:
    using Bindings = std::vector<std::pair<std::string, Expr>>;

    struct Scope {
        std::size_t visible;     // how many of the definitions it sees
        const Bindings* params;  // a function's parameters, when lowering its body
        int depth;               // 0 for the region's own tree
        const std::unordered_set<int>* units;
    };

    bool listed(const std::vector<std::string>& names, const std::string& name) const
    {
        return std::find(names.begin(), names.end(), name) != names.end();
    }

    // The latest definition of `name` that `scope` sees.
    const SymbolicDefinition* definition(const std::string& name, const Scope& scope) const
    {
        for (std::size_t i = scope.visible; i-- > 0;) {
            if (o_.definitions[i].name == name) {
                return &o_.definitions[i];
            }
        }
        return nullptr;
    }

    Expr named(Expr e)
    {
        if (o_.order && e.kind() == Expr::Kind::Symbol) {
            const std::string& name = e.symbol().name;
            if (!listed(*o_.order, name)) {
                o_.order->push_back(name);
            }
        }
        return e;
    }

    Expr variable(const std::string& name)
    {
        return named(Expr::variable(name, !listed(o_.real, name)));
    }

    // True if `name` is free in `scope`: no definition, parameter or constant gives it a
    // value.
    bool isFree(const std::string& name, const Scope& scope) const
    {
        if (scope.params && std::any_of(scope.params->begin(), scope.params->end(),
                                        [&](const auto& p) { return p.first == name; })) {
            return false;
        }
        if (listed(o_.keep, name)) {
            return true;
        }
        return !definition(name, scope) && name != "π" && name != "pi" && name != "e" && name != "g";
    }

    // The names in a tree that are units (section 8): the candidates, when the tree has
    // no free names besides them. 2 m + 3 mm has units; in ½ m v², m is a mass.
    std::unordered_set<int> unitPositions(const MathNode& tree, const Scope& scope) const
    {
        std::unordered_set<int> candidates;
        collectUnitCandidates(tree, candidates);
        if (candidates.empty()) {
            return candidates;
        }
        bool free = false;
        const auto visit = [&](const auto& self, const MathNode& n) -> void {
            if (n.kind == MathNode::Kind::Identifier && !candidates.contains(n.id) && isFree(n.text, scope)) {
                free = true;
            }
            for (std::size_t i = n.kind == MathNode::Kind::Call ? 1 : 0; i < n.children.size(); ++i) {
                self(self, n.children[i]);
            }
        };
        visit(visit, tree);
        return free ? std::unordered_set<int>{} : candidates;
    }

    // A definition's body, in the definitions above it, with its parameters bound. From
    // the region's own tree, an error in it is reported on the name that used it.
    Expr body(const SymbolicDefinition& d, const Bindings* params, const MathNode& at, const Scope& scope)
    {
        const auto lowerBody = [&] {
            Scope inner{static_cast<std::size_t>(&d - o_.definitions.data()), params, scope.depth + 1, nullptr};
            const std::unordered_set<int> units = d.units ? std::unordered_set<int>(d.units->begin(), d.units->end())
                                                          : unitPositions(*d.body, inner);
            inner.units = &units;
            return lower(*d.body, inner);
        };
        if (scope.depth > 0) {
            return lowerBody();
        }
        try {
            return lowerBody();
        } catch (const Error& e) {
            throw Error("In " + d.name + ": " + e.what(), 0, 0, at.id);
        }
    }

    Expr identifier(const MathNode& m, const Scope& scope)
    {
        const std::string& name = m.text;
        if (scope.params) {
            for (const auto& [param, value] : *scope.params) {
                if (param == name) {
                    return named(value); // in order where a parameter first appears
                }
            }
        }
        if (!listed(o_.keep, name)) {
            if (const SymbolicDefinition* d = definition(name, scope)) {
                if (d->function) {
                    throw Error(name + " is a function; give it arguments, like " + name + "(x)", 0, 0, m.id);
                }
                return body(*d, nullptr, m, scope);
            }
            if (name == "π" || name == "pi") {
                return named(Expr::pi());
            }
            if (name == "e") {
                return named(Expr::e());
            }
            if (name == "g") {
                return named(Expr::constant("g", Dimension::of(1, 0, -2), 9.80665));
            }
            if (scope.units->contains(m.id)) {
                if (const Unit* unit = UnitRegistry::instance().find(name)) {
                    if (scope.depth == 0 && o_.units) {
                        o_.units->push_back(m.id);
                    }
                    return named(Expr::unit(name, unit->value.dim, unit->value.value));
                }
            }
        }
        return variable(name);
    }

    Expr call(const MathNode& m, const Scope& scope)
    {
        const auto& c = m.children;
        if (c[0].kind != MathNode::Kind::Identifier) {
            throw Error("Fill in the placeholder", 0, 0, c[0].id);
        }
        const std::string& name = c[0].text;
        // A defined value comes first, as it reads: in M (h/2), M's names before h.
        std::optional<Expr> value;
        if (!listed(o_.keep, name) && !(scope.params && std::any_of(scope.params->begin(), scope.params->end(),
                                                                    [&](const auto& p) { return p.first == name; }))) {
            if (const SymbolicDefinition* d = definition(name, scope); d && !d->function) {
                value = body(*d, nullptr, c[0], scope);
            }
        }
        std::vector<Expr> args;
        for (std::size_t i = 1; i < c.size(); ++i) {
            args.push_back(lower(c[i], scope));
        }
        if (value) {
            if (args.size() != 1) {
                throw Error(name + " is not a function", 0, 0, m.id);
            }
            return *value * args[0];
        }
        // "M (h/2)" on a value is multiplication, as numerically.
        const auto times = [&](const Expr& value) {
            if (args.size() != 1) {
                throw Error(name + " is not a function", 0, 0, m.id);
            }
            return value * args[0];
        };
        if (scope.params) {
            for (const auto& [param, value] : *scope.params) {
                if (param == name) {
                    return times(value);
                }
            }
        }
        if (!listed(o_.keep, name)) {
            if (const SymbolicDefinition* d = definition(name, scope)) {
                if (!d->function) {
                    return times(body(*d, nullptr, c[0], scope));
                }
                if (args.size() != d->params.size()) {
                    const std::size_t n = d->params.size();
                    throw Error(name + " takes " + std::to_string(n) + (n == 1 ? " argument" : " arguments"), 0, 0, m.id);
                }
                Bindings params;
                for (std::size_t i = 0; i < args.size(); ++i) {
                    params.emplace_back(d->params[i], args[i]);
                }
                return body(*d, &params, m, scope); // inlined (section 7)
            }
            const bool unary = name == "sqrt" || name == "exp" || functionNamed(name).has_value();
            if (unary && args.size() != 1) {
                throw Error(name + " takes 1 argument", 0, 0, m.id);
            }
            if (name == "sqrt") {
                return Expr::pow(args[0], Expr(Number(Rational(1, 2))));
            }
            if (name == "exp") {
                return Expr::pow(named(Expr::e()), args[0]);
            }
            if (const auto f = functionNamed(name)) {
                return Expr::apply(*f, args[0]);
            }
        }
        return Expr::call(name, std::move(args)); // min, max, and undefined functions
    }

    Expr lower(const MathNode& m, const Scope& scope)
    {
        const auto& c = m.children;
        try {
            switch (m.kind) {
            case MathNode::Kind::Number:
                return Expr(parseNumber(m));
            case MathNode::Kind::Identifier:
                return identifier(m, scope);
            case MathNode::Kind::Binary:
                switch (m.op) {
                case '+': return lower(c[0], scope) + lower(c[1], scope);
                case '-': return lower(c[0], scope) - lower(c[1], scope);
                case '*': return lower(c[0], scope) * lower(c[1], scope);
                default: throw Error("Fill in the placeholder", 0, 0, m.id);
                }
            case MathNode::Kind::Negate:
                return -lower(c[0], scope);
            case MathNode::Kind::Fraction:
                return lower(c[0], scope) / lower(c[1], scope);
            case MathNode::Kind::Power:
                return Expr::pow(lower(c[0], scope), lower(c[1], scope));
            case MathNode::Kind::Radical:
                return Expr::pow(lower(c[0], scope), Expr(Number(Rational(1, 2))));
            case MathNode::Kind::Group:
                return lower(c[0], scope);
            case MathNode::Kind::Call:
                return call(m, scope);
            case MathNode::Kind::Placeholder:
                throw Error("Fill in the placeholder", 0, 0, m.id);
            case MathNode::Kind::Equation:
                throw Error("An equation can only be a whole expression", 0, 0, m.id);
            case MathNode::Kind::Keyword:
                throw Error("A keyword goes after →", 0, 0, m.id);
            case MathNode::Kind::Definition:
            case MathNode::Kind::Evaluation:
            case MathNode::Kind::Symbolic:
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

    const LowerOptions& o_;
};

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

Expr exprFromMath(const MathNode& node, const LowerOptions& options)
{
    return Lowerer(options).region(node);
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

// Factors side by side, and whether each is a unit.
MathNode juxtapose(std::vector<MathNode> nodes, const std::vector<bool>& units)
{
    MathNode result = std::move(nodes.front());
    for (std::size_t i = 1; i < nodes.size(); ++i) {
        // "3 x", but "3·2^(1/3)": a number after another factor would read as one number.
        // Between units, a dot: kN·m.
        const MathNode& next = nodes[i];
        const bool number = next.kind == MathNode::Kind::Number
                            || (next.kind == MathNode::Kind::Power && next.children[0].kind == MathNode::Kind::Number);
        const bool dot = number || (units[i] && units[i - 1]);
        result = MathNode::binary('*', std::move(result), std::move(nodes[i]), !dot);
    }
    return result;
}

bool isUnit(const Expr& e)
{
    return e.kind() == Expr::Kind::Symbol && e.symbol().kind == Symbol::Kind::Unit;
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
        if (!options.main.empty()) {
            main_ = options.main;
        }
    }

    // A whole result: a unit alone keeps its number, 1 m.
    MathNode result(const Expr& e) const
    {
        if (isUnit(e)) {
            return product(Number(1), {{e, Expr(1)}});
        }
        return raise(e);
    }

    MathNode raise(const Expr& e) const
    {
        switch (e.kind()) {
        case Expr::Kind::Number:
            return number(e.number());
        case Expr::Kind::Symbol: {
            MathNode n = MathNode::identifier(e.symbol().name);
            if (isUnit(e) && options_.upright) {
                options_.upright->push_back(n.id);
            }
            return n;
        }
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
                // e^(k t) reads like a function, after the names: k e^(k t).
                return f.exponent.isNumber() ? std::tuple<int, std::size_t>{1, 0}
                                             : std::tuple<int, std::size_t>{4, firstAppearance(f.exponent)};
            }
            if (base.symbol().kind == Symbol::Kind::Unit) {
                return {6, appearance(base.symbol().name)}; // units last: 10 kN·m
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
        std::vector<bool> topUnits;
        std::vector<bool> bottomUnits;
        // A measurement, a number with units, shows as a decimal: 4.167×10⁶ mm⁴, not 12500000/3 mm⁴.
        const bool measured = std::any_of(factors.begin(), factors.end(), [](const ExprFactor& f) { return isUnit(f.base); });
        if (!coefficient.isExact() || coefficient.decimal() || (measured && !coefficient.isInteger())) {
            if (!coefficient.isOne() || up.empty()) {
                top.push_back(number(coefficient.isExact() ? Number(coefficient.exact(), true) : coefficient));
                topUnits.push_back(false);
            }
        } else {
            const Rational& r = coefficient.exact();
            // A quantity keeps its number: 1 m, not m.
            const bool unitsOnly = !up.empty() && std::all_of(up.begin(), up.end(), [](const ExprFactor& f) {
                return isUnit(f.base);
            });
            if (r.num() != 1 || up.empty() || unitsOnly) {
                top.push_back(MathNode::number(std::to_string(r.num())));
                topUnits.push_back(false);
            }
            if (r.den() != 1) {
                bottom.push_back(MathNode::number(std::to_string(r.den())));
                bottomUnits.push_back(false);
            }
        }
        for (const ExprFactor& f : up) {
            top.push_back(factor(f));
            topUnits.push_back(isUnit(f.base));
        }
        for (const ExprFactor& f : down) {
            bottom.push_back(factor(f));
            bottomUnits.push_back(isUnit(f.base));
        }
        if (top.size() > 1 && top.front().kind == MathNode::Kind::Number && top.front().text == "1"
            && !std::all_of(topUnits.begin() + 1, topUnits.end(), [](bool u) { return u; })) {
            top.erase(top.begin());
            topUnits.erase(topUnits.begin());
        }
        MathNode numerator = juxtapose(std::move(top), topUnits);
        if (bottom.empty()) {
            return numerator;
        }
        return MathNode(MathNode::Kind::Fraction, {std::move(numerator), juxtapose(std::move(bottom), bottomUnits)});
    }

    // A term, c · expr, shown without its sign.
    MathNode term(const Number& c, const Expr& expr) const
    {
        if (expr.kind() == Expr::Kind::Product) {
            return product(c.abs() * expr.coefficient(), expr.factors());
        }
        return product(c.abs(), {{expr, Expr(1)}});
    }

    // No variables in it: 1 m, ln(2), √2.
    static bool constantLike(const Expr& e)
    {
        std::vector<Usage> usage;
        collectUsage(e, usage);
        return usage.empty();
    }

    static bool unitsOnly(const Expr& e)
    {
        if (e.kind() == Expr::Kind::Product) {
            return std::all_of(e.factors().begin(), e.factors().end(), [](const ExprFactor& f) { return isUnit(f.base); });
        }
        return isUnit(e);
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
        if (main.empty() && !options_.main.empty()) {
            main = options_.main; // diff, x: a polynomial in x
        }
        // Descending powers only when that leads with a plus: 2 a x + b, but 2 L − x.
        if (!main.empty()) {
            const auto leading = std::max_element(terms.begin(), terms.end(), [&](const ExprTerm& a, const ExprTerm& b) {
                return degree(a.expr, main) < degree(b.expr, main);
            });
            if (leading->coefficient.isNegative() || !(degree(leading->expr, main) > Rational(0))) {
                main.clear();
            }
        }
        std::stable_sort(terms.begin(), terms.end(), [&](const ExprTerm& a, const ExprTerm& b) {
            if (!main.empty()) {
                const Rational da = degree(a.expr, main);
                const Rational db = degree(b.expr, main);
                if (!(da == db)) {
                    return da > db;
                }
            }
            // A term with no variables, like a quantity, goes last like a constant: x + 1 m.
            const bool qa = constantLike(a.expr);
            const bool qb = constantLike(b.expr);
            if (qa != qb) {
                return qb;
            }
            if (main.empty() && !main_.empty()) {
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
    return Raiser(e, options).result(e);
}

namespace {

// True if e has a root in it, as √(b² − 4 a c) or √2 does.
bool hasRoot(const Expr& e)
{
    if (e.kind() != Expr::Kind::Product) {
        return false;
    }
    return std::any_of(e.factors().begin(), e.factors().end(), [](const ExprFactor& f) {
        return f.exponent.isNumber() && !f.exponent.number().isInteger();
    });
}

// What e is over: its coefficient's denominator and its factors with negative exponents.
Expr denominatorOf(const Expr& e)
{
    if (e.kind() != Expr::Kind::Product && !e.isNumber()) {
        return Expr(1);
    }
    const Number& c = e.isNumber() ? e.number() : e.coefficient();
    std::vector<Expr> parts{Expr(c.isExact() && !c.decimal() ? c.exact().den() : 1)};
    if (e.kind() == Expr::Kind::Product) {
        for (const ExprFactor& f : e.factors()) {
            if (f.exponent.isNumber() && f.exponent.number().isNegative()) {
                parts.push_back(Expr::pow(f.base, -f.exponent));
            }
        }
    }
    return Expr::product(parts);
}

} // namespace

MathNode mathFromSolutions(const std::string& x, const std::vector<Expr>& solutions, const ExprDisplayOptions& options)
{
    ExprDisplayOptions display = options;
    display.main = x;
    const auto shown = [&](const Expr& e) { return Raiser(e, display).result(e); };
    const auto equation = [&](MathNode value) {
        return MathNode(MathNode::Kind::Equation, {MathNode::identifier(x), std::move(value)});
    };

    std::vector<MathNode> items;
    std::vector<bool> used(solutions.size(), false);
    for (std::size_t i = 0; i < solutions.size(); ++i) {
        if (used[i]) {
            continue;
        }
        used[i] = true;
        std::optional<MathNode> paired;
        for (std::size_t j = i + 1; j < solutions.size() && !paired; ++j) {
            if (used[j]) {
                continue;
            }
            // s = a ± b, for a b with a root in it.
            const Expr a = simplify((solutions[i] + solutions[j]) / Expr(2));
            Expr b = simplify((solutions[i] - solutions[j]) / Expr(2));
            if (!hasRoot(b) && !(a == Expr(0))) {
                continue;
            }
            if (b.coefficient().isNegative()) {
                b = -b;
            }
            used[j] = true;
            const Expr d = denominatorOf(b);
            const Expr top = simplify(a * d);
            if (a == Expr(0)) {
                MathNode plusMinus(MathNode::Kind::Negate, {shown(b)});
                plusMinus.op = '~';
                paired = std::move(plusMinus);
            } else if (!(d == Expr(1)) && denominatorOf(top) == Expr(1)) {
                // Over the root's denominator: (−b ± √D)/(2 a).
                MathNode both = MathNode::binary('~', shown(top), shown(simplify(b * d)));
                paired = MathNode(MathNode::Kind::Fraction, {std::move(both), shown(d)});
            } else {
                paired = MathNode::binary('~', shown(a), shown(b));
            }
        }
        items.push_back(equation(paired ? std::move(*paired) : shown(solutions[i])));
    }
    MathNode list = std::move(items.front());
    for (std::size_t i = 1; i < items.size(); ++i) {
        list = MathNode::binary(',', std::move(list), std::move(items[i]));
    }
    return list;
}

} // namespace lemma
