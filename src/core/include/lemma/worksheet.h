#pragma once

#include "lemma/ast.h"
#include "lemma/format.h"
#include "lemma/math.h"
#include "lemma/plot.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lemma {

struct RegionResult {
    enum class Status { Empty, Ok, Error };

    Status status = Status::Empty;
    Statement::Kind kind = Statement::Kind::Empty;
    std::string value;  // Evaluation: the formatted result, e.g. "20 J"
    std::string number; // Evaluation: just its number, e.g. "20", to typeset before the unit
    // Evaluation with no unit given: the unit the result is shown in, if any, e.g. J.
    std::optional<MathNode> unit;
    std::vector<int> units; // math regions: the identifiers that meant units, shown upright
    // Symbolic evaluation: the result, typeset after "→"; `value` is its linear text.
    std::optional<MathNode> symbolic;
    std::string error;
    int errorNode = 0;          // math regions: the node the error refers to
    std::size_t errorBegin = 0; // plot regions: the byte range of the field the error refers to
    std::size_t errorEnd = 0;
    PlotData plot; // plot regions: the sampled traces and axis ranges
};

struct Region {
    // Text regions hold notes and headings. They are laid out, but never evaluated.
    // Plot regions draw their source's comma-separated traces against `axes`.
    enum class Kind { Math, Text, Plot };

    int id = 0;
    Kind kind = Kind::Math;
    double x = 0.0; // left end of the (first) baseline, in 1/96 inch from the top-left of the first page
    double y = 0.0;
    double width = 0.0;  // text regions wrap at this width; plots are this wide; unused for math
    double height = 0.0; // plots: the height of the plot area
    MathNode math;       // math regions; an empty one is a placeholder
    std::string source;  // text and plots, UTF-8; for text, lines are separated by '\n'; for plots, the traces
    PlotAxes axes;       // plots only
    RegionResult result;
};

// A page of math, text and plot regions. Like Mathcad, it evaluates top to bottom, then left to right,
// so a name can only be used below (or to the right of) where it is defined.
class Worksheet {
public:
    int addRegion(double x, double y, MathNode math = {});
    // A math region from linear syntax, as if pasted. Throws lemma::Error if it doesn't parse.
    int addRegion(double x, double y, std::string_view linear);
    int addTextRegion(double x, double y, double width, std::string text = {});
    int addPlotRegion(double x, double y, double width, double height, std::string traces = {});
    void removeRegion(int id);
    // Puts back a removed region with its id and its place in the order (for undo).
    void restoreRegion(Region region);
    void clear();

    Region* region(int id);
    const Region* region(int id) const;
    const std::vector<Region>& regions() const { return regions_; }

    // Recomputes every region's result.
    void evaluate();

    FormatOptions format;

private:
    Region& addRegionOfKind(Region::Kind kind, double x, double y);

    std::vector<Region> regions_;
    int nextId_ = 1;
};

} // namespace lemma
