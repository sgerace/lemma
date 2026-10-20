#pragma once

#include "lemma/math.h"
#include "lemma/math_edit.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lemma {

// Measures text in the math font, in document units. The app implements it with Qt; the
// tests use fixed metrics.
class MathFontMetrics {
public:
    struct Extent {
        double width = 0.0;   // advance
        double ascent = 0.0;  // ink above the baseline
        double descent = 0.0; // ink below the baseline
    };

    virtual ~MathFontMetrics() = default;
    // `size` is the font's em size.
    virtual Extent measure(std::string_view utf8, double size) const = 0;
};

// The math font's layout constants, in ems. These are STIX Two Math's, from its MATH
// table (fractions use the display-style values, since Lemma doesn't shrink them).
struct MathConstants {
    double scriptScale = 0.70;
    double scriptScriptScale = 0.55;
    double axisHeight = 0.258;
    double fractionNumeratorShiftUp = 0.640;
    double fractionDenominatorShiftDown = 0.640;
    double fractionGapMin = 0.150;
    double fractionRuleThickness = 0.068;
    double superscriptShiftUp = 0.360;
    double superscriptBottomMin = 0.120;
    double superscriptBaselineDropMax = 0.230;
    double subscriptShiftDown = 0.210;
    double spaceAfterScript = 0.040;
    double radicalVerticalGap = 0.085;
    double radicalRuleThickness = 0.068;
    double radicalExtraAscender = 0.078;
};

// One thing to draw, in the region's coordinates: x to the right and y down from the
// left end of its baseline.
struct MathLayoutItem {
    enum class Kind {
        Text,            // text at (x, y), its baseline start, in the math font at `size`
        Rule,            // a filled rectangle: a fraction bar
        Placeholder,     // a small filled square
        MissingOperator, // a small hollow square
        Radical,         // a polyline through `points`, `thickness` wide: the radical sign and its bar
        Delimiter,       // a tall parenthesis filling the rectangle, `text` "(" or ")"
    };

    Kind kind = Kind::Text;
    std::string text;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    double size = 0.0;
    double thickness = 0.0;
    std::vector<std::pair<double, double>> points;
};

// Where a node of the tree was laid out, in the region's coordinates.
struct MathNodeBox {
    double left = 0.0;
    double right = 0.0;
    double top = 0.0;
    double bottom = 0.0;
    double baseline = 0.0;
    double size = 0.0; // its font size
    // Number, Identifier: the x of the caret at each byte offset of the text.
    std::vector<double> offsets;
};

// Where to draw the caret's bar.
struct MathCaretGeometry {
    double x = 0.0;
    double top = 0.0;
    double bottom = 0.0;
};

struct MathLayout {
    double width = 0.0;
    double ascent = 0.0;
    double descent = 0.0;
    std::vector<MathLayoutItem> items;
    std::unordered_map<int, MathNodeBox> boxes;

    const MathNodeBox* box(int id) const;
    MathCaretGeometry caretGeometry(const MathCaret& caret) const;
    // The stop nearest a point, for a click (section 7).
    MathCaret caretAt(const MathNode& root, double x, double y) const;
    // Up or Down: into the slot above or below, nearest the caret's x (section 6.2).
    std::optional<MathCaret> vertical(const MathNode& root, const MathCaret& caret, bool up) const;
};

struct MathLayoutOptions {
    double size = 16.0;
    // An evaluation's result, shown after "=": its number, and its unit when the region
    // doesn't give one.
    std::string resultNumber;
    const MathNode* resultUnit = nullptr;
    // A symbolic evaluation's result, shown after "→".
    const MathNode* resultTree = nullptr;
    // While editing, an empty unit or keyword slot is shown as a placeholder.
    bool showEmptyUnit = false;
    // Identifiers that meant units; they are upright, and other names italic.
    std::vector<int> upright;
};

MathLayout layoutMath(const MathNode& root, const MathFontMetrics& metrics, const MathLayoutOptions& options = {},
                      const MathConstants& constants = {});

// A name as typeset: letters in math italic (U+1D44E and on), the rest as is.
std::string mathItalic(std::string_view text);

} // namespace lemma
