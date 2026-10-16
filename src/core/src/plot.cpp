#include "lemma/plot.h"

#include "lemma/error.h"
#include "lemma/evaluator.h"
#include "lemma/format.h"
#include "lemma/parser.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace lemma {

namespace {

bool isBlank(std::string_view s)
{
    return s.find_first_not_of(" \t") == std::string_view::npos;
}

// The traces' byte ranges, split at commas outside parentheses ("f, max(x, 1)" is two).
std::vector<std::pair<std::size_t, std::size_t>> splitTraces(std::string_view s)
{
    std::vector<std::pair<std::size_t, std::size_t>> pieces;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || (s[i] == ',' && depth == 0)) {
            pieces.emplace_back(start, i);
            start = i + 1;
        } else if (s[i] == '(') {
            ++depth;
        } else if (s[i] == ')') {
            depth = std::max(0, depth - 1);
        }
    }
    return pieces;
}

NodePtr parseExpression(std::string_view text)
{
    const Statement s = parseStatement(text);
    if (s.kind != Statement::Kind::Expression) {
        throw Error("Expected an expression here, not a definition or evaluation", 0, text.size());
    }
    return s.expr;
}

// Evaluates an axis field, naming the field in any error, since it has no squiggle.
Quantity axisValue(std::string_view text, const char* field, const Environment& env)
{
    try {
        return evaluate(*parseExpression(text), env);
    } catch (const Error& e) {
        throw Error(std::string(field) + ": " + e.what());
    }
}

void requireDimension(const Quantity& q, const Dimension& dim, const char* field)
{
    if (!(q.dim == dim)) {
        throw Error(std::string(field) + ": units don't match: the axis is " + describeDimension(dim) + ", but this is " +
                    describeDimension(q.dim));
    }
}

// The trace to evaluate at each x: "f" means f(x).
NodePtr traceExpression(std::string_view text, const std::string& xVar, const Environment& env)
{
    NodePtr expr = parseExpression(text);
    if (expr->kind != Node::Kind::Identifier || !(env.function(expr->name) || isBuiltinFunction(expr->name))) {
        return expr;
    }
    auto arg = std::make_shared<Node>(*expr);
    arg->name = xVar;
    auto call = std::make_shared<Node>(*expr);
    call->kind = Node::Kind::Call;
    call->args = {arg};
    return call;
}

Error offset(const Error& e, std::size_t by)
{
    return Error(e.what(), e.begin() + by, e.end() + by);
}

// Widens [lo, hi] to round numbers, so fitted limits read well.
std::pair<double, double> fit(double lo, double hi)
{
    if (lo == hi) {
        const double pad = lo == 0.0 ? 1.0 : std::abs(lo) * 0.1;
        lo -= pad;
        hi += pad;
    }
    const double step = niceStep(hi - lo);
    return {std::floor(lo / step) * step, std::ceil(hi / step) * step};
}

} // namespace

double niceStep(double span, int count)
{
    if (!(span > 0.0) || !std::isfinite(span) || count < 1) {
        return 1.0;
    }
    const double raw = span / count;
    const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
    const double n = raw / magnitude;
    return (n < 1.5 ? 1.0 : n < 3.0 ? 2.0 : n < 7.0 ? 5.0 : 10.0) * magnitude;
}

PlotData evaluatePlot(std::string_view traces, const PlotAxes& axes, const Environment& env)
{
    PlotData data;

    std::string xVar;
    try {
        const NodePtr x = parseExpression(axes.x);
        if (x->kind != Node::Kind::Identifier) {
            throw Error("");
        }
        xVar = x->name;
    } catch (const Error&) {
        throw Error("x-axis variable: type a name, like x");
    }

    const Quantity xMin = axisValue(axes.xMin, "x min", env);
    const Quantity xMax = axisValue(axes.xMax, "x max", env);
    requireDimension(xMax, xMin.dim, "x max");
    if (!(xMin.value < xMax.value)) {
        throw Error("x max must be greater than x min");
    }
    data.xMin = xMin.value;
    data.xMax = xMax.value;
    data.xDim = xMin.dim;

    std::optional<Dimension> yDim;
    double lo = std::numeric_limits<double>::infinity();
    double hi = -lo;
    Environment local(&env);
    for (const auto& [b, e] : splitTraces(traces)) {
        const std::string_view text = traces.substr(b, e - b);
        if (isBlank(text)) {
            continue; // e.g. a trailing comma while typing the next trace
        }
        NodePtr expr;
        try {
            expr = traceExpression(text, xVar, env);
        } catch (const Error& err) {
            throw offset(err, b);
        }

        PlotTrace trace;
        trace.begin = b + text.find_first_not_of(" \t");
        trace.end = b + text.find_last_not_of(" \t") + 1;
        trace.segments.emplace_back();
        std::optional<Error> firstError;
        for (int i = 0; i < kPlotSamples; ++i) {
            const double x = data.xMin + (data.xMax - data.xMin) * i / (kPlotSamples - 1);
            local.define(xVar, {x, data.xDim});
            std::optional<Quantity> y;
            try {
                y = evaluate(*expr, local);
            } catch (const Error& err) {
                if (!firstError) {
                    firstError = offset(err, b);
                }
            }
            if (!y || !std::isfinite(y->value)) {
                if (!trace.segments.back().empty()) {
                    trace.segments.emplace_back(); // a gap
                }
                continue;
            }
            if (!yDim) {
                yDim = y->dim;
            } else if (!(y->dim == *yDim)) {
                throw Error("Units don't match: this trace is " + describeDimension(y->dim) +
                                ", but the one before it is " + describeDimension(*yDim),
                            trace.begin, trace.end);
            }
            trace.segments.back().push_back({x, y->value});
            lo = std::min(lo, y->value);
            hi = std::max(hi, y->value);
        }
        if (trace.segments.back().empty()) {
            trace.segments.pop_back();
        }
        if (trace.segments.empty()) {
            // Nowhere to draw it: the error is more useful than an empty plot.
            throw firstError ? *firstError : Error("This trace has no values to plot", trace.begin, trace.end);
        }
        data.traces.push_back(std::move(trace));
    }

    std::optional<Quantity> yMin;
    std::optional<Quantity> yMax;
    if (!isBlank(axes.yMin)) {
        yMin = axisValue(axes.yMin, "y min", env);
    }
    if (!isBlank(axes.yMax)) {
        yMax = axisValue(axes.yMax, "y max", env);
    }
    if (!yDim) {
        yDim = yMin ? yMin->dim : yMax ? yMax->dim : Dimension{};
    }
    if (yMin) {
        requireDimension(*yMin, *yDim, "y min");
    }
    if (yMax) {
        requireDimension(*yMax, *yDim, "y max");
    }
    data.yDim = *yDim;

    if (data.traces.empty()) {
        lo = -10.0; // nothing to fit yet
        hi = 10.0;
    }
    const auto [fitLo, fitHi] = fit(yMin ? std::min(lo, yMin->value) : lo, yMax ? std::max(hi, yMax->value) : hi);
    data.yMin = yMin ? yMin->value : fitLo;
    data.yMax = yMax ? yMax->value : fitHi;
    if (!(data.yMin < data.yMax)) {
        throw Error("y max must be greater than y min");
    }
    return data;
}

} // namespace lemma
