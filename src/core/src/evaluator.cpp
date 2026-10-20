#include "lemma/evaluator.h"

#include "lemma/error.h"
#include "lemma/format.h"
#include "lemma/units.h"

#include <cmath>
#include <functional>
#include <numbers>
#include <optional>

namespace lemma {

void Environment::define(const std::string& name, Quantity value)
{
    functions_.erase(name);
    variables_[name] = value;
}

void Environment::defineFunction(const std::string& name, Function function)
{
    variables_.erase(name);
    functions_[name] = std::move(function);
}

const Quantity* Environment::variable(std::string_view name) const
{
    const auto it = variables_.find(std::string(name));
    if (it != variables_.end()) {
        return &it->second;
    }
    if (functions_.contains(std::string(name))) {
        return nullptr; // shadowed by a function in this scope
    }
    return parent_ ? parent_->variable(name) : nullptr;
}

const Function* Environment::function(std::string_view name) const
{
    const auto it = functions_.find(std::string(name));
    if (it != functions_.end()) {
        return &it->second;
    }
    if (variables_.contains(std::string(name))) {
        return nullptr;
    }
    return parent_ ? parent_->function(name) : nullptr;
}

namespace {

std::optional<Quantity> constant(std::string_view name)
{
    if (name == "π" || name == "pi") return Quantity::scalar(std::numbers::pi);
    if (name == "e") return Quantity::scalar(std::numbers::e);
    if (name == "g") return Quantity{9.80665, Dimension::of(1, 0, -2)};
    return std::nullopt;
}

Quantity checked(Quantity q, const Node& node)
{
    if (!std::isfinite(q.value)) {
        throw Error("The result is not a finite number", node.begin, node.end, node.origin);
    }
    return q;
}

Quantity power(const Quantity& base, const Quantity& exponent, const Node& node)
{
    if (!exponent.dim.isDimensionless()) {
        throw Error("An exponent can't have units (it has " + describeDimension(exponent.dim) + ")", node.begin,
                    node.end, node.origin);
    }
    if (base.dim.isDimensionless()) {
        return checked({std::pow(base.value, exponent.value), {}}, node);
    }
    const auto rational = Rational::fromDouble(exponent.value);
    if (!rational) {
        throw Error("A quantity with units can only be raised to a simple fraction", node.begin, node.end, node.origin);
    }
    return checked({std::pow(base.value, exponent.value), base.dim.pow(*rational)}, node);
}

Quantity requireUnitless(const Quantity& q, const std::string& function, const Node& node)
{
    if (!q.dim.isDimensionless()) {
        throw Error(function + " needs a unitless argument (it has " + describeDimension(q.dim) + ")", node.begin,
                    node.end, node.origin);
    }
    return q;
}

using Builtin = std::function<Quantity(const std::vector<Quantity>&, const Node&)>;

Builtin unitlessFunction(std::string name, double (*fn)(double))
{
    return [name = std::move(name), fn](const std::vector<Quantity>& args, const Node& node) {
        return checked(Quantity::scalar(fn(requireUnitless(args[0], name, *node.args[0]).value)), node);
    };
}

Quantity extremum(const std::vector<Quantity>& args, const Node& node, bool wantMax)
{
    Quantity best = args[0];
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (!(args[i].dim == best.dim)) {
            throw Error("Units don't match: " + describeDimension(best.dim) + " and " + describeDimension(args[i].dim),
                        node.args[i]->begin, node.args[i]->end, node.args[i]->origin);
        }
        if (wantMax ? args[i].value > best.value : args[i].value < best.value) {
            best = args[i];
        }
    }
    return best;
}

struct BuiltinEntry {
    int arity; // -1 for one or more
    Builtin fn;
};

const std::unordered_map<std::string, BuiltinEntry>& builtins()
{
    static const std::unordered_map<std::string, BuiltinEntry> table = {
        {"sqrt", {1, [](const auto& a, const Node& n) { return power(a[0], Quantity::scalar(0.5), n); }}},
        {"abs", {1, [](const auto& a, const Node&) { return Quantity{std::abs(a[0].value), a[0].dim}; }}},
        {"sin", {1, unitlessFunction("sin", [](double x) { return std::sin(x); })}},
        {"cos", {1, unitlessFunction("cos", [](double x) { return std::cos(x); })}},
        {"tan", {1, unitlessFunction("tan", [](double x) { return std::tan(x); })}},
        {"asin", {1, unitlessFunction("asin", [](double x) { return std::asin(x); })}},
        {"acos", {1, unitlessFunction("acos", [](double x) { return std::acos(x); })}},
        {"atan", {1, unitlessFunction("atan", [](double x) { return std::atan(x); })}},
        {"ln", {1, unitlessFunction("ln", [](double x) { return std::log(x); })}},
        {"log", {1, unitlessFunction("log", [](double x) { return std::log10(x); })}},
        {"exp", {1, unitlessFunction("exp", [](double x) { return std::exp(x); })}},
        {"min", {-1, [](const auto& a, const Node& n) { return extremum(a, n, false); }}},
        {"max", {-1, [](const auto& a, const Node& n) { return extremum(a, n, true); }}},
    };
    return table;
}

class Evaluator {
public:
    Evaluator(const Environment& env, Lookup lookup, std::vector<int>* units = nullptr)
        : env_(env), lookup_(lookup), units_(units)
    {
    }

    Quantity eval(const Node& node) const
    {
        switch (node.kind) {
        case Node::Kind::Number:
            return Quantity::scalar(node.number);
        case Node::Kind::Identifier:
            return identifier(node);
        case Node::Kind::Unary: {
            Quantity q = eval(*node.args[0]);
            q.value = -q.value;
            return q;
        }
        case Node::Kind::Binary:
            return binary(node);
        case Node::Kind::Call:
            return call(node);
        }
        throw Error("Unknown expression", node.begin, node.end, node.origin);
    }

private:
    Quantity identifier(const Node& node) const
    {
        const Unit* unit = UnitRegistry::instance().find(node.name);
        if (lookup_ == Lookup::UnitsFirst && unit) {
            return unitValue(*unit, node);
        }
        if (const Quantity* q = env_.variable(node.name)) {
            return *q;
        }
        if (const auto c = constant(node.name)) {
            return *c;
        }
        if (unit) {
            return unitValue(*unit, node);
        }
        if (env_.function(node.name) || builtins().contains(node.name)) {
            throw Error(node.name + " is a function; give it arguments, like " + node.name + "(x)",
                        node.begin, node.end, node.origin);
        }
        throw Error(node.name + " is not defined", node.begin, node.end, node.origin);
    }

    Quantity binary(const Node& node) const
    {
        if (node.op == '=') {
            throw Error("An equation (Ctrl+=) only has a meaning in a symbolic evaluation (→)", node.begin, node.end,
                        node.origin);
        }
        const Quantity a = eval(*node.args[0]);
        const Quantity b = eval(*node.args[1]);
        switch (node.op) {
        case '+':
        case '-':
            if (!(a.dim == b.dim)) {
                throw Error("Units don't match: " + describeDimension(a.dim) + " and " + describeDimension(b.dim),
                            node.begin, node.end, node.origin);
            }
            return {node.op == '+' ? a.value + b.value : a.value - b.value, a.dim};
        case '*':
            return checked({a.value * b.value, a.dim * b.dim}, node);
        case '/':
            if (b.value == 0.0) {
                throw Error("Division by zero", node.args[1]->begin, node.args[1]->end, node.args[1]->origin);
            }
            return checked({a.value / b.value, a.dim / b.dim}, node);
        case '^':
            return power(a, b, node);
        default:
            throw Error("Unknown operator", node.begin, node.end, node.origin);
        }
    }

    Quantity call(const Node& node) const
    {
        std::vector<Quantity> args;
        args.reserve(node.args.size());
        for (const NodePtr& arg : node.args) {
            args.push_back(eval(*arg));
        }

        if (const Function* fn = env_.function(node.name)) {
            if (args.size() != fn->params.size()) {
                throw arityError(node, static_cast<int>(fn->params.size()));
            }
            Environment local(fn->closure.get());
            for (std::size_t i = 0; i < args.size(); ++i) {
                local.define(fn->params[i], args[i]);
            }
            try {
                return Evaluator(local, Lookup::VariablesFirst).eval(*fn->body);
            } catch (const Error& e) {
                // The body's source positions belong to another region.
                throw Error("In " + node.name + ": " + e.what(), node.begin, node.end, node.origin);
            }
        }

        // "M (h/2)" on a value means multiplication, not a call.
        const auto multiply = [&](const Quantity& value) -> Quantity {
            if (args.size() != 1) {
                throw Error(node.name + " is not a function", node.begin, node.end, node.origin);
            }
            return checked({value.value * args[0].value, value.dim * args[0].dim}, node);
        };
        if (const Quantity* q = env_.variable(node.name)) {
            return multiply(*q);
        }

        const auto it = builtins().find(node.name);
        if (it == builtins().end()) {
            return multiply(identifier(node));
        }
        const BuiltinEntry& builtin = it->second;
        if (builtin.arity >= 0 ? static_cast<int>(args.size()) != builtin.arity : args.empty()) {
            throw arityError(node, builtin.arity);
        }
        return builtin.fn(args, node);
    }

    Quantity unitValue(const Unit& unit, const Node& node) const
    {
        if (units_ && node.origin) {
            units_->push_back(node.origin);
        }
        return unit.value;
    }

    static Error arityError(const Node& node, int expected)
    {
        const std::string count = expected < 0 ? "at least one argument"
                                  : expected == 1 ? "1 argument"
                                                  : std::to_string(expected) + " arguments";
        return Error(node.name + " takes " + count, node.begin, node.end, node.origin);
    }

    const Environment& env_;
    Lookup lookup_;
    std::vector<int>* units_;
};

} // namespace

Quantity evaluate(const Node& node, const Environment& env, Lookup lookup, std::vector<int>* units)
{
    return Evaluator(env, lookup, units).eval(node);
}

bool isBuiltinFunction(std::string_view name)
{
    return builtins().contains(std::string(name));
}

} // namespace lemma
