// Box layout of the editor tree, with stand-in font metrics: every character is half an
// em wide, 0.7 em tall and 0.2 em deep.

#include "lemma/math_layout.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

using namespace lemma;
using Catch::Approx;

namespace {

class FixedMetrics : public MathFontMetrics {
public:
    Extent measure(std::string_view text, double size) const override
    {
        const auto chars = std::count_if(text.begin(), text.end(),
                                         [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; });
        return {0.5 * size * static_cast<double>(chars), 0.7 * size, 0.2 * size};
    }
};

const FixedMetrics kMetrics;

const MathLayoutItem* findText(const MathLayout& layout, const std::string& text)
{
    const auto it = std::find_if(layout.items.begin(), layout.items.end(), [&](const MathLayoutItem& item) {
        return item.kind == MathLayoutItem::Kind::Text && item.text == text;
    });
    return it == layout.items.end() ? nullptr : &*it;
}

int count(const MathLayout& layout, MathLayoutItem::Kind kind)
{
    return static_cast<int>(std::count_if(layout.items.begin(), layout.items.end(),
                                          [&](const MathLayoutItem& item) { return item.kind == kind; }));
}

} // namespace

TEST_CASE("A fraction stacks its numerator over its denominator, across a rule on the axis")
{
    const MathNode tree = mathFromLinear("a/bc");
    const MathLayout layout = layoutMath(tree, kMetrics);
    const MathNodeBox& num = *layout.box(tree.children[0].id);
    const MathNodeBox& den = *layout.box(tree.children[1].id);

    REQUIRE(count(layout, MathLayoutItem::Kind::Rule) == 1);
    const MathLayoutItem& rule = *std::find_if(layout.items.begin(), layout.items.end(), [](const MathLayoutItem& i) {
        return i.kind == MathLayoutItem::Kind::Rule;
    });
    CHECK(num.bottom < rule.y);
    CHECK(den.top > rule.y + rule.height);
    CHECK(rule.y + rule.height / 2 == Approx(-0.258 * 16)); // centered on the math axis
    CHECK(-num.baseline >= 0.64 * 16); // at least the font's numerator shift
    // Centered, and the rule is as wide as the wider of the two.
    CHECK((num.left + num.right) / 2 == Approx((den.left + den.right) / 2));
    CHECK(rule.width >= den.right - den.left);
    CHECK(layout.ascent > 16 * 0.64);
    CHECK(layout.descent > 16 * 0.64);
}

TEST_CASE("An exponent is raised and smaller")
{
    const MathNode tree = mathFromLinear("x^2");
    const MathLayout layout = layoutMath(tree, kMetrics);
    const MathNodeBox& base = *layout.box(tree.children[0].id);
    const MathNodeBox& exponent = *layout.box(tree.children[1].id);
    CHECK(exponent.size == Approx(16 * 0.7));
    CHECK(exponent.baseline < base.baseline - 0.3 * 16);
    CHECK(exponent.left >= base.right);
    // Exponents of exponents shrink once more, and no further.
    const MathNode tower = mathFromLinear("x^y^z^w");
    const MathLayout t = layoutMath(tower, kMetrics);
    const MathNode& z = tower.children[1].children[1].children[0];
    CHECK(t.box(z.id)->size == Approx(16 * 0.55));
}

TEST_CASE("Names are italic, units and built-in functions upright")
{
    const MathNode tree = mathFromLinear("sin(x) m");
    MathLayoutOptions options;
    options.upright = {tree.children[1].id};
    const MathLayout layout = layoutMath(tree, kMetrics, options);
    CHECK(findText(layout, "sin"));
    CHECK(findText(layout, "𝑥"));
    CHECK(findText(layout, "m"));
    CHECK(mathItalic("hδ_1") == "ℎ𝛿_1");
    CHECK(mathItalic("Δ") == "Δ");
}

TEST_CASE("A name's part after an underscore is a subscript")
{
    const MathNode tree = mathFromLinear("δ_allow");
    const MathLayout layout = layoutMath(tree, kMetrics);
    const MathLayoutItem* base = findText(layout, "𝛿");
    const MathLayoutItem* sub = findText(layout, mathItalic("allow"));
    REQUIRE(base);
    REQUIRE(sub);
    CHECK(sub->y > base->y);
    CHECK(sub->size < base->size);
    // The caret can be anywhere in the name's text, including the subscript.
    const MathNodeBox& box = *layout.box(tree.id);
    CHECK(box.offsets[std::string("δ_").size()] == Approx(sub->x));
    CHECK(box.offsets.back() == Approx(box.right));
}

TEST_CASE("Implied parentheses are drawn, and grow around tall contents")
{
    CHECK(findText(layoutMath(mathFromLinear("a*(b+c)"), kMetrics), "("));
    CHECK(!findText(layoutMath(mathFromLinear("a+b*c"), kMetrics), "("));
    const MathLayout tall = layoutMath(mathFromLinear("a*(b/c + 1)"), kMetrics);
    CHECK(count(tall, MathLayoutItem::Kind::Delimiter) == 2);
    CHECK(!findText(tall, "("));
}

TEST_CASE("Radicals, placeholders and operator placeholders")
{
    const MathLayout root = layoutMath(mathFromLinear("sqrt(x)"), kMetrics);
    CHECK(count(root, MathLayoutItem::Kind::Radical) == 1);

    MathEditor e;
    e.type("a");
    e.type("+");
    const MathLayout hole = layoutMath(e.root(), kMetrics);
    CHECK(count(hole, MathLayoutItem::Kind::Placeholder) == 1);
    e.type("b");
    e.left();
    e.backspace();
    CHECK(count(layoutMath(e.root(), kMetrics), MathLayoutItem::Kind::MissingOperator) == 1);
}

TEST_CASE("An evaluation shows its result, and its unit placeholder only while editing")
{
    const MathNode tree = mathFromLinear("L =");
    MathLayoutOptions options;
    options.resultNumber = "2.5";
    const MathNode unit = MathNode::identifier("m");
    options.resultUnit = &unit;
    MathLayout layout = layoutMath(tree, kMetrics, options);
    const MathLayoutItem* equals = findText(layout, "=");
    const MathLayoutItem* number = findText(layout, "2.5");
    const MathLayoutItem* metres = findText(layout, "m"); // a unit, so upright
    REQUIRE(equals);
    REQUIRE(number);
    REQUIRE(metres);
    CHECK(equals->x < number->x);
    CHECK(number->x < metres->x);
    CHECK(count(layout, MathLayoutItem::Kind::Placeholder) == 0);

    options.showEmptyUnit = true;
    layout = layoutMath(tree, kMetrics, options);
    CHECK(count(layout, MathLayoutItem::Kind::Placeholder) == 1);
    CHECK(layout.box(tree.children[1].id)->left > metres->x);
}

TEST_CASE("Clicking finds the nearest stop, and Up and Down move between stacked slots")
{
    const MathNode tree = mathFromLinear("ab/cd");
    const MathLayout layout = layoutMath(tree, kMetrics);
    const MathNode& num = tree.children[0];
    const MathNode& den = tree.children[1];
    const MathNodeBox& n = *layout.box(num.id);

    // A click just right of "a" in the numerator.
    const MathCaret clicked = layout.caretAt(tree, n.offsets[1] + 1, n.baseline - 4);
    CHECK(clicked == MathCaret{num.id, 1});

    const auto down = layout.vertical(tree, clicked, false);
    REQUIRE(down);
    CHECK(*down == MathCaret{den.id, 1});
    CHECK(!layout.vertical(tree, clicked, true));
    CHECK(layout.vertical(tree, *down, true) == clicked);

    const MathNode power = mathFromLinear("x^2");
    const MathLayout p = layoutMath(power, kMetrics);
    const auto up = p.vertical(power, MathCaret{power.children[0].id, 1}, true);
    REQUIRE(up);
    CHECK(up->node == power.children[1].id);
}

TEST_CASE("The caret's bar spans the box it is beside")
{
    const MathNode tree = mathFromLinear("a/b");
    const MathLayout layout = layoutMath(tree, kMetrics);
    const MathCaretGeometry beside = layout.caretGeometry({tree.id, 0, MathCaret::Side::Right});
    CHECK(beside.x == Approx(layout.width));
    CHECK(beside.top == Approx(-layout.ascent));
    CHECK(beside.bottom == Approx(layout.descent));
    const MathCaretGeometry inside = layout.caretGeometry({tree.children[0].id, 1});
    CHECK(inside.bottom < 0); // in the numerator, above the rule
}
