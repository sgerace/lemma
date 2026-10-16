#pragma once

#include "lemma/ast.h"
#include "lemma/format.h"

#include <cstddef>
#include <string>
#include <vector>

namespace lemma {

struct RegionResult {
    enum class Status { Empty, Ok, Error };

    Status status = Status::Empty;
    Statement::Kind kind = Statement::Kind::Empty;
    std::string value;   // Evaluation: the formatted result, e.g. "20 J"
    std::string display; // the region as shown when not being edited, e.g. "L·F = 20 J"
    std::string error;
    std::size_t errorBegin = 0; // byte range of the source the error refers to
    std::size_t errorEnd = 0;
};

struct Region {
    // Text regions hold notes and headings. They are laid out, but never evaluated.
    enum class Kind { Math, Text };

    int id = 0;
    Kind kind = Kind::Math;
    double x = 0.0; // left end of the (first) baseline, in 1/96 inch from the top-left of the first page
    double y = 0.0;
    double width = 0.0; // text regions wrap at this width; unused for math
    std::string source; // UTF-8; for text, lines are separated by '\n'
    RegionResult result;
};

// A page of math and text regions. Like Mathcad, it evaluates top to bottom, then left to right,
// so a name can only be used below (or to the right of) where it is defined.
class Worksheet {
public:
    int addRegion(double x, double y, std::string source = {});
    int addTextRegion(double x, double y, double width, std::string text = {});
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
    std::vector<Region> regions_;
    int nextId_ = 1;
};

} // namespace lemma
