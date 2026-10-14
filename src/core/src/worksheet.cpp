#include "lemma/worksheet.h"

#include "lemma/error.h"
#include "lemma/evaluator.h"
#include "lemma/parser.h"

#include <algorithm>
#include <memory>

namespace lemma {

namespace {

std::string trim(std::string_view s)
{
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t");
    return std::string(s.substr(first, last - first + 1));
}

RegionResult evaluateRegion(const std::string& source, Environment& env, const FormatOptions& format)
{
    RegionResult result;
    result.display = trim(source);
    try {
        const Statement s = parseStatement(source);
        result.kind = s.kind;
        switch (s.kind) {
        case Statement::Kind::Empty:
            return result;
        case Statement::Kind::Expression:
            evaluate(*s.expr, env);
            break;
        case Statement::Kind::Definition:
            env.define(s.name, evaluate(*s.expr, env));
            break;
        case Statement::Kind::FunctionDefinition:
            env.defineFunction(s.name, {s.params, s.expr, std::make_shared<Environment>(env)});
            break;
        case Statement::Kind::Evaluation: {
            const Quantity q = evaluate(*s.expr, env);
            if (s.unit) {
                const Quantity unit = evaluate(*s.unit, env, Lookup::UnitsFirst);
                if (!(unit.dim == q.dim)) {
                    const auto name = [](const Dimension& d) { return d.isDimensionless() ? "unitless" : d.toString(); };
                    throw Error("Units don't match: the result is " + name(q.dim) + ", but " + s.unitText + " is " +
                                    name(unit.dim),
                                s.unit->begin, s.unit->end);
                }
                if (unit.value == 0.0) {
                    throw Error("Can't display a result in a zero unit", s.unit->begin, s.unit->end);
                }
                result.value = formatNumber(q.value / unit.value, format) + " " + s.unitText;
            } else {
                result.value = formatQuantity(q, format);
            }
            result.display = trim(std::string_view(source).substr(0, s.equalsPos)) + " = " + result.value;
            break;
        }
        }
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

int Worksheet::addRegion(double x, double y, std::string source)
{
    Region r;
    r.id = nextId_++;
    r.x = x;
    r.y = y;
    r.source = std::move(source);
    regions_.push_back(std::move(r));
    return regions_.back().id;
}

void Worksheet::removeRegion(int id)
{
    std::erase_if(regions_, [id](const Region& r) { return r.id == id; });
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

void Worksheet::evaluate()
{
    std::vector<Region*> order;
    order.reserve(regions_.size());
    for (Region& r : regions_) {
        order.push_back(&r);
    }
    std::stable_sort(order.begin(), order.end(), [](const Region* a, const Region* b) {
        return a->y != b->y ? a->y < b->y : a->x < b->x;
    });

    Environment env;
    for (Region* r : order) {
        r->result = evaluateRegion(r->source, env, format);
    }
}

} // namespace lemma
