#pragma once

#include "lemma/dimension.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace lemma {

class Environment;

// A plot region's placeholders, apart from its traces (the region's source, e.g. "f, g(x)/2").
struct PlotAxes {
    std::string x = "x";      // the variable swept along the x axis
    std::string xMin = "-10"; // limits are expressions and may carry units, e.g. "3 m"
    std::string xMax = "10";
    std::string yMin; // empty: fit the data
    std::string yMax;

    friend bool operator==(const PlotAxes&, const PlotAxes&) = default;
};

struct PlotPoint {
    double x = 0.0;
    double y = 0.0;
};

struct PlotTrace {
    std::size_t begin = 0; // byte range of this trace in the region's source
    std::size_t end = 0;
    // The curve, broken wherever it can't be evaluated (e.g. 1/x at 0).
    std::vector<std::vector<PlotPoint>> segments;
};

// What a plot shows. Values are in SI base units.
struct PlotData {
    std::vector<PlotTrace> traces;
    double xMin = 0.0;
    double xMax = 1.0;
    double yMin = 0.0;
    double yMax = 1.0;
    Dimension xDim;
    Dimension yDim;
};

constexpr int kPlotSamples = 401;

// Samples each comma-separated trace across the x range, with the x variable bound in a
// scope of its own. A bare function name, like "f" or "sin", is called with the x
// variable. Throws lemma::Error if the plot can't be drawn at all. Errors in a trace
// carry its byte range in `traces`; errors in an axis field name the field instead.
PlotData evaluatePlot(std::string_view traces, const PlotAxes& axes, const Environment& env);

// A round step (1, 2 or 5 × 10ⁿ) that splits `span` into about `count` parts, for ticks
// and fitted limits.
double niceStep(double span, int count = 5);

} // namespace lemma
