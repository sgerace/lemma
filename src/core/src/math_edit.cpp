#include "lemma/math_edit.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace lemma {

using Kind = MathNode::Kind;
using Side = MathCaret::Side;

namespace {

// ---------------------------------------------------------------------------
// Stops

bool isText(const MathNode& n)
{
    return n.kind == Kind::Number || n.kind == Kind::Identifier;
}

bool isContinuation(char c)
{
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

void collectStops(const MathNode& n, std::vector<MathCaret>& out)
{
    const auto beside = [&](Side side) { out.push_back({n.id, 0, side, false}); };
    switch (n.kind) {
    case Kind::Number:
    case Kind::Identifier:
        for (std::size_t i = 0; i <= n.text.size(); ++i) {
            if (i == n.text.size() || !isContinuation(n.text[i])) {
                out.push_back({n.id, static_cast<int>(i), Side::Left, false});
            }
        }
        return;
    case Kind::Placeholder:
        out.push_back({n.id, 0, Side::Left, false});
        return;
    case Kind::Negate:
    case Kind::Fraction:
    case Kind::Radical:
    case Kind::Group:
        // These have something of their own on the left: the minus sign, the rule, the
        // radical sign, the parenthesis.
        beside(Side::Left);
        break;
    default:
        break;
    }
    for (const MathNode& child : n.children) {
        collectStops(child, out);
    }
    if (n.isBox()) {
        beside(Side::Right);
    }
}

MathCaret leftEdge(const MathNode& n)
{
    std::vector<MathCaret> stops;
    collectStops(n, stops);
    return stops.front();
}

MathCaret rightEdge(const MathNode& n)
{
    std::vector<MathCaret> stops;
    collectStops(n, stops);
    return stops.back();
}

// ---------------------------------------------------------------------------
// Characters

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isIdentStart(unsigned char c) { return std::isalpha(c) || c == '_' || c >= 0x80; }
bool isIdentChar(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }

// Splits a leaf's text the way the parser's lexer would, so editing a leaf behaves like
// editing linear text: "10N" is a number then a name, "2e3" one number. Only a single
// token, or a number followed by a name, can be a leaf (or a juxtaposition); anything
// else gives an empty result. A dangling exponent sign ("2e-") is kept in the number,
// which is incomplete until a digit follows.
std::vector<MathNode> lexLeaf(const std::string& text)
{
    std::vector<MathNode> tokens;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t start = i;
        if (isDigit(text[i]) || text[i] == '.') {
            while (i < text.size() && isDigit(text[i])) ++i;
            if (i < text.size() && text[i] == '.') {
                ++i;
                while (i < text.size() && isDigit(text[i])) ++i;
            }
            if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
                std::size_t j = i + 1;
                const bool sign = j < text.size() && (text[j] == '+' || text[j] == '-');
                if (sign) ++j;
                if (j < text.size() && isDigit(text[j])) {
                    i = j;
                    while (i < text.size() && isDigit(text[i])) ++i;
                } else if (sign && j == text.size()) {
                    i = j;
                }
            }
            tokens.push_back(MathNode::number(text.substr(start, i - start)));
        } else if (isIdentStart(static_cast<unsigned char>(text[i]))) {
            while (i < text.size() && isIdentChar(static_cast<unsigned char>(text[i]))) ++i;
            tokens.push_back(MathNode::identifier(text.substr(start, i - start)));
        } else {
            return {};
        }
    }
    const bool single = tokens.size() == 1;
    const bool juxtaposed = tokens.size() == 2 && tokens[0].kind == Kind::Number && tokens[1].kind == Kind::Identifier;
    if (!single && !juxtaposed) {
        return {};
    }
    return tokens;
}

bool hasExponent(const std::string& number)
{
    return number.find_first_of("eE") != std::string::npos;
}

// Operators and their precedence (section 2), loosest first.
int precedence(char op)
{
    switch (op) {
    case '+':
    case '-':
        return 1;
    case '^':
        return 4;
    default:
        return 2; // '*', '/', and an operator placeholder
    }
}

int precedence(const MathNode& n)
{
    if (n.kind == Kind::Negate) {
        return 3;
    }
    return precedence(n.op);
}

bool leftAssociative(char op)
{
    return op != '^';
}

MathNode wrapped(char op, MathNode first, MathNode second)
{
    switch (op) {
    case '/':
        return MathNode(Kind::Fraction, {std::move(first), std::move(second)});
    case '^':
        return MathNode(Kind::Power, {std::move(first), std::move(second)});
    default:
        return MathNode::binary(op, std::move(first), std::move(second));
    }
}

} // namespace

std::vector<MathCaret> mathStops(const MathNode& root)
{
    std::vector<MathCaret> stops;
    collectStops(root, stops);
    return stops;
}

// ---------------------------------------------------------------------------
// The editor

MathEditor::MathEditor(MathNode root) : root_(std::move(root))
{
    caret_ = rightEdge(root_);
}

MathEditor::MathEditor(MathNode root, const MathCaret& caret) : root_(std::move(root))
{
    setCaret(caret);
}

void MathEditor::setCaret(const MathCaret& caret)
{
    grown_.clear();
    afterColon_ = false;
    const MathNode* n = findMathNode(root_, caret.node);
    if (!n) {
        caret_ = rightEdge(root_);
        return;
    }
    if (caret.selected) {
        caret_ = {n->id, 0, caret.side, true};
        return;
    }
    if (isText(*n)) {
        int offset = std::clamp(caret.offset, 0, static_cast<int>(n->text.size()));
        while (offset > 0 && offset < static_cast<int>(n->text.size()) && isContinuation(n->text[offset])) {
            --offset;
        }
        caret_ = {n->id, offset, Side::Left, false};
        return;
    }
    const std::vector<MathCaret> stops = mathStops(root_);
    const MathCaret wanted{n->id, 0, caret.side, false};
    if (std::find(stops.begin(), stops.end(), wanted) != stops.end()) {
        caret_ = wanted;
    } else {
        caret_ = caret.side == Side::Left ? leftEdge(*n) : rightEdge(*n);
    }
}

void MathEditor::select(int id, Side side)
{
    caret_ = {id, 0, side, true};
}

MathEditor::Change MathEditor::type(std::string_view ch)
{
    const bool afterColon = std::exchange(afterColon_, false);
    grown_.clear();
    if (ch.empty()) {
        return Change::None;
    }
    // Unicode spellings of the operators, as the parser accepts them.
    if (ch == "·" || ch == "×") ch = "*";
    if (ch == "÷") ch = "/";
    if (ch == "−") ch = "-";
    if (ch == "≔") ch = ":";

    if (ch.size() == 1) {
        switch (ch[0]) {
        case '+':
        case '-':
        case '*':
        case '/':
        case '^':
            return typeOperator(ch[0]);
        case '(':
            return typeOpenParen();
        case ')':
            return typeCloseParen();
        case ',':
            return typeComma();
        case '\\':
            return typeRadical();
        case ':':
            return typeStatement(':');
        case '=':
            if (afterColon) {
                return Change::None; // the "=" of ":="
            }
            return typeStatement('=');
        default:
            break;
        }
    }
    const auto first = static_cast<unsigned char>(ch[0]);
    if (isIdentChar(first) || first == '.') {
        return typeLeafCharacter(ch);
    }
    return Change::None;
}

// ---------------------------------------------------------------------------
// Operators (section 5)

bool MathEditor::fillMissingOperator(char op)
{
    if (caret_.selected) {
        return false;
    }
    // An operator placeholder just left or right of the caret takes the operator typed.
    std::vector<const MathNode*> pending;
    const auto gather = [&](const auto& self, const MathNode& n) -> void {
        if (n.kind == Kind::Binary && n.op == 0) {
            pending.push_back(&n);
        }
        for (const MathNode& c : n.children) {
            self(self, c);
        }
    };
    gather(gather, root_);
    for (const MathNode* p : pending) {
        if (rightEdge(p->children[0]) == caret_ || leftEdge(p->children[1]) == caret_) {
            MathNode* target = findMathNode(root_, p->id);
            MathNode lhs = std::move(target->children[0]);
            MathNode rhs = std::move(target->children[1]);
            *target = wrapped(op, std::move(lhs), std::move(rhs));
            caret_ = leftEdge(target->children[1]);
            return true;
        }
    }
    return false;
}

MathEditor::Change MathEditor::typeOperator(char op)
{
    if (fillMissingOperator(op)) {
        return Change::Edited;
    }

    // "2e" juxtaposes a number and e until the exponent's sign or digit arrives.
    if ((op == '-' || op == '+') && !caret_.selected) {
        int index = 0;
        const MathNode* n = findMathNode(root_, caret_.node);
        const MathNode* p = parentOf(root_, caret_.node, &index);
        if (n->kind == Kind::Identifier && (n->text == "e" || n->text == "E") && caret_.offset == 1 && p
            && p->kind == Kind::Binary && p->juxtaposed && index == 1 && p->children[0].kind == Kind::Number
            && !hasExponent(p->children[0].text)) {
            MathNode* target = findMathNode(root_, p->id);
            *target = MathNode::number(p->children[0].text + n->text + op);
            caret_ = rightEdge(*target);
            return Change::Typed;
        }
    }

    int target = caret_.node;
    Side side = caret_.side;
    if (!caret_.selected) {
        MathNode* n = findMathNode(root_, caret_.node);
        if (n->kind == Kind::Placeholder) {
            if (op == '-') {
                MathNode inner;
                const MathCaret next = leftEdge(inner);
                *n = MathNode(Kind::Negate, {std::move(inner)});
                caret_ = next;
                return Change::Edited;
            }
            side = Side::Right; // a + |■ then * gives a + ■·|■ (section 5.2)
        } else if (isText(*n)) {
            const int length = static_cast<int>(n->text.size());
            if (caret_.offset > 0 && caret_.offset < length) {
                // In the middle of a leaf: split it around the operator.
                std::vector<MathNode> lhs = lexLeaf(n->text.substr(0, caret_.offset));
                std::vector<MathNode> rhs = lexLeaf(n->text.substr(caret_.offset));
                if (lhs.size() != 1 || rhs.size() != 1) {
                    return Change::None;
                }
                const MathCaret next = leftEdge(rhs[0]);
                *n = wrapped(op, std::move(lhs[0]), std::move(rhs[0]));
                caret_ = next;
                return Change::Edited;
            }
            side = caret_.offset == 0 ? Side::Left : Side::Right;
        }
        if (side == Side::Left && op == '-') {
            // At the start of an operand, "-" negates it (section 5.2).
            int index = 0;
            const MathNode* p = parentOf(root_, n->id, &index);
            if (p && p->kind == Kind::Call && index == 0) {
                return Change::None;
            }
            MathNode operand = std::move(*n);
            const MathCaret next = leftEdge(operand);
            *n = MathNode(Kind::Negate, {std::move(operand)});
            caret_ = next;
            return Change::Edited;
        }
        // Climb by precedence, within the slot (section 5.1).
        for (;;) {
            int index = 0;
            const MathNode* p = parentOf(root_, target, &index);
            if (!p || (p->kind != Kind::Binary && p->kind != Kind::Negate)) {
                break;
            }
            const int pp = precedence(*p);
            const int po = precedence(op);
            bool climb = false;
            if (side == Side::Right) {
                climb = index == static_cast<int>(p->children.size()) - 1
                        && (pp > po || (pp == po && leftAssociative(op)));
            } else {
                climb = p->kind == Kind::Binary && index == 0 && (pp > po || (pp == po && !leftAssociative(op)));
            }
            if (!climb) {
                break;
            }
            target = p->id;
        }
    }

    MathNode* s = findMathNode(root_, target);
    int index = 0;
    const MathNode* p = parentOf(root_, target, &index);
    if (s->isStatement() || (p && p->kind == Kind::Call && index == 0)) {
        return Change::None; // a statement can't be an operand, nor can a function's name
    }
    MathNode hole;
    const int holeId = hole.id;
    MathNode operand = std::move(*s);
    *s = side == Side::Right ? wrapped(op, std::move(operand), std::move(hole))
                             : wrapped(op, std::move(hole), std::move(operand));
    caret_ = {holeId, 0, Side::Left, false};
    return Change::Edited;
}

// ---------------------------------------------------------------------------
// Letters, digits and "." (section 8.1)

MathEditor::Change MathEditor::multiplyBeside(MathNode tree, int caretNode)
{
    // Next to a node that isn't a leaf (or at the edge of one), a new operand multiplies (6.4).
    MathNode* n = findMathNode(root_, caretNode);
    int index = 0;
    const MathNode* p = parentOf(root_, caretNode, &index);
    if (n->isStatement() || (p && p->kind == Kind::Call && index == 0)) {
        return Change::None;
    }
    bool right = caret_.side == Side::Right;
    if (isText(*n)) {
        if (caret_.offset != 0 && caret_.offset != static_cast<int>(n->text.size())) {
            return Change::None;
        }
        right = caret_.offset != 0;
    }
    const MathCaret next = rightEdge(tree);
    MathNode existing = std::move(*n);
    *n = right ? MathNode::binary('*', std::move(existing), std::move(tree))
               : MathNode::binary('*', std::move(tree), std::move(existing));
    caret_ = next;
    return Change::Edited;
}

MathEditor::Change MathEditor::typeLeafCharacter(std::string_view ch)
{
    std::vector<MathNode> fresh = lexLeaf(std::string(ch));
    if (fresh.size() != 1) {
        return Change::None;
    }
    MathNode* n = findMathNode(root_, caret_.node);
    int index = 0;
    const MathNode* p = parentOf(root_, caret_.node, &index);
    const bool isName = p && p->kind == Kind::Call && index == 0;

    if (caret_.selected) {
        if (isName && fresh[0].kind != Kind::Identifier) {
            return Change::None;
        }
        const MathCaret next = rightEdge(fresh[0]);
        *n = std::move(fresh[0]);
        caret_ = next;
        return Change::Edited;
    }
    if (n->kind == Kind::Placeholder) {
        if (isName && fresh[0].kind != Kind::Identifier) {
            return Change::None;
        }
        const MathCaret next = rightEdge(fresh[0]);
        *n = std::move(fresh[0]);
        caret_ = next;
        return Change::Typed;
    }
    if (!isText(*n)) {
        return multiplyBeside(std::move(fresh[0]), n->id);
    }

    // "2e" then a digit: the e was the start of an exponent after all.
    if (isDigit(ch[0]) && n->kind == Kind::Identifier && (n->text == "e" || n->text == "E") && caret_.offset == 1
        && p && p->kind == Kind::Binary && p->juxtaposed && index == 1 && p->children[0].kind == Kind::Number
        && !hasExponent(p->children[0].text)) {
        MathNode* product = findMathNode(root_, p->id);
        *product = MathNode::number(p->children[0].text + n->text + std::string(ch));
        caret_ = rightEdge(*product);
        return Change::Typed;
    }

    std::string text = n->text;
    text.insert(static_cast<std::size_t>(caret_.offset), ch);
    const int at = caret_.offset + static_cast<int>(ch.size());
    std::vector<MathNode> tokens = lexLeaf(text);
    if (tokens.empty() || (isName && (tokens.size() != 1 || tokens[0].kind != Kind::Identifier))) {
        return Change::None;
    }
    if (tokens.size() == 1) {
        n->kind = tokens[0].kind; // keep the node, and its id, as it grows
        n->text = std::move(text);
        caret_ = {n->id, at, Side::Left, false};
        return Change::Typed;
    }
    const int split = static_cast<int>(tokens[0].text.size());
    const MathCaret next = at <= split ? MathCaret{tokens[0].id, at, Side::Left, false}
                                       : MathCaret{tokens[1].id, at - split, Side::Left, false};
    *n = MathNode::binary('*', std::move(tokens[0]), std::move(tokens[1]), true);
    caret_ = next;
    return Change::Typed;
}

// ---------------------------------------------------------------------------
// Parentheses, commas, radicals (sections 8.2, 8.3)

MathEditor::Change MathEditor::typeOpenParen()
{
    MathNode* n = findMathNode(root_, caret_.node);
    int index = 0;
    const MathNode* p = parentOf(root_, caret_.node, &index);
    const bool isName = p && p->kind == Kind::Call && index == 0;
    if (caret_.selected) {
        if (n->isStatement() || isName) {
            return Change::None;
        }
        MathNode contents = std::move(*n);
        *n = MathNode(Kind::Group, {std::move(contents)});
        caret_ = {n->id, 0, caret_.side, false};
        return Change::Edited;
    }
    if (n->kind == Kind::Placeholder) {
        if (isName) {
            return Change::None;
        }
        MathNode inner;
        const MathCaret next = leftEdge(inner);
        *n = MathNode(Kind::Group, {std::move(inner)});
        caret_ = next;
        return Change::Edited;
    }
    if (n->kind == Kind::Identifier && caret_.offset == static_cast<int>(n->text.size())) {
        if (isName) {
            return Change::None;
        }
        // Right after a name, "(" calls it.
        MathNode argument;
        const MathCaret next = leftEdge(argument);
        MathNode name = std::move(*n);
        *n = MathNode(Kind::Call, {std::move(name), std::move(argument)});
        caret_ = next;
        return Change::Edited;
    }
    MathNode inner;
    const MathCaret next = leftEdge(inner);
    MathNode group(Kind::Group, {std::move(inner)});
    if (multiplyBeside(std::move(group), n->id) == Change::None) {
        return Change::None;
    }
    caret_ = next;
    return Change::Edited;
}

MathEditor::Change MathEditor::typeCloseParen()
{
    // Out of the innermost group or call the caret is in.
    for (const MathNode* p = parentOf(root_, caret_.node); p; p = parentOf(root_, p->id)) {
        if (p->kind == Kind::Group || p->kind == Kind::Call) {
            caret_ = {p->id, 0, Side::Right, false};
            return Change::Moved;
        }
    }
    // With none, group everything in this slot up to the caret: "a+b)".
    int target = caret_.node;
    if (!caret_.selected) {
        const MathNode* n = findMathNode(root_, target);
        const bool atRight = isText(*n) ? caret_.offset == static_cast<int>(n->text.size())
                                        : n->kind != Kind::Placeholder && caret_.side == Side::Right;
        if (!atRight) {
            return Change::None;
        }
        for (;;) {
            int index = 0;
            const MathNode* p = parentOf(root_, target, &index);
            if (!p || (p->kind != Kind::Binary && p->kind != Kind::Negate)
                || index != static_cast<int>(p->children.size()) - 1) {
                break;
            }
            target = p->id;
        }
    }
    MathNode* s = findMathNode(root_, target);
    int index = 0;
    const MathNode* p = parentOf(root_, target, &index);
    if (s->isStatement() || (p && p->kind == Kind::Call && index == 0)) {
        return Change::None;
    }
    MathNode contents = std::move(*s);
    *s = MathNode(Kind::Group, {std::move(contents)});
    caret_ = {s->id, 0, Side::Right, false};
    return Change::Edited;
}

MathEditor::Change MathEditor::typeComma()
{
    // A new argument after the one the caret is in.
    for (int id = caret_.node;;) {
        int index = 0;
        MathNode* p = parentOf(root_, id, &index);
        if (!p) {
            return Change::None;
        }
        if (p->kind == Kind::Call && index >= 1) {
            MathNode argument;
            const MathCaret next = leftEdge(argument);
            p->children.insert(p->children.begin() + index + 1, std::move(argument));
            caret_ = next;
            return Change::Edited;
        }
        id = p->id;
    }
}

MathEditor::Change MathEditor::typeRadical()
{
    MathNode* n = findMathNode(root_, caret_.node);
    int index = 0;
    const MathNode* p = parentOf(root_, caret_.node, &index);
    const bool isName = p && p->kind == Kind::Call && index == 0;
    if (caret_.selected) {
        if (n->isStatement() || isName) {
            return Change::None;
        }
        MathNode radicand = std::move(*n);
        *n = MathNode(Kind::Radical, {std::move(radicand)});
        caret_ = {n->id, 0, Side::Right, false};
        return Change::Edited;
    }
    MathNode inner;
    const MathCaret next = leftEdge(inner);
    if (n->kind == Kind::Placeholder) {
        if (isName) {
            return Change::None;
        }
        *n = MathNode(Kind::Radical, {std::move(inner)});
        caret_ = next;
        return Change::Edited;
    }
    if (multiplyBeside(MathNode(Kind::Radical, {std::move(inner)}), n->id) == Change::None) {
        return Change::None;
    }
    caret_ = next;
    return Change::Edited;
}

MathEditor::Change MathEditor::typeStatement(char op)
{
    // ":" and "=" act on the whole region, wherever the caret is (section 8.4).
    if (root_.isStatement()) {
        return Change::None;
    }
    MathNode hole;
    const MathCaret next = leftEdge(hole);
    MathNode expression = std::move(root_);
    root_ = MathNode(op == ':' ? Kind::Definition : Kind::Evaluation, {std::move(expression), std::move(hole)});
    caret_ = next;
    afterColon_ = op == ':';
    return Change::Edited;
}

// ---------------------------------------------------------------------------
// Deleting (section 9)

bool MathEditor::removePlaceholder(int id, bool before)
{
    int index = 0;
    MathNode* p = parentOf(root_, id, &index);
    if (!p) {
        return false;
    }
    // Keeps child `keep` of p in p's place, with the caret on the side the placeholder was.
    const auto collapse = [&](int keep, bool caretRight) {
        MathNode kept = std::move(p->children[keep]);
        const MathCaret next = caretRight ? rightEdge(kept) : leftEdge(kept);
        *p = std::move(kept);
        caret_ = next;
    };
    const int last = static_cast<int>(p->children.size()) - 1;
    if (before) {
        // An operator, or a box's opening, is on the placeholder's left.
        switch (p->kind) {
        case Kind::Binary:
        case Kind::Fraction:
        case Kind::Power:
        case Kind::Definition:
        case Kind::Evaluation:
            if (index == 1) {
                collapse(0, true);
                return true;
            }
            return false;
        case Kind::Negate:
        case Kind::Radical:
        case Kind::Group:
            collapse(0, false);
            return true;
        case Kind::Call:
            if (index == 0) {
                return false;
            }
            if (last == 1) {
                collapse(0, true); // f(■) goes back to f
            } else {
                p->children.erase(p->children.begin() + index);
                caret_ = index > 1 ? rightEdge(p->children[index - 1]) : leftEdge(p->children[index]);
            }
            return true;
        default:
            return false;
        }
    }
    // Delete: an operator is on the placeholder's right.
    switch (p->kind) {
    case Kind::Binary:
    case Kind::Fraction:
    case Kind::Power:
    case Kind::Definition:
        if (index == 0) {
            collapse(1, false);
            return true;
        }
        return false;
    case Kind::Evaluation:
        if (index == 0 && p->children[1].kind == Kind::Placeholder) {
            collapse(1, false);
            return true;
        }
        return false;
    case Kind::Call:
        if (index >= 1 && index < last) {
            p->children.erase(p->children.begin() + index);
            caret_ = leftEdge(p->children[index]);
            return true;
        }
        return false;
    default:
        return false;
    }
}

MathEditor::Change MathEditor::eraseAtLeftEdge(int id)
{
    // Backspace with the caret at the left edge of node `id`: what is to the left?
    for (;;) {
        int index = 0;
        MathNode* p = parentOf(root_, id, &index);
        if (!p) {
            return Change::None; // the start of the region
        }
        switch (p->kind) {
        case Kind::Binary:
            if (index == 0) {
                id = p->id;
                continue;
            }
            if (p->op != 0) {
                p->op = 0; // a + |b → a □ |b (rule 4)
                p->juxtaposed = false;
                caret_ = leftEdge(p->children[1]);
                return Change::Edited;
            }
            if (p->children[0].kind == Kind::Placeholder || p->children[1].kind == Kind::Placeholder) {
                const bool keepRhs = p->children[0].kind == Kind::Placeholder;
                MathNode kept = std::move(p->children[keepRhs ? 1 : 0]);
                const MathCaret next = keepRhs ? leftEdge(kept) : rightEdge(kept);
                *p = std::move(kept);
                caret_ = next;
                return Change::Edited;
            }
            return Change::None;
        case Kind::Negate:
        case Kind::Group:
        case Kind::Radical: {
            // Remove the minus sign, or unwrap the box (rule 5).
            MathNode kept = std::move(p->children[0]);
            const MathCaret next = leftEdge(kept);
            *p = std::move(kept);
            caret_ = next;
            return Change::Edited;
        }
        case Kind::Power:
        case Kind::Call:
            if (index == 0) {
                id = p->id; // the base and the name start where the box does
                continue;
            }
            select(p->id, Side::Right);
            return Change::Moved;
        case Kind::Fraction:
            select(p->id, Side::Right);
            return Change::Moved;
        case Kind::Definition:
        case Kind::Evaluation:
            if (index == 0) {
                return Change::None;
            }
            select(p->children[1].id, Side::Right); // rule 6; a placeholder was handled already
            return Change::Moved;
        default:
            return Change::None;
        }
    }
}

MathEditor::Change MathEditor::eraseAtRightEdge(int id)
{
    // Delete with the caret at the right edge of node `id`: what is to the right?
    for (;;) {
        int index = 0;
        MathNode* p = parentOf(root_, id, &index);
        if (!p) {
            return Change::None; // the end of the region
        }
        switch (p->kind) {
        case Kind::Binary:
            if (index == 1) {
                id = p->id;
                continue;
            }
            if (p->op != 0) {
                p->op = 0;
                p->juxtaposed = false;
                caret_ = rightEdge(p->children[0]);
                return Change::Edited;
            }
            if (p->children[0].kind == Kind::Placeholder || p->children[1].kind == Kind::Placeholder) {
                const bool keepRhs = p->children[0].kind == Kind::Placeholder;
                MathNode kept = std::move(p->children[keepRhs ? 1 : 0]);
                const MathCaret next = keepRhs ? leftEdge(kept) : rightEdge(kept);
                *p = std::move(kept);
                caret_ = next;
                return Change::Edited;
            }
            return Change::None;
        case Kind::Negate:
            id = p->id;
            continue;
        case Kind::Group:
        case Kind::Radical: {
            MathNode kept = std::move(p->children[0]);
            const MathCaret next = rightEdge(kept);
            *p = std::move(kept);
            caret_ = next;
            return Change::Edited;
        }
        case Kind::Fraction:
        case Kind::Power:
        case Kind::Call:
            select(p->id, Side::Left);
            return Change::Moved;
        case Kind::Definition:
        case Kind::Evaluation: {
            if (index == 1) {
                return Change::None;
            }
            if (p->children[1].kind == Kind::Placeholder) {
                MathNode kept = std::move(root_.children[0]);
                root_ = std::move(kept);
                caret_ = rightEdge(root_);
                return Change::Edited;
            }
            select(p->children[1].id, Side::Left);
            return Change::Moved;
        }
        default:
            return Change::None;
        }
    }
}

MathEditor::Change MathEditor::erase(bool before)
{
    afterColon_ = false;
    grown_.clear();
    MathNode* n = findMathNode(root_, caret_.node);

    if (caret_.selected) {
        MathNode hole;
        const MathCaret next = leftEdge(hole);
        *n = std::move(hole);
        caret_ = next;
        return Change::Edited;
    }

    if (isText(*n)) {
        const int length = static_cast<int>(n->text.size());
        int from = caret_.offset;
        int to = caret_.offset;
        if (before && from > 0) {
            do {
                --from;
            } while (from > 0 && isContinuation(n->text[from]));
        } else if (!before && to < length) {
            do {
                ++to;
            } while (to < length && isContinuation(n->text[to]));
        }
        if (from == to) {
            return before ? eraseAtLeftEdge(n->id) : eraseAtRightEdge(n->id);
        }
        std::string text = n->text;
        text.erase(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from));
        int index = 0;
        MathNode* p = parentOf(root_, n->id, &index);
        if (text.empty() && p && p->kind == Kind::Binary && p->juxtaposed) {
            // Juxtaposition has no operator to leave behind: "10 N" goes back to "10".
            MathNode kept = std::move(p->children[1 - index]);
            const MathCaret next = index == 1 ? rightEdge(kept) : leftEdge(kept);
            *p = std::move(kept);
            caret_ = next;
            return Change::Erased;
        }
        if (text.empty()) {
            MathNode hole;
            caret_ = leftEdge(hole);
            *n = std::move(hole);
            return Change::Erased;
        }
        const bool isName = p && p->kind == Kind::Call && index == 0;
        std::vector<MathNode> tokens = lexLeaf(text);
        if (tokens.size() == 2 && !isName) {
            const int split = static_cast<int>(tokens[0].text.size());
            const MathCaret next = from <= split ? MathCaret{tokens[0].id, from, Side::Left, false}
                                                 : MathCaret{tokens[1].id, from - split, Side::Left, false};
            *n = MathNode::binary('*', std::move(tokens[0]), std::move(tokens[1]), true);
            caret_ = next;
            return Change::Erased;
        }
        if (tokens.size() == 1 && !isName) {
            n->kind = tokens[0].kind;
        }
        n->text = std::move(text);
        caret_ = {n->id, from, Side::Left, false};
        return Change::Erased;
    }

    if (n->kind == Kind::Placeholder) {
        if (n->id == root_.id) {
            return Change::None;
        }
        if (removePlaceholder(n->id, before)) {
            return Change::Edited;
        }
        return before ? eraseAtLeftEdge(n->id) : eraseAtRightEdge(n->id);
    }

    // Beside a box or a minus sign.
    if (before && caret_.side == Side::Right) {
        select(n->id, Side::Right); // a box: select it first (rule 5)
        return Change::Moved;
    }
    if (!before && caret_.side == Side::Left) {
        if (n->kind == Kind::Negate) {
            MathNode kept = std::move(n->children[0]);
            const MathCaret next = leftEdge(kept);
            *n = std::move(kept);
            caret_ = next;
            return Change::Edited;
        }
        select(n->id, Side::Left);
        return Change::Moved;
    }
    return before ? eraseAtLeftEdge(n->id) : eraseAtRightEdge(n->id);
}

// ---------------------------------------------------------------------------
// Moving and selecting (sections 6, 7)

MathEditor::Change MathEditor::step(bool forward)
{
    afterColon_ = false;
    grown_.clear();
    if (caret_.selected) {
        const MathNode* s = findMathNode(root_, caret_.node);
        caret_ = forward ? rightEdge(*s) : leftEdge(*s);
        return Change::Moved;
    }
    const std::vector<MathCaret> stops = mathStops(root_);
    const auto it = std::find(stops.begin(), stops.end(), caret_);
    if (it == stops.end()) {
        caret_ = stops.back();
        return Change::Moved;
    }
    if (forward ? it + 1 == stops.end() : it == stops.begin()) {
        return Change::None;
    }
    caret_ = forward ? *(it + 1) : *(it - 1);
    return Change::Moved;
}

MathEditor::Change MathEditor::home()
{
    setCaret(leftEdge(root_));
    return Change::Moved;
}

MathEditor::Change MathEditor::end()
{
    setCaret(rightEdge(root_));
    return Change::Moved;
}

MathEditor::Change MathEditor::tab(bool back)
{
    afterColon_ = false;
    grown_.clear();
    std::vector<MathCaret> holes;
    for (const MathCaret& stop : mathStops(root_)) {
        if (findMathNode(root_, stop.node)->kind == Kind::Placeholder) {
            holes.push_back(stop);
        }
    }
    if (holes.empty()) {
        return Change::None;
    }
    // The first placeholder after (or before) the caret, in reading order.
    const std::vector<MathCaret> stops = mathStops(root_);
    const MathNode* here = findMathNode(root_, caret_.node);
    const MathCaret from = caret_.selected ? (back ? leftEdge(*here) : rightEdge(*here)) : caret_;
    const auto position = [&](const MathCaret& c) { return std::find(stops.begin(), stops.end(), c) - stops.begin(); };
    const auto at = position(from);
    if (back) {
        for (auto it = holes.rbegin(); it != holes.rend(); ++it) {
            if (position(*it) < at) {
                caret_ = *it;
                return Change::Moved;
            }
        }
        caret_ = holes.back();
    } else {
        for (const MathCaret& hole : holes) {
            if (position(hole) > at) {
                caret_ = hole;
                return Change::Moved;
            }
        }
        caret_ = holes.front();
    }
    return Change::Moved;
}

MathEditor::Change MathEditor::grow()
{
    afterColon_ = false;
    if (!caret_.selected) {
        const MathNode* n = findMathNode(root_, caret_.node);
        Side side = caret_.side;
        if (isText(*n)) {
            side = caret_.offset == 0 ? Side::Left : Side::Right;
        } else if (n->kind == Kind::Placeholder) {
            side = Side::Right;
        }
        grown_.push_back(caret_);
        select(n->id, side);
        return Change::Moved;
    }
    const MathNode* p = parentOf(root_, caret_.node);
    if (!p) {
        // From the whole region, back to where growing started.
        const MathNode* s = findMathNode(root_, caret_.node);
        caret_ = grown_.empty() ? (caret_.side == Side::Left ? leftEdge(*s) : rightEdge(*s)) : grown_.front();
        grown_.clear();
        return Change::Moved;
    }
    grown_.push_back(caret_);
    caret_.node = p->id;
    return Change::Moved;
}

MathEditor::Change MathEditor::shrink()
{
    afterColon_ = false;
    if (grown_.empty()) {
        return Change::None;
    }
    caret_ = grown_.back();
    grown_.pop_back();
    return Change::Moved;
}

MathEditor::Change MathEditor::flip()
{
    afterColon_ = false;
    grown_.clear();
    const MathNode* n = findMathNode(root_, caret_.node);
    if (caret_.selected) {
        caret_.side = caret_.side == Side::Left ? Side::Right : Side::Left;
        return Change::Moved;
    }
    if (isText(*n)) {
        const int length = static_cast<int>(n->text.size());
        caret_.offset = caret_.offset == length ? 0 : length;
        return Change::Moved;
    }
    if (n->kind == Kind::Placeholder) {
        return Change::None;
    }
    caret_ = caret_.side == Side::Left ? rightEdge(*n) : leftEdge(*n);
    return Change::Moved;
}

// ---------------------------------------------------------------------------
// Clipboard (section 10)

std::optional<std::string> MathEditor::copy() const
{
    const MathNode& n = caret_.selected ? *findMathNode(root_, caret_.node) : root_;
    // An evaluation's unit may be left empty; any other placeholder can't be written.
    const MathNode* hole = firstPlaceholder(n);
    if (n.kind == Kind::Evaluation && n.children[1].kind == Kind::Placeholder) {
        hole = firstPlaceholder(n.children[0]);
    }
    if (hole) {
        return std::nullopt;
    }
    return toLinear(n);
}

MathEditor::Change MathEditor::paste(MathNode tree)
{
    afterColon_ = false;
    grown_.clear();
    MathNode* n = findMathNode(root_, caret_.node);
    int index = 0;
    const MathNode* p = parentOf(root_, caret_.node, &index);
    const bool isName = p && p->kind == Kind::Call && index == 0;
    const bool replaces = caret_.selected || n->kind == Kind::Placeholder;
    if (tree.isStatement() && !(replaces && n->id == root_.id)) {
        return Change::None; // a definition or evaluation can only be a whole region
    }
    if (isName && (!replaces || tree.kind != Kind::Identifier)) {
        return Change::None;
    }
    if (replaces) {
        const MathCaret next = rightEdge(tree);
        *n = std::move(tree);
        caret_ = next;
        return Change::Edited;
    }
    return multiplyBeside(std::move(tree), n->id);
}

} // namespace lemma
