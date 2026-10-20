// The editor tree and the editing model, checked against the worked examples of
// docs/editor-spec.md. States are written in the spec's notation: `|` is the caret,
// ⟨…⟩ a selection, [n / d] a fraction, b^[e] a power, √[x] a radical.

#include "lemma/error.h"
#include "lemma/math.h"
#include "lemma/math_edit.h"
#include "lemma/math_layout.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

using namespace lemma;

namespace {

std::string show(const MathNode& n, const MathCaret& caret);

std::string showChild(const MathNode& parent, int index, const MathCaret& caret)
{
    const std::string s = show(parent.children[static_cast<std::size_t>(index)], caret);
    return needsParentheses(parent, index) ? "(" + s + ")" : s;
}

std::string show(const MathNode& n, const MathCaret& caret)
{
    std::string s;
    switch (n.kind) {
    case MathNode::Kind::Number:
    case MathNode::Kind::Identifier:
        s = n.text;
        if (caret.node == n.id && !caret.selected) {
            s.insert(static_cast<std::size_t>(caret.offset), "|");
        }
        break;
    case MathNode::Kind::Placeholder:
        s = caret.node == n.id && !caret.selected ? "|■" : "■";
        break;
    case MathNode::Kind::Binary: {
        const char* op = n.op == '+' ? " + " : n.op == '-' ? " − " : n.op == 0 ? " □ " : n.juxtaposed ? " " : "·";
        s = showChild(n, 0, caret) + op + showChild(n, 1, caret);
        break;
    }
    case MathNode::Kind::Negate:
        s = "−" + showChild(n, 0, caret);
        break;
    case MathNode::Kind::Fraction:
        s = "[" + show(n.children[0], caret) + " / " + show(n.children[1], caret) + "]";
        break;
    case MathNode::Kind::Power:
        s = showChild(n, 0, caret) + "^[" + show(n.children[1], caret) + "]";
        break;
    case MathNode::Kind::Radical:
        s = "√[" + show(n.children[0], caret) + "]";
        break;
    case MathNode::Kind::Group:
        s = "(" + show(n.children[0], caret) + ")";
        break;
    case MathNode::Kind::Call:
        s = show(n.children[0], caret) + "(";
        for (std::size_t i = 1; i < n.children.size(); ++i) {
            s += (i > 1 ? ", " : "") + show(n.children[i], caret);
        }
        s += ")";
        break;
    case MathNode::Kind::Definition:
        s = show(n.children[0], caret) + " := " + show(n.children[1], caret);
        break;
    case MathNode::Kind::Evaluation:
        s = show(n.children[0], caret) + " = " + show(n.children[1], caret);
        break;
    }
    if (caret.node == n.id && caret.selected) {
        s = caret.side == MathCaret::Side::Left ? "|⟨" + s + "⟩" : "⟨" + s + "⟩|";
    } else if (caret.node == n.id && !n.isLeaf()) {
        s = caret.side == MathCaret::Side::Left ? "|" + s : s + "|";
    }
    return s;
}

std::string show(const MathEditor& e)
{
    return show(e.root(), e.caret());
}

// Types keys into the editor. Characters are typed; {name} presses a key.
void keys(MathEditor& e, std::string_view input)
{
    for (std::size_t i = 0; i < input.size();) {
        if (input[i] == '{') {
            const std::size_t close = input.find('}', i);
            const std::string_view key = input.substr(i + 1, close - i - 1);
            if (key == "space") e.grow();
            else if (key == "shift-space") e.shrink();
            else if (key == "left") e.left();
            else if (key == "right") e.right();
            else if (key == "home") e.home();
            else if (key == "end") e.end();
            else if (key == "bs") e.backspace();
            else if (key == "del") e.deleteForward();
            else if (key == "ins") e.flip();
            else if (key == "tab") e.tab(false);
            else if (key == "shift-tab") e.tab(true);
            else FAIL("unknown key " << key);
            i = close + 1;
            continue;
        }
        std::size_t length = 1;
        while (i + length < input.size() && (static_cast<unsigned char>(input[i + length]) & 0xC0) == 0x80) {
            ++length;
        }
        e.type(input.substr(i, length));
        i += length;
    }
}

std::string typed(std::string_view input)
{
    MathEditor e;
    keys(e, input);
    return show(e);
}

std::string after(std::string_view linear, std::string_view input)
{
    MathEditor e(mathFromLinear(linear));
    keys(e, input);
    return show(e);
}

} // namespace

TEST_CASE("Typing linear input follows precedence (5.3)")
{
    CHECK(typed("a+b*c") == "a + b·c|");
    CHECK(typed("a*b+c") == "a·b + c|");
    CHECK(typed("a-b-c") == "a − b − c|");
    CHECK(typed("2^3^2") == "2^[3^[2|]]");
    CHECK(typed("9.81m/s^2") == "[9.81 m / s^[2|]]");
    CHECK(typed("a+b/c") == "a + [b / c|]");
}

TEST_CASE("Typing stays in a slot until the user leaves it (5.3)")
{
    CHECK(typed("x^2+1") == "x^[2 + 1|]");
    CHECK(typed("x^2{right}+1") == "x^[2] + 1|");
    CHECK(typed("x^2{space}{space}+1") == "x^[2] + 1|");
    CHECK(typed("a/b*c") == "[a / b·c|]");
    CHECK(typed("a/b{space}{space}*c") == "[a / b]·c|");
}

TEST_CASE("A selection overrides precedence (5.3)")
{
    CHECK(typed("a+b{space}{space}*c") == "(a + b)·c|");
    CHECK(typed("a+b{space}{space}/c") == "[a + b / c|]");
    CHECK(typed("a*b{space}+c") == "a·(b + c|)");
    CHECK(typed("x{home}{space}^e") == "e|^[x]");
}

TEST_CASE("Operators on placeholders and in the middle of a leaf (5.1, 5.2)")
{
    CHECK(typed("+") == "■ + |■");
    CHECK(typed("-") == "−|■");
    CHECK(typed("a+*") == "a + ■·|■");
    CHECK(typed("abc{left}+") == "ab + |c");
    CHECK(typed("a+b{left}-") == "a + −|b");
    CHECK(typed("x^-2") == "x^[−2|]");
    CHECK(after("f(x)", "{home}-") == "−|f(x)");     // before a name is before the call
    CHECK(after("f(x)", "{home}{right}+") == "f|(x)"); // a name can't be split
}

TEST_CASE("Right steps through every stop in reading order (6.1)")
{
    MathEditor e(mathFromLinear("a/b*c"));
    e.home();
    const char* expected[] = {"|[a / b]·c", "[|a / b]·c", "[a| / b]·c", "[a / |b]·c",
                              "[a / b|]·c", "[a / b]|·c", "[a / b]·|c", "[a / b]·c|"};
    for (const char* state : expected) {
        CHECK(show(e) == state);
        e.right();
    }
    CHECK(show(e) == "[a / b]·c|"); // nothing past the last stop
    CHECK(e.right() == MathEditor::Change::None);
}

TEST_CASE("Typing next to a box multiplies (6.4)")
{
    CHECK(typed("a/b{right}x") == "[a / b]·x|");
    CHECK(typed("(a+b)2") == "(a + b)·2|");
}

TEST_CASE("Tab moves between placeholders")
{
    CHECK(typed("f(a,,{tab}") == "f(a, |■, ■)");
    CHECK(typed("f(a,,{tab}{tab}") == "f(a, ■, |■)");
    CHECK(typed("f(a,,{shift-tab}") == "f(a, |■, ■)");
}

TEST_CASE("Space grows the selection one step at a time (7)")
{
    MathEditor e(mathFromLinear("a + b + c"));
    keys(e, "{space}");
    CHECK(show(e) == "a + b + ⟨c⟩|");
    keys(e, "{space}");
    CHECK(show(e) == "⟨a + b + c⟩|");
    keys(e, "{space}");
    CHECK(show(e) == "a + b + c|"); // from the whole region, back to where it started

    keys(e, "{left}{left}{space}{space}");
    CHECK(show(e) == "⟨a + b⟩| + c");
    keys(e, "{space}");
    CHECK(show(e) == "⟨a + b + c⟩|");
    keys(e, "{shift-space}{shift-space}");
    CHECK(show(e) == "a + ⟨b⟩| + c");

    keys(e, "{ins}");
    CHECK(show(e) == "a + |⟨b⟩ + c");
    keys(e, "{left}");
    CHECK(show(e) == "a + |b + c");
    CHECK(after("f(x)", "{home}{space}{space}") == "|⟨f(x)⟩"); // a call's name grows to the call
}

TEST_CASE("Letters after a number juxtapose; letters in a row are one name (8.1)")
{
    CHECK(typed("10N") == "10 N|");
    CHECK(typed("LF") == "LF|");
    CHECK(typed("x1") == "x1|");
    CHECK(typed("2e") == "2 e|");
    CHECK(typed("2e3") == "2e3|");
    CHECK(typed("2e-3") == "2e-3|");
    CHECK(typed("2e3{bs}") == "2 e|");
    CHECK(typed("2e-{bs}") == "2 e|");
    CHECK(typed("x.") == "x|");       // literal subscripts aren't supported yet
    CHECK(typed("1.2.") == "1.2|");
    CHECK(typed("a+b{space}{space}x") == "x|"); // typing over a selection replaces it
}

TEST_CASE("Parentheses and commas (8.2)")
{
    CHECK(typed("f(") == "f(|■)");
    CHECK(typed("(") == "(|■)");
    CHECK(typed("2(") == "2·(|■)");
    CHECK(typed("b+c{space}{space}(") == "(b + c)|");
    CHECK(typed("(a+b)") == "(a + b)|");
    CHECK(typed("(a+b)*c") == "(a + b)·c|");
    CHECK(typed("a*b+c)") == "(a·b + c)|");
    CHECK(typed("a*b+c{left}{left})") == "(a·b)| + c");
    CHECK(typed("f(a,") == "f(a, |■)");
    CHECK(typed("f(a,b)") == "f(a, b)|");
    CHECK(typed("a,") == "a|");
}

TEST_CASE("Radicals (8.3)")
{
    CHECK(typed("\\") == "√[|■]");
    CHECK(typed("\\x+1") == "√[x + 1|]");
    CHECK(typed("x+1{space}{space}\\") == "√[x + 1]|");
    CHECK(typed("2\\") == "2·√[|■]");
}

TEST_CASE("Definitions and evaluations act on the whole region (8.4)")
{
    CHECK(typed("x:") == "x := |■");
    CHECK(typed("x:=2") == "x := 2|");
    CHECK(typed("x:2kN/m") == "x := [2 kN / m|]");
    CHECK(typed("f(x):x^2") == "f(x) := x^[2|]");
    CHECK(typed("a+b=") == "a + b = |■");
    CHECK(typed("a+b=ft") == "a + b = ft|");
    CHECK(typed("a+b{left}=") == "a + b = |■");
    CHECK(typed("x:2=") == "x := 2|");
    CHECK(typed(":") == "■ := |■");
}

TEST_CASE("Backspace: selections, characters and placeholders (9, rules 1-3)")
{
    CHECK(typed("a+b*c{space}{space}{bs}") == "a + |■");
    CHECK(typed("ab{bs}") == "a|");
    CHECK(typed("a{bs}") == "|■");
    CHECK(typed("10N{bs}") == "10|"); // an emptied juxtaposed name takes the space with it
    CHECK(typed("a+{bs}") == "a|");
    CHECK(typed("a/{bs}") == "a|");
    CHECK(typed("x^{bs}") == "x|");
    CHECK(typed("-{bs}") == "|■");
    CHECK(typed("f(a,{bs}") == "f(a|)");
    CHECK(typed("f({bs}") == "f|");
}

TEST_CASE("Backspace next to an operator leaves an operator placeholder (9, rule 4)")
{
    CHECK(typed("a+b{left}{bs}") == "a □ |b");
    CHECK(typed("a+b{left}{bs}*") == "a·|b");
    CHECK(typed("a+b{left}{bs}/") == "[a / |b]");
    CHECK(typed("a+b{left}{bs}{bs}") == "a □ |b"); // neither side is a placeholder
    CHECK(typed("a+b{left}{left}{del}") == "a| □ b");
}

TEST_CASE("Backspace at the start of a slot (9, rule 5)")
{
    CHECK(typed("(a+b){home}{right}{bs}") == "|a + b");
    CHECK(after("sqrt(x)", "{home}{right}{bs}") == "|x");
    MathEditor e(mathFromLinear("a/b"));
    keys(e, "{end}{left}{left}{bs}");
    CHECK(show(e) == "⟨[a / b]⟩|");
    keys(e, "{bs}");
    CHECK(show(e) == "|■");
    CHECK(after("x^2", "{end}{left}{left}{bs}") == "⟨x^[2]⟩|");
    CHECK(after("a/b", "{end}{bs}") == "⟨[a / b]⟩|"); // right after a box
    CHECK(after("-a", "{end}{left}{bs}") == "|a");
}

TEST_CASE("Backspace before a statement operator (9, rules 6-7)")
{
    CHECK(typed("x:{bs}") == "x|");
    CHECK(typed("a+b={bs}") == "a + b|");
    CHECK(after("x := 2", "{end}{left}{bs}") == "x := ⟨2⟩|");
    CHECK(after("ab", "{home}{bs}") == "|ab");
    CHECK(after("ab", "{end}{del}") == "ab|");
}

TEST_CASE("Delete mirrors Backspace")
{
    CHECK(after("ab", "{home}{del}") == "|b");
    CHECK(typed("(a+b){left}{del}") == "a + b|");
    CHECK(after("a/b", "{home}{del}") == "|⟨[a / b]⟩");
    CHECK(after("-a", "{home}{del}") == "|a");
}

TEST_CASE("Copy writes linear text; paste parses it (10)")
{
    MathEditor e;
    CHECK(e.paste(mathFromLinear("x^2+1")) == MathEditor::Change::Edited);
    CHECK(show(e) == "x^[2] + 1|");
    CHECK(e.copy() == "x^2 + 1");
    keys(e, "{left}{space}");
    CHECK(e.copy() == "1");
    keys(e, "{right}*");
    CHECK(!e.copy().has_value()); // a placeholder has no linear form
    keys(e, "{bs}");
    keys(e, "{space}{space}");
    CHECK(e.paste(mathFromLinear("2 m")) == MathEditor::Change::Edited);
    CHECK(show(e) == "2 m|");

    CHECK(after("a = ", "{end}") == "a = |■");
    MathEditor evaluation(mathFromLinear("a ="));
    CHECK(evaluation.copy() == "a ="); // an empty unit is fine
}

TEST_CASE("Linear syntax round-trips values and tidies spacing")
{
    const auto linear = [](std::string_view s) { return toLinear(mathFromLinear(s)); };
    CHECK(linear("w:= 2 kN/m") == "w := 2 kN/m");
    CHECK(linear("(a+b)/c") == "(a + b)/c");
    CHECK(linear("a/(b c)") == "a/(b c)");
    CHECK(linear("a/(b)") == "a/b");
    CHECK(linear("f(x, y):=x y") == "f(x, y) := x y");
    CHECK(linear("x =") == "x =");
    CHECK(linear("x = ft") == "x = ft");
    CHECK(linear("sqrt(16 m^2) =") == "sqrt(16 m^2) =");
    CHECK(linear("-x^2") == "-x^2");
    CHECK(linear("M (h/2)") == "M(h/2)"); // a name before parentheses is a call, as the parser reads it
    CHECK(linear("2 (h/2)") == "2 (h/2)");
    CHECK(linear("2.50e3 kg") == "2.50e3 kg");
    CHECK(linear("a - (b - c)") == "a - (b - c)");
    CHECK(linear("2^3^2") == "2^3^2");
    CHECK(linear("(2^3)^2") == "(2^3)^2");

    // Parsing gives the 2D structure.
    CHECK(show(mathFromLinear("w L^4 / (8 E I)"), {}) == "[w L^[4] / 8 E I]");
    CHECK(show(mathFromLinear("sqrt(x + 1)"), {}) == "√[x + 1]");
    CHECK(show(mathFromLinear("a*(b+c)"), {}) == "a·(b + c)");
}

TEST_CASE("Lowering names the node an error is about")
{
    const auto errorNode = [](const MathNode& root) {
        try {
            lowerMath(root);
        } catch (const Error& e) {
            return e.node();
        }
        return 0;
    };
    MathEditor e;
    keys(e, "x:");
    CHECK(errorNode(e.root()) == e.root().children[1].id); // fill in the placeholder

    const MathNode bad = mathFromLinear("2 + 3");
    MathEditor definition(bad);
    keys(definition, ":1");
    CHECK(errorNode(definition.root()) == bad.id); // only a name or a function can be defined

    const Statement s = lowerMath(mathFromLinear("f(x, y) := x y"));
    CHECK(s.kind == Statement::Kind::FunctionDefinition);
    CHECK(s.params == std::vector<std::string>{"x", "y"});
    CHECK(lowerMath(mathFromLinear("x =")).kind == Statement::Kind::Evaluation);
    CHECK(lowerMath(MathNode()).kind == Statement::Kind::Empty);
}

namespace {

class SimpleMetrics : public MathFontMetrics {
public:
    Extent measure(std::string_view text, double size) const override
    {
        return {0.5 * size * static_cast<double>(text.size()), 0.7 * size, 0.2 * size};
    }
};

const SimpleMetrics kSimpleMetrics;

// The shape every tree must keep, whatever is typed.
void checkTree(const MathNode& n, bool root)
{
    using K = MathNode::Kind;
    INFO(toLinear(n));
    std::size_t expected = 0;
    switch (n.kind) {
    case K::Number:
    case K::Identifier:
        REQUIRE(!n.text.empty());
        break;
    case K::Placeholder:
        break;
    case K::Binary:
    case K::Fraction:
    case K::Power:
        expected = 2;
        break;
    case K::Definition:
    case K::Evaluation:
        REQUIRE(root); // statements are only ever the whole region
        expected = 2;
        break;
    case K::Negate:
    case K::Radical:
    case K::Group:
        expected = 1;
        break;
    case K::Call:
        REQUIRE(n.children.size() >= 2);
        REQUIRE((n.children[0].kind == K::Identifier || n.children[0].kind == K::Placeholder));
        expected = n.children.size();
        break;
    }
    REQUIRE(n.children.size() == expected);
    for (const MathNode& c : n.children) {
        checkTree(c, false);
    }
}

void checkEditor(const MathEditor& e)
{
    checkTree(e.root(), true);
    const MathCaret& c = e.caret();
    if (c.selected) {
        REQUIRE(findMathNode(e.root(), c.node));
    } else {
        const std::vector<MathCaret> stops = mathStops(e.root());
        REQUIRE(std::find(stops.begin(), stops.end(), c) != stops.end());
    }
}

} // namespace

TEST_CASE("Any sequence of keys leaves a valid tree and caret")
{
    const char* keys[] = {"a", "b", "x", "2", "1", ".", "e", "+", "-", "*", "/", "^", "(", ")", ",", "\\",
                          ":", "=", "{space}", "{shift-space}", "{left}", "{right}", "{home}", "{end}", "{bs}",
                          "{del}", "{ins}", "{tab}", "{shift-tab}", "f(", "_"};
    std::uint32_t seed = 12345;
    const auto random = [&](std::size_t n) {
        seed = seed * 1664525u + 1013904223u; // a fixed sequence, so failures reproduce
        return static_cast<std::size_t>((seed >> 8) % n);
    };
    for (int run = 0; run < 1000; ++run) {
        MathEditor e;
        std::string history;
        for (int i = 0; i < 25; ++i) {
            const char* key = keys[random(std::size(keys))];
            history += key;
            INFO(history);
            ::keys(e, key);
            checkEditor(e);
            if (random(20) == 0) {
                e.paste(mathFromLinear("(p + 1)/q"));
                checkEditor(e);
            }
        }
        // It lays out, and every stop has a place a click can find again.
        const MathLayout layout = layoutMath(e.root(), kSimpleMetrics);
        for (const MathCaret& stop : mathStops(e.root())) {
            REQUIRE(layout.box(stop.node));
            const MathCaretGeometry g = layout.caretGeometry(stop);
            REQUIRE(g.top < g.bottom);
            const MathCaret clicked = layout.caretAt(e.root(), g.x, (g.top + g.bottom) / 2);
            REQUIRE(findMathNode(e.root(), clicked.node));
            layout.vertical(e.root(), stop, true);
            layout.vertical(e.root(), stop, false);
        }

        // Whatever it ended as, it lowers or reports an error on a node of its own.
        try {
            lowerMath(e.root());
        } catch (const Error& err) {
            REQUIRE(findMathNode(e.root(), err.node()));
        }
    }
}
