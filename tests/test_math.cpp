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
    case MathNode::Kind::Symbolic:
        s = show(n.children[0], caret) + " → ";
        for (std::size_t i = 1; i < n.children.size(); ++i) {
            s += (i > 1 ? "; " : "") + show(n.children[i], caret);
        }
        break;
    case MathNode::Kind::Keyword:
        s = show(n.children[0], caret);
        for (std::size_t i = 1; i < n.children.size(); ++i) {
            s += ", " + show(n.children[i], caret);
        }
        break;
    case MathNode::Kind::Equation:
        s = show(n.children[0], caret) + " ⩵ " + show(n.children[1], caret);
        break;
    case MathNode::Kind::Derivative: {
        const int last = static_cast<int>(n.children.size()) - 1;
        s = "d/d" + show(n.children[0], caret);
        if (last == 2) {
            s += "^[" + show(n.children[1], caret) + "]";
        }
        s += " " + showChild(n, last, caret);
        break;
    }
    case MathNode::Kind::Integral: {
        const std::size_t count = n.children.size();
        s = "∫";
        if (count == 4) {
            s += "_[" + show(n.children[0], caret) + "]^[" + show(n.children[1], caret) + "]";
        }
        s += " " + show(n.children[count - 2], caret) + " d" + show(n.children[count - 1], caret);
        break;
    }
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
    CHECK(typed("2e-3{left}{left}{bs}") == "2e|-3"); // "2-3" isn't a number
    CHECK(typed("x1(a){home}{right}{bs}") == "x|1(a)"); // a name can't become "1"
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

TEST_CASE("? makes a derivative, & and Ctrl+I an integral (8.5)")
{
    CHECK(typed("?") == "d/d|■ ■");
    CHECK(typed("?x{right}y^2") == "d/dx y^[2|]");
    CHECK(typed("&") == "∫_[■]^[■] |■ d■"); // a definite integral starts at its integrand
    CHECK(typed("&x{tab}x{tab}0{tab}L") == "∫_[0]^[L|] x dx");
    CHECK(typed("∫") == "∫ |■ d■");
    CHECK(typed("∫x{tab}x") == "∫ x dx|");

    // A selection is the operand, and the caret goes to the first empty slot.
    CHECK(after("y(x)", "{space}?") == "d/d|■ y(x)");
    CHECK(after("y(x)", "{space}&") == "∫_[|■]^[■] y(x) d■");
    CHECK(after("y(x)", "{space}∫") == "∫ y(x) d|■");
    // Next to existing content, it multiplies (6.4).
    CHECK(typed("2?") == "2·d/d|■ ■");
    CHECK(typed("2&") == "2·∫_[■]^[■] |■ d■");

    // The variable is a name slot: no numbers or operators in it.
    CHECK(typed("?2") == "d/d|■ ■");
    CHECK(typed("?x+") == "d/dx| ■");
    CHECK(typed("∫{tab}x(") == "∫ ■ dx|");
}

TEST_CASE("^ after a derivative's variable gives it an order (8.5)")
{
    CHECK(typed("?x^2") == "d/dx^[2|] ■");
    CHECK(typed("?x^2{right}f") == "d/dx^[2] f|");
    CHECK(typed("?x{left}^") == "d/d|x ■"); // only at the end of the name
    CHECK(after("diff(f, x, 2)", "{home}{right}{right}^") == "d/dx^[2|] f"); // to the order it has
}

TEST_CASE("A derivative's operand is parenthesized as for ·; an integrand never is (2)")
{
    CHECK(after("diff(x + 1, x)", "") == "d/dx (x + 1)|");
    CHECK(after("diff(x*y, x)", "") == "d/dx x·y|");
    CHECK(after("integrate(x + 1, x)", "") == "∫ x + 1 dx|");
    CHECK(after("diff(x, x)^2", "") == "(d/dx x)^[2]|");
    CHECK(after("2*integrate(x, x)", "") == "2·∫ x dx|");
}

TEST_CASE("Backspace on a derivative's or integral's slots (9, rule 7)")
{
    // Undoing ? or & typed by mistake.
    CHECK(typed("?{bs}") == "|■");
    CHECK(typed("&{bs}") == "|■");
    CHECK(after("y(x)", "{space}?{bs}") == "|y(x)");
    CHECK(after("y(x)", "{space}∫{bs}") == "y(x)|");
    CHECK(after("y(x)", "{space}&{del}") == "∫ |y(x) d■"); // two empty limits go first
    // An empty order goes, and so do two empty limits.
    CHECK(typed("?x^{bs}") == "d/dx| ■");
    CHECK(typed("&f{tab}x{tab}{bs}") == "∫ |f dx");
    // With something else filled in, it selects the whole operator first (rule 5).
    CHECK(typed("&f{tab}x{tab}0{tab}{bs}") == "⟨∫_[0]^[■] f dx⟩|");
    CHECK(typed("?x{right}{bs}") == "⟨d/dx ■⟩|");
    CHECK(after("diff(y, x)", "{home}{right}{bs}") == "⟨d/dx y⟩|");
    CHECK(after("integrate(f, x)", "{end}{left}{del}") == "|⟨∫ f dx⟩");
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

TEST_CASE("Backspace before a statement operator, and at the ends (9, rules 6 and 8)")
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
    CHECK(linear("f()") == "f(■)"); // a call always has a slot

    // Juxtaposition next to "(" keeps its meaning: a name there would make a call.
    CHECK(linear("(a/b)(c+d)") == "a/b*(c + d)");
    const MathNode power(MathNode::Kind::Power, {MathNode::identifier("x"), MathNode::identifier("n")});
    const MathNode group(MathNode::Kind::Group, {mathFromLinear("a + b")});
    CHECK(toLinear(MathNode::binary('*', power, group, true)) == "x^n*(a + b)");
    CHECK(linear("x^2 (a+b)") == "x^2 (a + b)");
    CHECK(linear("2 (a+b)") == "2 (a + b)");

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
void checkTree(const MathNode& n, bool root, const MathNode* parent = nullptr, std::size_t index = 0)
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
    case K::Symbolic:
        // The region, or a definition's value (7.1).
        REQUIRE((root || (parent->kind == K::Definition && index == 1)));
        REQUIRE(n.children.size() >= 2);
        for (std::size_t i = 1; i < n.children.size(); ++i) {
            const K k = n.children[i].kind;
            REQUIRE((k == K::Placeholder || k == K::Identifier || k == K::Keyword));
        }
        expected = n.children.size();
        break;
    case K::Keyword:
        REQUIRE((parent && parent->kind == K::Symbolic && index >= 1));
        REQUIRE(n.children.size() >= 2);
        REQUIRE((n.children[0].kind == K::Identifier || n.children[0].kind == K::Placeholder));
        expected = n.children.size();
        break;
    case K::Equation:
        // Only a whole slot is an equation: the region, the expression, or an argument.
        REQUIRE((root || (parent->kind == K::Symbolic && index == 0) || (parent->kind == K::Keyword && index >= 1)));
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
    case K::Derivative:
    case K::Integral: {
        const bool derivative = n.kind == K::Derivative;
        REQUIRE((n.children.size() == 2 || n.children.size() == (derivative ? 3u : 4u)));
        const K variable = calculusVariable(n).kind;
        REQUIRE((variable == K::Identifier || variable == K::Placeholder));
        expected = n.children.size();
        break;
    }
    }
    REQUIRE(n.children.size() == expected);
    for (std::size_t i = 0; i < n.children.size(); ++i) {
        checkTree(n.children[i], false, &n, i);
    }
}

MathNode withoutGroups(MathNode n)
{
    if (n.kind == MathNode::Kind::Group) {
        return withoutGroups(std::move(n.children[0]));
    }
    for (MathNode& c : n.children) {
        c = withoutGroups(std::move(c));
    }
    return n;
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
                          "{del}", "{ins}", "{tab}", "{shift-tab}", "f(", "_", "→", "⩵", ";", "solve,",
                          "?", "&", "∫"};
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
                const char* pasted[] = {"(p + 1)/q", "p == q", "diff(p^2, q, 2)", "integrate(p, q, 0, 1)"};
                e.paste(mathFromLinear(pasted[random(std::size(pasted))]));
                checkEditor(e);
            }
        }
        // It lays out as it does while being edited, and every stop has a place a click
        // can find again.
        MathLayoutOptions editing;
        editing.showEmptyUnit = true;
        const MathLayout layout = layoutMath(e.root(), kSimpleMetrics, editing);
        for (const MathCaret& stop : mathStops(e.root())) {
            REQUIRE(layout.box(stop.node));
            const MathCaretGeometry g = layout.caretGeometry(stop);
            REQUIRE(g.top < g.bottom);
            const MathCaret clicked = layout.caretAt(e.root(), g.x, (g.top + g.bottom) / 2);
            REQUIRE(findMathNode(e.root(), clicked.node));
            layout.vertical(e.root(), stop, true);
            layout.vertical(e.root(), stop, false);
        }

        // Copy's text pastes back as the same tree, less the parentheses paste drops.
        if (const auto copied = e.copy()) {
            INFO(*copied);
            const MathNode& tree = e.caret().selected ? *findMathNode(e.root(), e.caret().node) : e.root();
            REQUIRE(toLinear(mathFromLinear(*copied)) == toLinear(withoutGroups(tree)));
        }

        // Whatever it ended as, it lowers or reports an error on a node of its own.
        try {
            lowerMath(e.root());
        } catch (const Error& err) {
            REQUIRE(findMathNode(e.root(), err.node()));
        }
    }
}

// Symbolic evaluation's editing, from docs/symbolic-spec.md, section 6.

TEST_CASE("Ctrl+. makes a symbolic evaluation, with the caret on the keyword (6.2)")
{
    CHECK(typed("x^2{right}+1→") == "x^[2] + 1 → |■");
    CHECK(typed("a+b{left}→") == "a + b → |■"); // the whole region, wherever the caret is
    CHECK(typed("→") == "|■");                  // nothing to evaluate
    CHECK(typed("x:→") == "x := |■");           // nothing to evaluate yet
    CHECK(typed("f(x):x^2{right}→diff,x") == "f(x) := x^[2] → diff, x|"); // a definition's value (7.1)
    CHECK(typed("f(x):x^2{right}→{bs}") == "f(x) := x^[2]|");
    CHECK(typed("x→simplify") == "x → simplify|");
}

TEST_CASE("A keyword's arguments follow a comma, and ; starts another keyword (6.2)")
{
    CHECK(typed("x→solve,x") == "x → solve, x|");
    CHECK(typed("x→substitute,a,b") == "x → substitute, a, b|");
    CHECK(typed("x→keep,P;diff,x") == "x → keep, P; diff, x|");
    CHECK(typed("x→;") == "x → ■; |■");
    // A keyword is a name: no operators, parentheses or numbers in it.
    CHECK(typed("x→so+") == "x → so|");
    CHECK(typed("x→so(") == "x → so|");
    CHECK(typed("x→2") == "x → |■");
    // Its arguments are ordinary slots.
    CHECK(typed("x→solve,x^2") == "x → solve, x^[2|]");
}

TEST_CASE("Backspace and Delete take keywords, then the arrow, away (6.2)")
{
    CHECK(typed("x→{bs}") == "x|");
    CHECK(after("x ->", "{bs}") == "x|");
    CHECK(typed("x→ab{bs}{bs}{bs}") == "x|");
    CHECK(typed("x→solve,{bs}") == "x → solve|");
    CHECK(typed("x→a;{bs}") == "x → a|");
    CHECK(typed("x→a{home}{right}{del}") == "x → |⟨a⟩");
    CHECK(after("x ->", "{home}{right}{del}") == "x|");
}

TEST_CASE("Ctrl+= makes the whole slot an equation's left side (6.3)")
{
    CHECK(typed("a+b⩵c") == "a + b ⩵ c|");
    CHECK(typed("a+b{left}⩵c") == "a + b ⩵ c|");
    CHECK(typed("a⩵b→") == "a ⩵ b → |■");
    CHECK(typed("a→solve,x⩵2") == "a → solve, x ⩵ 2|");
    CHECK(typed("a⩵b⩵") == "a ⩵ b|");  // one equals sign to an equation
    CHECK(typed("a⩵b=") == "a ⩵ b|");  // and it isn't evaluated numerically
    CHECK(typed("x:a⩵") == "x := a|"); // nor defined
    CHECK(typed("a⩵{bs}") == "a|");
    CHECK(after("a == b", "{home}{right}{del}") == "a ⩵ |⟨b⟩");
}

TEST_CASE("Symbolic evaluations and equations in linear text (6.4)")
{
    for (const char* source : {"x^2 - 1 ->", "x^2 - 5*x + 6 -> factor", "M == σ*I/c -> solve, σ",
                               "θ(x) := y(x) -> diff, x", "M_max := w*L^2/8 -> keep, w",
                               "y -> keep, P, L; diff, x", "f(x) -> substitute, x == 2",
                               "t_peak := v_y(t) == 0 -> solve, t", "θ(x) := diff(y(x), x)",
                               "diff(f, x, 2)", "integrate(f, x)", "integrate(θ(x), x, 0, L) =",
                               "2 integrate(x, x)", "diff(x^2, x) -> keep, x"}) {
        CHECK(toLinear(mathFromLinear(source)) == source);
    }
    CHECK(toLinear(mathFromLinear("a ⩵ b → simplify")) == "a == b -> simplify");
    const Statement s = lowerMath(mathFromLinear("M == σ*I/c -> solve, σ; simplify"));
    CHECK(s.kind == Statement::Kind::Symbolic);
    REQUIRE(s.keywords.size() == 2);
    CHECK(s.keywords[0].name == "solve");
    CHECK(s.keywords[0].args.size() == 1);
    CHECK(s.keywords[1].name == "simplify");
    CHECK_THROWS(mathFromLinear("x -> 2"));
    CHECK(mathFromLinear("x := 1 -> simplify").children[1].kind == MathNode::Kind::Symbolic);

    // An empty keyword copies as none; an equation pastes only as a whole slot.
    MathEditor e;
    keys(e, "x^2→");
    CHECK(e.copy() == "x^2 ->");
    MathEditor sum;
    keys(sum, "a+");
    CHECK(sum.paste(mathFromLinear("p == q")) == MathEditor::Change::None);
}

TEST_CASE("Up and Down move between a derivative's variable and order, and an integral's limits (6.2)")
{
    for (const char* source : {"diff(f, x, 3)", "integrate(f, x, 0, L)"}) {
        INFO(source);
        const MathNode tree = mathFromLinear(source);
        const MathLayout layout = layoutMath(tree, kSimpleMetrics);
        const MathCaret below{tree.children[0].id, 0, MathCaret::Side::Left, false};
        const auto up = layout.vertical(tree, below, true);
        REQUIRE(up);
        CHECK(up->node == tree.children[1].id);
        CHECK(layout.vertical(tree, *up, false)->node == tree.children[0].id);
    }
}
