#include "lemma/math_layout.h"

#include "lemma/evaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace lemma {

using Kind = MathNode::Kind;

namespace {

// ---------------------------------------------------------------------------
// UTF-8

char32_t decode(std::string_view s, std::size_t& i)
{
    const auto c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80) {
        return c;
    }
    const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    char32_t cp = c & (0x3F >> extra);
    for (int k = 0; k < extra && i < s.size(); ++k) {
        cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
    }
    return cp;
}

void encode(char32_t cp, std::string& out)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// ---------------------------------------------------------------------------
// Boxes

// A laid-out piece, with its origin at the left end of its baseline.
struct Box {
    double width = 0.0;
    double ascent = 0.0;
    double descent = 0.0;
    std::vector<MathLayoutItem> items;
    std::vector<std::pair<int, MathNodeBox>> nodes;

    // Adds `child` with its origin at (dx, dy), without changing this box's extent.
    void place(Box child, double dx, double dy)
    {
        for (MathLayoutItem& item : child.items) {
            item.x += dx;
            item.y += dy;
            for (auto& p : item.points) {
                p.first += dx;
                p.second += dy;
            }
            items.push_back(std::move(item));
        }
        for (auto& [id, b] : child.nodes) {
            b.left += dx;
            b.right += dx;
            b.top += dy;
            b.bottom += dy;
            b.baseline += dy;
            for (double& x : b.offsets) {
                x += dx;
            }
            nodes.emplace_back(id, std::move(b));
        }
    }

    // Adds `child` on the right, `gap` after what's there.
    void append(Box child, double gap = 0.0)
    {
        const double x = width + gap;
        width = x + child.width;
        ascent = std::max(ascent, child.ascent);
        descent = std::max(descent, child.descent);
        place(std::move(child), x, 0.0);
    }

    void record(int id, double size, std::vector<double> offsets = {})
    {
        nodes.emplace_back(id, MathNodeBox{0.0, width, -ascent, descent, 0.0, size, std::move(offsets)});
    }
};

class Layouter {
public:
    Layouter(const MathFontMetrics& metrics, const MathLayoutOptions& options, const MathConstants& constants)
        : metrics_(metrics), options_(options), c_(constants)
    {
    }

    Box statement(const MathNode& root)
    {
        switch (root.kind) {
        case Kind::Definition: {
            Box box = node(root.children[0], 0);
            box.append(text(":=", size(0)), relationSpace());
            box.append(node(root.children[1], 0), relationSpace());
            box.record(root.id, size(0)); // so a selection of the whole region can be underlined
            return box;
        }
        case Kind::Evaluation: {
            Box box = evaluation(root);
            box.record(root.id, size(0));
            return box;
        }
        case Kind::Symbolic: {
            Box box = symbolic(root);
            box.record(root.id, size(0));
            return box;
        }
        default:
            return node(root, 0);
        }
    }

private:
    double size(int level) const
    {
        return options_.size * (level == 0 ? 1.0 : level == 1 ? c_.scriptScale : c_.scriptScriptScale);
    }

    double relationSpace() const { return options_.size * 5.0 / 18.0; }

    Box text(const std::string& s, double sz) const
    {
        const MathFontMetrics::Extent e = metrics_.measure(s, sz);
        Box box;
        box.width = e.width;
        box.ascent = e.ascent;
        box.descent = e.descent;
        MathLayoutItem item;
        item.kind = MathLayoutItem::Kind::Text;
        item.text = s;
        item.size = sz;
        box.items.push_back(std::move(item));
        return box;
    }

    bool isUpright(const MathNode& n) const
    {
        return forceUpright_ || std::find(options_.upright.begin(), options_.upright.end(), n.id) != options_.upright.end();
    }

    // A Number or Identifier, with the caret position of every byte of its text. A name's
    // part after "_" is shown as a subscript, as in δ_allow.
    Box leaf(const MathNode& n, int level, bool upright)
    {
        const double sz = size(level);
        const auto shown = [&](std::string_view s) {
            return n.kind == Kind::Identifier && !upright ? mathItalic(s) : std::string(s);
        };
        const std::size_t underscore = n.kind == Kind::Identifier ? n.text.find('_', 1) : std::string::npos;
        const std::string_view base = std::string_view(n.text).substr(0, underscore);

        Box box = text(shown(base), sz);
        std::vector<double> offsets(n.text.size() + 1, 0.0);
        for (std::size_t i = 1; i <= base.size(); ++i) {
            offsets[i] = metrics_.measure(shown(base.substr(0, i)), sz).width;
        }
        if (underscore != std::string::npos) {
            const double subSize = size(std::min(level + 1, 2));
            const std::string_view sub = std::string_view(n.text).substr(underscore + 1);
            Box script = text(shown(sub), subSize);
            const double x = box.width;
            const double drop = c_.subscriptShiftDown * sz;
            for (std::size_t i = 0; i <= sub.size(); ++i) {
                offsets[underscore + 1 + i] = x + metrics_.measure(shown(sub.substr(0, i)), subSize).width;
            }
            offsets[underscore] = x;
            box.width = x + script.width;
            box.ascent = std::max(box.ascent, script.ascent - drop);
            box.descent = std::max(box.descent, script.descent + drop);
            box.place(std::move(script), x, drop);
        }
        box.record(n.id, sz, std::move(offsets));
        return box;
    }

    Box square(MathLayoutItem::Kind kind, double sz, double side, double raise)
    {
        const double bearing = 0.08 * sz;
        Box box;
        box.width = side + 2 * bearing;
        box.ascent = side + raise;
        box.descent = std::max(0.0, -raise);
        MathLayoutItem item;
        item.kind = kind;
        item.x = bearing;
        item.y = -(side + raise);
        item.width = side;
        item.height = side;
        box.items.push_back(item);
        return box;
    }

    // Parentheses around a box: the font's at their natural size if the box fits, drawn
    // taller, centered on the math axis, if not.
    Box parenthesize(Box inner, double sz)
    {
        const MathFontMetrics::Extent paren = metrics_.measure("(", sz);
        Box box;
        if (inner.ascent <= paren.ascent * 1.05 && inner.descent <= paren.descent * 1.1) {
            box.append(text("(", sz));
            box.append(std::move(inner));
            box.append(text(")", sz));
            return box;
        }
        const double axis = c_.axisHeight * sz;
        const double half = std::max(inner.ascent - axis, inner.descent + axis) + 0.1 * sz;
        const double width = 0.33 * sz;
        const auto delimiter = [&](const char* which) {
            Box d;
            d.width = width;
            d.ascent = axis + half;
            d.descent = half - axis;
            MathLayoutItem item;
            item.kind = MathLayoutItem::Kind::Delimiter;
            item.text = which;
            item.y = -(axis + half);
            item.width = width;
            item.height = 2 * half;
            item.thickness = c_.fractionRuleThickness * sz;
            d.items.push_back(std::move(item));
            return d;
        };
        box.append(delimiter("("));
        box.append(std::move(inner));
        box.append(delimiter(")"));
        return box;
    }

    Box child(const MathNode& parent, int index, int level)
    {
        Box box = node(parent.children[static_cast<std::size_t>(index)], level);
        return needsParentheses(parent, index) ? parenthesize(std::move(box), size(level)) : box;
    }

    Box node(const MathNode& n, int level)
    {
        const double sz = size(level);
        Box box;
        switch (n.kind) {
        case Kind::Number:
            return leaf(n, level, true);
        case Kind::Identifier:
            return leaf(n, level, isUpright(n));
        case Kind::Placeholder:
            box = square(MathLayoutItem::Kind::Placeholder, sz, 0.45 * sz, 0.0);
            break;
        case Kind::Binary: {
            box = child(n, 0, level);
            const bool script = level > 0;
            if (n.op == 0) {
                const double side = 0.36 * sz;
                box.append(square(MathLayoutItem::Kind::MissingOperator, sz, side, c_.axisHeight * sz - side / 2),
                           (script ? 0.05 : 0.15) * sz);
                box.append(child(n, 1, level), (script ? 0.05 : 0.15) * sz);
            } else if (n.op == '*' && n.juxtaposed) {
                box.append(child(n, 1, level), (script ? 0.10 : 3.0 / 18.0) * sz);
            } else if (n.op == ',') {
                box.append(text(",", sz)); // a list of solutions
                box.append(child(n, 1, level), 0.6 * sz);
            } else {
                const double space = (n.op == '*' ? (script ? 0.03 : 2.0 / 18.0) : (script ? 0.08 : 4.0 / 18.0)) * sz;
                box.append(text(n.op == '+' ? "+" : n.op == '-' ? "−" : n.op == '~' ? "±" : "⋅", sz), space);
                box.append(child(n, 1, level), space);
            }
            break;
        }
        case Kind::Negate:
            box = text(n.op == '~' ? "±" : "−", sz);
            box.append(child(n, 0, level));
            break;
        case Kind::Fraction:
            box = fraction(n, level);
            break;
        case Kind::Power:
            box = power(n, level);
            break;
        case Kind::Radical:
            box = radical(n, level);
            break;
        case Kind::Group:
            box = parenthesize(node(n.children[0], level), sz);
            break;
        case Kind::Call: {
            const MathNode& name = n.children[0];
            const bool upright = isUpright(n) || (name.kind == Kind::Identifier && isBuiltinFunction(name.text));
            box = name.kind == Kind::Identifier ? leaf(name, level, upright || isUpright(name)) : node(name, level);
            Box args;
            for (std::size_t i = 1; i < n.children.size(); ++i) {
                if (i > 1) {
                    args.append(text(",", sz));
                }
                args.append(node(n.children[i], level), i > 1 ? 0.2 * sz : 0.0);
            }
            box.append(parenthesize(std::move(args), sz), 0.03 * sz);
            break;
        }
        case Kind::Keyword:
            box = keyword(n, level);
            break;
        case Kind::Equation:
            box = child(n, 0, level);
            box.append(boldEquals(sz), relationSpace());
            box.append(child(n, 1, level), relationSpace());
            break;
        case Kind::Definition:
        case Kind::Evaluation:
        case Kind::Symbolic:
            return statement(n);
        }
        box.record(n.id, sz);
        return box;
    }

    // A keyword's name, upright like a built-in function's, then its arguments: solve, σ.
    Box keyword(const MathNode& n, int level)
    {
        const double sz = size(level);
        const MathNode& name = n.children[0];
        Box box = name.kind == Kind::Identifier ? leaf(name, level, true) : node(name, level);
        for (std::size_t i = 1; i < n.children.size(); ++i) {
            box.append(text(",", sz));
            box.append(node(n.children[i], level), 0.25 * sz);
        }
        return box;
    }

    // An equation's equals sign, bold to tell it from an evaluation's (symbolic spec, 6.3).
    Box boldEquals(double sz) const
    {
        const MathFontMetrics::Extent e = metrics_.measure("=", sz);
        const double thickness = 0.12 * sz;
        const double half = 0.14 * sz;
        const double axis = c_.axisHeight * sz;
        Box box;
        box.width = e.width;
        box.ascent = axis + half + thickness / 2;
        box.descent = std::max(0.0, -(axis - half - thickness / 2));
        for (const double centre : {-axis - half, -axis + half}) {
            MathLayoutItem bar;
            bar.kind = MathLayoutItem::Kind::Rule;
            bar.x = 0.06 * sz;
            bar.y = centre - thickness / 2;
            bar.width = e.width - 0.12 * sz;
            bar.height = thickness;
            box.items.push_back(bar);
        }
        return box;
    }

    // expression → result, with the keywords above the arrow (symbolic spec, 6.1). With
    // none, the keyword slot is hidden unless the region is being edited.
    Box symbolic(const MathNode& root)
    {
        const double sz = size(0);
        Box box = node(root.children[0], 0);

        const bool shown = options_.showEmptyUnit
                           || std::any_of(root.children.begin() + 1, root.children.end(),
                                          [](const MathNode& k) { return k.kind != Kind::Placeholder; });
        Box keywords;
        for (std::size_t i = 1; i < root.children.size(); ++i) {
            const MathNode& k = root.children[i];
            if (!shown) {
                Box empty; // not shown, but still somewhere for the caret
                empty.record(k.id, size(1));
                keywords.append(std::move(empty));
                continue;
            }
            if (i > 1) {
                keywords.append(text(";", size(1)));
            }
            keywords.append(k.kind == Kind::Identifier ? leaf(k, 1, true) : node(k, 1), i > 1 ? 0.3 * size(1) : 0.0);
        }

        const MathFontMetrics::Extent glyph = metrics_.measure("→", sz);
        const double length = std::max(glyph.width, keywords.width + 0.4 * sz);
        Box arrow;
        arrow.width = length;
        arrow.ascent = glyph.ascent;
        arrow.descent = glyph.descent;
        if (length > glyph.width) {
            // A longer shaft under long keywords, ending in the arrow's head.
            const double thickness = c_.fractionRuleThickness * sz;
            MathLayoutItem shaft;
            shaft.kind = MathLayoutItem::Kind::Rule;
            shaft.x = 0.05 * sz;
            shaft.y = -c_.axisHeight * sz - thickness / 2;
            shaft.width = length - glyph.width * 0.6;
            shaft.height = thickness;
            arrow.items.push_back(shaft);
        }
        arrow.place(text("→", sz), length - glyph.width, 0.0);
        const double keywordsWidth = keywords.width;
        const double lift = glyph.ascent + 0.12 * sz + keywords.descent;
        arrow.ascent = std::max(arrow.ascent, lift + keywords.ascent);
        arrow.place(std::move(keywords), (length - keywordsWidth) / 2, -lift);
        box.append(std::move(arrow), relationSpace());

        if (options_.resultTree) {
            box.append(node(*options_.resultTree, 0), relationSpace());
        }
        return box;
    }

    Box fraction(const MathNode& n, int level)
    {
        const double sz = size(level);
        Box num = node(n.children[0], level);
        Box den = node(n.children[1], level);
        const double pad = 0.12 * sz;
        const double thickness = c_.fractionRuleThickness * sz;
        const double axis = c_.axisHeight * sz;
        const double gap = c_.fractionGapMin * sz;
        const double up = std::max(c_.fractionNumeratorShiftUp * sz, num.descent + gap + axis + thickness / 2);
        const double down = std::max(c_.fractionDenominatorShiftDown * sz, den.ascent + gap - axis + thickness / 2);

        Box box;
        box.width = std::max(num.width, den.width) + 2 * pad;
        box.ascent = up + num.ascent;
        box.descent = down + den.descent;
        MathLayoutItem rule;
        rule.kind = MathLayoutItem::Kind::Rule;
        rule.x = pad / 2;
        rule.y = -axis - thickness / 2;
        rule.width = box.width - pad;
        rule.height = thickness;
        box.items.push_back(rule);
        const double numWidth = num.width;
        const double denWidth = den.width;
        box.place(std::move(num), (box.width - numWidth) / 2, -up);
        box.place(std::move(den), (box.width - denWidth) / 2, down);
        return box;
    }

    Box power(const MathNode& n, int level)
    {
        const double sz = size(level);
        Box box = child(n, 0, level);
        const int scriptLevel = std::min(level + 1, 2);
        Box exponent = node(n.children[1], scriptLevel);
        const double up = std::max({c_.superscriptShiftUp * sz, exponent.descent + c_.superscriptBottomMin * sz,
                                    box.ascent - c_.superscriptBaselineDropMax * sz});
        const double x = box.width + 0.03 * sz;
        box.width = x + exponent.width + c_.spaceAfterScript * sz;
        box.ascent = std::max(box.ascent, up + exponent.ascent);
        box.descent = std::max(box.descent, exponent.descent - up);
        box.place(std::move(exponent), x, -up);
        return box;
    }

    Box radical(const MathNode& n, int level)
    {
        const double sz = size(level);
        Box inner = node(n.children[0], level);
        const double thickness = c_.radicalRuleThickness * sz;
        const double top = std::max(inner.ascent, 0.5 * sz) + c_.radicalVerticalGap * sz + thickness;
        const double bottom = inner.descent + 0.06 * sz;
        const double height = top + bottom;
        const double sign = 0.5 * sz + 0.05 * height;
        const double bar = -top + thickness / 2;
        const double innerX = sign + 0.06 * sz;

        Box box;
        box.width = innerX + inner.width + 0.1 * sz;
        box.ascent = top + c_.radicalExtraAscender * sz;
        box.descent = bottom;
        MathLayoutItem item;
        item.kind = MathLayoutItem::Kind::Radical;
        item.thickness = thickness;
        item.points = {{0.02 * sz, -top + height * 0.58},
                       {sign * 0.22, -top + height * 0.52},
                       {sign * 0.5, bottom - thickness / 2},
                       {sign * 0.95, bar},
                       {box.width - 0.04 * sz, bar}};
        box.items.push_back(std::move(item));
        box.place(std::move(inner), innerX, 0.0);
        return box;
    }

    Box evaluation(const MathNode& root)
    {
        const double sz = size(0);
        const double thin = 3.0 / 18.0 * sz;
        Box box = node(root.children[0], 0);
        box.append(text("=", sz), relationSpace());
        double gap = relationSpace();
        if (!options_.resultNumber.empty()) {
            box.append(text(options_.resultNumber, sz), gap);
            gap = thin;
        }
        const MathNode& unit = root.children[1];
        if (unit.kind != Kind::Placeholder) {
            box.append(node(unit, 0), gap);
        } else {
            if (options_.resultUnit) {
                forceUpright_ = true;
                box.append(node(*options_.resultUnit, 0), gap);
                forceUpright_ = false;
            }
            if (options_.showEmptyUnit) {
                box.append(node(unit, 0), thin);
            } else {
                // Not shown, but still somewhere for the caret.
                Box empty;
                empty.record(unit.id, sz);
                box.append(std::move(empty));
            }
        }
        return box;
    }

    const MathFontMetrics& metrics_;
    const MathLayoutOptions& options_;
    const MathConstants& c_;
    bool forceUpright_ = false;
};

} // namespace

std::string mathItalic(std::string_view text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        char32_t cp = decode(text, i);
        if (cp == U'h') {
            cp = 0x210E; // the one hole in the italic alphabet: Planck's constant
        } else if (cp >= U'a' && cp <= U'z') {
            cp = 0x1D44E + (cp - U'a');
        } else if (cp >= U'A' && cp <= U'Z') {
            cp = 0x1D434 + (cp - U'A');
        } else if (cp >= 0x3B1 && cp <= 0x3C9) {
            cp = 0x1D6FC + (cp - 0x3B1); // lowercase Greek; capitals stay upright, as in TeX
        }
        encode(cp, out);
    }
    return out;
}

MathLayout layoutMath(const MathNode& root, const MathFontMetrics& metrics, const MathLayoutOptions& options,
                      const MathConstants& constants)
{
    Box box = Layouter(metrics, options, constants).statement(root);
    MathLayout layout;
    layout.width = box.width;
    layout.ascent = box.ascent;
    layout.descent = box.descent;
    layout.items = std::move(box.items);
    for (auto& [id, b] : box.nodes) {
        layout.boxes[id] = std::move(b);
    }
    return layout;
}

// ---------------------------------------------------------------------------
// Carets

const MathNodeBox* MathLayout::box(int id) const
{
    const auto it = boxes.find(id);
    return it == boxes.end() ? nullptr : &it->second;
}

MathCaretGeometry MathLayout::caretGeometry(const MathCaret& caret) const
{
    const MathNodeBox* b = box(caret.node);
    if (!b) {
        return {width, -ascent, descent};
    }
    // A text caret spans its line, at the size of the text it's in.
    const double top = b->baseline - 0.8 * b->size;
    const double bottom = b->baseline + 0.25 * b->size;
    if (caret.selected) {
        return {caret.side == MathCaret::Side::Left ? b->left : b->right, std::min(top, b->top),
                std::max(bottom, b->bottom)};
    }
    if (!b->offsets.empty()) {
        const auto i = static_cast<std::size_t>(std::clamp(caret.offset, 0, static_cast<int>(b->offsets.size()) - 1));
        return {b->offsets[i], top, bottom};
    }
    const double x = caret.side == MathCaret::Side::Left ? b->left : b->right;
    return {x, std::min(top, b->top), std::max(bottom, b->bottom)};
}

MathCaret MathLayout::caretAt(const MathNode& root, double x, double y) const
{
    MathCaret best;
    double bestScore = std::numeric_limits<double>::max();
    double bestHeight = 0.0;
    for (const MathCaret& stop : mathStops(root)) {
        const MathCaretGeometry g = caretGeometry(stop);
        double gx = g.x;
        if (const MathNodeBox* b = box(stop.node); b && b->offsets.empty() && findMathNode(root, stop.node)->isLeaf()) {
            gx = (b->left + b->right) / 2; // a placeholder: anywhere on it
        }
        const double dy = y < g.top ? g.top - y : y > g.bottom ? y - g.bottom : 0.0;
        const double score = std::abs(x - gx) + 3.0 * dy;
        const double height = g.bottom - g.top;
        // On a tie, the smaller (more deeply nested) stop wins.
        if (score < bestScore - 1e-9 || (score < bestScore + 1e-9 && height < bestHeight)) {
            best = stop;
            bestScore = score;
            bestHeight = height;
        }
    }
    return best;
}

std::optional<MathCaret> MathLayout::vertical(const MathNode& root, const MathCaret& caret, bool up) const
{
    const double x = caretGeometry(caret).x;
    for (int id = caret.node;;) {
        int index = 0;
        const MathNode* p = parentOf(root, id, &index);
        if (!p) {
            return std::nullopt;
        }
        const MathNode* target = nullptr;
        if (p->kind == Kind::Fraction && (up ? index == 1 : index == 0)) {
            target = &p->children[up ? 0 : 1];
        } else if (p->kind == Kind::Power && (up ? index == 0 : index == 1)) {
            target = &p->children[up ? 1 : 0];
        }
        if (target) {
            MathCaret best;
            double bestDistance = std::numeric_limits<double>::max();
            for (const MathCaret& stop : mathStops(*target)) {
                const double distance = std::abs(caretGeometry(stop).x - x);
                if (distance < bestDistance) {
                    best = stop;
                    bestDistance = distance;
                }
            }
            return best;
        }
        id = p->id;
    }
}

} // namespace lemma
