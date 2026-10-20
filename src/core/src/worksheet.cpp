#include "lemma/worksheet.h"

#include "lemma/error.h"
#include "lemma/evaluator.h"
#include "lemma/symbolic_bridge.h"
#include "lemma/symbolic_ops.h"
#include "lemma/units.h"

#include <algorithm>
#include <memory>

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

// The keywords of docs/symbolic-spec.md, section 9, that come in later steps.
bool isPlannedKeyword(const std::string& name)
{
    for (const char* k : {"expand", "factor", "substitute", "solve", "diff", "integrate", "keep", "real"}) {
        if (name == k) {
            return true;
        }
    }
    return false;
}

// expression → result (docs/symbolic-spec.md). With no keyword, or `simplify`, the
// result is the expression simplified; an equation's sides are simplified each.
RegionResult evaluateSymbolicRegion(const MathNode& math, const FormatOptions& format)
{
    RegionResult result;
    result.kind = Statement::Kind::Symbolic;
    try {
        for (std::size_t i = 1; i < math.children.size(); ++i) {
            const MathNode& k = math.children[i];
            if (k.kind == MathNode::Kind::Placeholder) {
                continue;
            }
            const MathNode& name = k.kind == MathNode::Kind::Keyword ? k.children[0] : k;
            if (name.kind != MathNode::Kind::Identifier) {
                throw Error("Fill in the placeholder", 0, 0, name.id);
            }
            if (name.text == "simplify") {
                if (k.kind == MathNode::Kind::Keyword) {
                    throw Error("simplify takes no arguments", 0, 0, k.children[1].id);
                }
            } else if (isPlannedKeyword(name.text)) {
                throw Error(name.text + " isn't available yet", 0, 0, name.id);
            } else {
                throw Error("Unknown keyword: " + name.text, 0, 0, name.id);
            }
        }

        const MathNode& expression = math.children[0];
        ExprDisplayOptions options;
        options.order = namesInOrder(expression);
        options.format = format;
        const auto shown = [&](const MathNode& side) { return mathFromExpr(simplify(exprFromMath(side)), options); };
        if (expression.kind == MathNode::Kind::Equation) {
            result.symbolic = MathNode(MathNode::Kind::Equation,
                                       {shown(expression.children[0]), shown(expression.children[1])});
        } else {
            result.symbolic = shown(expression);
        }
        result.value = toLinear(*result.symbolic);
        result.status = RegionResult::Status::Ok;
    } catch (const Error& e) {
        result.status = RegionResult::Status::Error;
        result.error = e.what();
        result.errorNode = e.node();
    }
    return result;
}

RegionResult evaluateMathRegion(const MathNode& math, Environment& env, const FormatOptions& format)
{
    if (math.kind == MathNode::Kind::Symbolic) {
        return evaluateSymbolicRegion(math, format);
    }
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
    for (Region* r : order) {
        r->result = r->kind == Region::Kind::Plot ? evaluatePlotRegion(*r, env) : evaluateMathRegion(r->math, env, format);
    }
}

} // namespace lemma
