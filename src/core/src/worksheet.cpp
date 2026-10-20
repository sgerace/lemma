#include "lemma/worksheet.h"

#include "lemma/error.h"
#include "lemma/evaluator.h"
#include "lemma/symbolic_bridge.h"
#include "lemma/symbolic_ops.h"
#include "lemma/units.h"

#include <algorithm>
#include <memory>
#include <span>

namespace lemma {

namespace {

// The unit a result is shown in when none is given, as a tree to typeset: a named unit
// (J), SI base units (kg·m/s²), or none for a unitless result.
std::optional<MathNode> defaultUnit(const Dimension& dim)
{
    if (dim.isDimensionless()) {
        return std::nullopt;
    }
    if (const Unit* unit = UnitRegistry::instance().preferredFor(dim)) {
        return MathNode::identifier(unit->name);
    }
    // Mathcad-style ordering of SI base units, as in Dimension::toString.
    struct Entry { BaseDimension dim; const char* symbol; };
    static constexpr Entry kOrder[] = {
        {BaseDimension::Mass, "kg"},       {BaseDimension::Length, "m"},  {BaseDimension::Time, "s"},
        {BaseDimension::Current, "A"},     {BaseDimension::Temperature, "K"},
        {BaseDimension::Amount, "mol"},    {BaseDimension::Luminosity, "cd"},
    };
    const auto product = [](std::vector<MathNode> parts) {
        MathNode n = std::move(parts.front());
        for (std::size_t i = 1; i < parts.size(); ++i) {
            n = MathNode::binary('*', std::move(n), std::move(parts[i]));
        }
        return n;
    };
    std::vector<MathNode> numerator;
    std::vector<MathNode> denominator;
    for (const Entry& e : kOrder) {
        const Rational exp = dim[e.dim];
        if (exp.isZero()) {
            continue;
        }
        const Rational mag = exp < Rational(0) ? -exp : exp;
        MathNode part = MathNode::identifier(e.symbol);
        if (!(mag == Rational(1))) {
            MathNode power = mag.isInteger() ? MathNode::number(std::to_string(mag.num()))
                                             : MathNode(MathNode::Kind::Fraction,
                                                        {MathNode::number(std::to_string(mag.num())),
                                                         MathNode::number(std::to_string(mag.den()))});
            part = MathNode(MathNode::Kind::Power, {std::move(part), std::move(power)});
        }
        (exp < Rational(0) ? denominator : numerator).push_back(std::move(part));
    }
    if (denominator.empty()) {
        return product(std::move(numerator));
    }
    MathNode top = numerator.empty() ? MathNode::number("1") : product(std::move(numerator));
    return MathNode(MathNode::Kind::Fraction, {std::move(top), product(std::move(denominator))});
}

// A keyword's arguments: the children after its name. A bare name has none.
std::span<const MathNode> argumentsOf(const MathNode& k)
{
    if (k.kind != MathNode::Kind::Keyword) {
        return {};
    }
    return std::span<const MathNode>(k.children).subspan(1);
}

Error placeholderOr(const MathNode& n, const std::string& message)
{
    return Error(n.kind == MathNode::Kind::Placeholder ? "Fill in the placeholder" : message, 0, 0, n.id);
}

// An operation keyword, ready to apply.
struct Operation {
    explicit Operation(std::string n, std::string v = {}) : name(std::move(n)), variable(std::move(v)) {}

    std::string name;      // simplify, expand, factor, substitute, diff, solve, integrate
    std::string variable;  // diff, solve, integrate: with respect to
    int order = 1;         // diff: how many times
    const MathNode* from = nullptr; // integrate, x, a, b: the limits
    const MathNode* to = nullptr;
    int keyword = 0;       // the keyword's node, for errors
    std::vector<std::pair<std::string, const MathNode*>> substitutions; // substitute: name, value
};

// expression → result (docs/symbolic-spec.md). Names defined above are substituted,
// except those `keep` lists. The operation keywords apply left to right; with none,
// the expression is simplified. An equation's sides are each worked on.
RegionResult evaluateSymbolicRegion(const MathNode& math, std::span<const SymbolicDefinition> definitions,
                                    const FormatOptions& format)
{
    RegionResult result;
    result.kind = Statement::Kind::Symbolic;
    try {
        LowerOptions lowering;
        lowering.definitions = definitions;
        std::vector<Operation> operations;
        for (std::size_t i = 1; i < math.children.size(); ++i) {
            const MathNode& k = math.children[i];
            if (k.kind == MathNode::Kind::Placeholder) {
                continue;
            }
            const MathNode& name = k.kind == MathNode::Kind::Keyword ? k.children[0] : k;
            if (name.kind != MathNode::Kind::Identifier) {
                throw Error("Fill in the placeholder", 0, 0, name.id);
            }
            const std::span<const MathNode> args = argumentsOf(k);
            const std::string& keyword = name.text;
            if (keyword == "keep" || keyword == "real") {
                // keep, L, P: the names to leave symbolic (7); real, y: the names that may be negative (5.3).
                if (args.empty()) {
                    throw Error(keyword + " needs the names it applies to, like " + keyword + ", x", 0, 0, name.id);
                }
                for (const MathNode& arg : args) {
                    if (arg.kind != MathNode::Kind::Identifier) {
                        throw placeholderOr(arg, keyword + " takes names");
                    }
                    (keyword == "keep" ? lowering.keep : lowering.real).push_back(arg.text);
                }
            } else if (keyword == "simplify" || keyword == "expand" || keyword == "factor") {
                if (!args.empty()) {
                    throw Error(keyword + " takes no arguments", 0, 0, args[0].id);
                }
                operations.emplace_back(keyword);
            } else if (keyword == "substitute") {
                // substitute, x == 1, y == 2 (9.4)
                if (args.empty()) {
                    throw Error("substitute needs what to replace, like substitute, x == 1", 0, 0, name.id);
                }
                Operation op(keyword);
                for (const MathNode& arg : args) {
                    if (arg.kind != MathNode::Kind::Equation || arg.children[0].kind != MathNode::Kind::Identifier) {
                        throw placeholderOr(arg, "Write what to substitute as name == value (Ctrl+=)");
                    }
                    op.substitutions.emplace_back(arg.children[0].text, &arg.children[1]);
                }
                operations.push_back(std::move(op));
            } else if (keyword == "diff") {
                // diff, x, or diff, x, n (9.6)
                if (args.empty() || args.size() > 2) {
                    throw Error("diff needs a variable, like diff, x", 0, 0, args.empty() ? name.id : args[2].id);
                }
                if (args[0].kind != MathNode::Kind::Identifier) {
                    throw placeholderOr(args[0], "diff differentiates with respect to a name");
                }
                Operation op(keyword, args[0].text);
                if (args.size() == 2) {
                    const MathNode& n = args[1];
                    const bool digits = n.kind == MathNode::Kind::Number
                                        && n.text.find_first_not_of("0123456789") == std::string::npos;
                    if (!digits || n.text.size() > 2 || std::stoi(n.text) < 1) {
                        throw placeholderOr(n, "How many times to differentiate: a whole number, like 2");
                    }
                    op.order = std::stoi(n.text);
                }
                operations.push_back(std::move(op));
            } else if (keyword == "solve") {
                // solve, x (9.5)
                if (args.size() != 1) {
                    throw Error("solve needs the variable to solve for, like solve, x", 0, 0,
                                args.empty() ? name.id : args[1].id);
                }
                if (args[0].kind != MathNode::Kind::Identifier) {
                    throw placeholderOr(args[0], "solve solves for a name");
                }
                Operation op(keyword, args[0].text);
                op.keyword = name.id;
                operations.push_back(std::move(op));
            } else if (keyword == "integrate") {
                // integrate, x, or integrate, x, a, b (9.7)
                if (args.size() != 1 && args.size() != 3) {
                    throw Error("integrate needs a variable, and its limits if it has them: integrate, x, 0, L", 0, 0,
                                args.empty() || args.size() == 3 ? name.id : args.back().id);
                }
                if (args[0].kind != MathNode::Kind::Identifier) {
                    throw placeholderOr(args[0], "integrate integrates with respect to a name");
                }
                Operation op(keyword, args[0].text);
                if (args.size() == 3) {
                    op.from = &args[1];
                    op.to = &args[2];
                }
                operations.push_back(std::move(op));
            } else {
                throw Error("Unknown keyword: " + keyword, 0, 0, name.id);
            }
        }
        if (operations.empty()) {
            operations.emplace_back("simplify");
        }
        // What an operation is with respect to stays a name, even if it's defined above.
        for (const Operation& op : operations) {
            if (!op.variable.empty()) {
                lowering.keep.push_back(op.variable);
            }
        }

        std::vector<std::string> order;
        lowering.units = &result.units;
        lowering.order = &order;
        const MathNode& expression = math.children[0];
        const bool equation = expression.kind == MathNode::Kind::Equation;
        std::vector<Expr> sides;
        for (const MathNode* side : equation ? std::vector{&expression.children[0], &expression.children[1]}
                                             : std::vector{&expression}) {
            sides.push_back(exprFromMath(*side, lowering));
        }

        std::string solvedFor; // after solve, sides are its solutions
        for (const Operation& op : operations) {
            if (!result.note.empty()) {
                break; // an operation beyond its scope ends the work
            }
            if (op.name == "solve") {
                if (!solvedFor.empty()) {
                    throw Error("Only one solve to a region", 0, 0, op.keyword);
                }
                try {
                    // A bare expression is solved for where it's zero (6.3).
                    sides = solve(sides[0], equation ? sides[1] : Expr(0), op.variable);
                    solvedFor = op.variable;
                } catch (const BeyondScope& beyond) {
                    for (Expr& e : sides) {
                        e = simplify(e);
                    }
                    result.note = beyond.what();
                }
                continue;
            }
            std::vector<std::pair<std::string, Expr>> values;
            for (const auto& [name, value] : op.substitutions) {
                values.emplace_back(name, exprFromMath(*value, lowering));
            }
            std::optional<Expr> from;
            std::optional<Expr> to;
            if (op.from) {
                from = exprFromMath(*op.from, lowering);
                to = exprFromMath(*op.to, lowering);
            }
            for (Expr& e : sides) {
                try {
                    if (op.name == "simplify") {
                        e = simplify(e);
                    } else if (op.name == "expand") {
                        e = expand(e);
                    } else if (op.name == "factor") {
                        e = factor(e);
                        if (hasUnfactored(e)) {
                            result.note = "Couldn't factor this further";
                        }
                    } else if (op.name == "diff") {
                        e = differentiate(e, op.variable, op.order);
                    } else if (op.name == "integrate") {
                        e = from ? integrate(e, op.variable, *from, *to) : integrate(e, op.variable);
                    } else if (op.name == "substitute") {
                        // All at once, so substitute, x == y, y == x swaps them.
                        e = simplify(replaceSymbols(e, [&](const Symbol& s) -> std::optional<Expr> {
                            for (const auto& [name, value] : values) {
                                if (s.kind == Symbol::Kind::Variable && s.name == name) {
                                    return value;
                                }
                            }
                            return std::nullopt;
                        }));
                    }
                } catch (const BeyondScope& beyond) {
                    e = simplify(e);
                    result.note = beyond.what();
                }
            }
        }

        ExprDisplayOptions display;
        display.order = order;
        for (const Operation& op : operations) {
            if (!op.variable.empty()) {
                display.main = op.variable;
            }
        }
        display.format = format;
        display.upright = &result.units;
        if (!solvedFor.empty()) {
            std::vector<Expr> solutions;
            for (const Expr& e : sides) {
                solutions.push_back(combineUnits(e, order));
            }
            result.symbolic = mathFromSolutions(solvedFor, solutions, display);
        } else {
            std::vector<MathNode> shown;
            for (const Expr& e : sides) {
                shown.push_back(mathFromExpr(combineUnits(e, order), display));
            }
            result.symbolic = equation ? MathNode(MathNode::Kind::Equation, {std::move(shown[0]), std::move(shown[1])})
                                       : std::move(shown[0]);
        }
        result.value = toLinear(*result.symbolic);
        result.status = RegionResult::Status::Ok;
    } catch (const Error& e) {
        result.status = RegionResult::Status::Error;
        result.error = e.what();
        result.errorNode = e.node();
        result.note.clear();
    }
    return result;
}

// What a definition region gives symbolic evaluations below it, if it defines a name.
std::optional<SymbolicDefinition> symbolicDefinition(const MathNode& math)
{
    if (math.kind != MathNode::Kind::Definition) {
        return std::nullopt;
    }
    const MathNode& lhs = math.children[0];
    SymbolicDefinition d;
    d.body = &math.children[1];
    if (lhs.kind == MathNode::Kind::Identifier) {
        d.name = lhs.text;
        return d;
    }
    if (lhs.kind != MathNode::Kind::Call || lhs.children[0].kind != MathNode::Kind::Identifier) {
        return std::nullopt;
    }
    d.name = lhs.children[0].text;
    d.function = true;
    for (std::size_t i = 1; i < lhs.children.size(); ++i) {
        if (lhs.children[i].kind != MathNode::Kind::Identifier) {
            return std::nullopt;
        }
        d.params.push_back(lhs.children[i].text);
    }
    return d;
}

RegionResult evaluateMathRegion(const MathNode& math, Environment& env, const FormatOptions& format)
{
    RegionResult result;
    try {
        const Statement s = lowerMath(math);
        result.kind = s.kind;
        switch (s.kind) {
        case Statement::Kind::Empty:
        case Statement::Kind::Symbolic: // evaluated above
            return result;
        case Statement::Kind::Expression:
            evaluate(*s.expr, env, Lookup::VariablesFirst, &result.units);
            break;
        case Statement::Kind::Definition:
            env.define(s.name, evaluate(*s.expr, env, Lookup::VariablesFirst, &result.units));
            break;
        case Statement::Kind::FunctionDefinition:
            env.defineFunction(s.name, {s.params, s.expr, std::make_shared<Environment>(env)});
            break;
        case Statement::Kind::Evaluation: {
            const Quantity q = evaluate(*s.expr, env, Lookup::VariablesFirst, &result.units);
            if (s.unit) {
                const Quantity unit = evaluate(*s.unit, env, Lookup::UnitsFirst, &result.units);
                if (!(unit.dim == q.dim)) {
                    throw Error("Units don't match: the result is " + describeDimension(q.dim) + ", but " +
                                    s.unitText + " is " + describeDimension(unit.dim),
                                0, 0, s.unit->origin);
                }
                if (unit.value == 0.0) {
                    throw Error("Can't display a result in a zero unit", 0, 0, s.unit->origin);
                }
                result.number = formatNumber(q.value / unit.value, format);
                result.value = result.number + " " + s.unitText;
            } else {
                result.unit = defaultUnit(q.dim);
                const Unit* preferred = UnitRegistry::instance().preferredFor(q.dim);
                result.number = formatNumber(preferred ? q.value / preferred->value.value : q.value, format);
                result.value = formatQuantity(q, format);
            }
            break;
        }
        }
        result.status = RegionResult::Status::Ok;
    } catch (const Error& e) {
        result.status = RegionResult::Status::Error;
        result.error = e.what();
        result.errorNode = e.node();
    }
    return result;
}

RegionResult evaluatePlotRegion(const Region& r, const Environment& env)
{
    RegionResult result;
    try {
        result.plot = evaluatePlot(r.source, r.axes, env);
        result.status = RegionResult::Status::Ok;
    } catch (const Error& e) {
        result.status = RegionResult::Status::Error;
        result.error = e.what();
        result.errorBegin = e.begin();
        result.errorEnd = e.end();
    }
    return result;
}

} // namespace

Region& Worksheet::addRegionOfKind(Region::Kind kind, double x, double y)
{
    Region r;
    r.id = nextId_++;
    r.kind = kind;
    r.x = x;
    r.y = y;
    regions_.push_back(std::move(r));
    return regions_.back();
}

int Worksheet::addRegion(double x, double y, MathNode math)
{
    Region& r = addRegionOfKind(Region::Kind::Math, x, y);
    r.math = std::move(math);
    return r.id;
}

int Worksheet::addRegion(double x, double y, std::string_view linear)
{
    return addRegion(x, y, mathFromLinear(linear));
}

int Worksheet::addTextRegion(double x, double y, double width, std::string text)
{
    Region& r = addRegionOfKind(Region::Kind::Text, x, y);
    r.width = width;
    r.source = std::move(text);
    return r.id;
}

int Worksheet::addPlotRegion(double x, double y, double width, double height, std::string traces)
{
    Region& r = addRegionOfKind(Region::Kind::Plot, x, y);
    r.width = width;
    r.height = height;
    r.source = std::move(traces);
    return r.id;
}

void Worksheet::removeRegion(int id)
{
    std::erase_if(regions_, [id](const Region& r) { return r.id == id; });
}

void Worksheet::restoreRegion(Region region)
{
    // Regions are kept in id order, which is also their stacking and tie-break order.
    const auto it = std::lower_bound(regions_.begin(), regions_.end(), region.id,
                                     [](const Region& r, int id) { return r.id < id; });
    nextId_ = std::max(nextId_, region.id + 1);
    regions_.insert(it, std::move(region));
}

void Worksheet::clear()
{
    regions_.clear();
}

Region* Worksheet::region(int id)
{
    const auto it = std::find_if(regions_.begin(), regions_.end(), [id](const Region& r) { return r.id == id; });
    return it == regions_.end() ? nullptr : &*it;
}

const Region* Worksheet::region(int id) const
{
    return const_cast<Worksheet*>(this)->region(id);
}

void Worksheet::evaluate()
{
    std::vector<Region*> order;
    order.reserve(regions_.size());
    for (Region& r : regions_) {
        if (r.kind != Region::Kind::Text) {
            order.push_back(&r);
        } else {
            r.result = {};
        }
    }
    std::stable_sort(order.begin(), order.end(), [](const Region* a, const Region* b) {
        return a->y != b->y ? a->y < b->y : a->x < b->x;
    });

    Environment env;
    std::vector<SymbolicDefinition> definitions; // for symbolic evaluations, as written
    for (Region* r : order) {
        if (r->kind == Region::Kind::Plot) {
            r->result = evaluatePlotRegion(*r, env);
        } else if (r->math.kind == MathNode::Kind::Symbolic) {
            r->result = evaluateSymbolicRegion(r->math, definitions, format);
        } else {
            r->result = evaluateMathRegion(r->math, env, format);
            if (auto d = symbolicDefinition(r->math)) {
                definitions.push_back(std::move(*d));
            }
        }
    }
}

} // namespace lemma
