#pragma once

#include "lemma/math.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lemma {

// Where the caret is in a math region's tree (docs/editor-spec.md, section 3): an
// insertion point, drawn as a bar, or a selection of a whole subtree, drawn underlined.
struct MathCaret {
    enum class Side { Left, Right };

    int node = 0;
    int offset = 0;         // an insertion point in a Number or Identifier: byte offset into its text
    Side side = Side::Left; // an insertion point beside any other node, or the bar of a selection
    bool selected = false;  // a selection of `node`'s subtree

    friend bool operator==(const MathCaret&, const MathCaret&) = default;
};

// Every insertion point in a tree, in reading order: the stops Left and Right step through
// (section 6.1).
std::vector<MathCaret> mathStops(const MathNode& root);

// Edits one math region's tree, following docs/editor-spec.md. Every command leaves a
// valid tree and a caret on one of its stops (or a selection).
class MathEditor {
public:
    // What a command did, for the undo stack: characters typed into, or erased from, one
    // leaf merge into one step; every other edit is a step of its own.
    enum class Change { None, Moved, Typed, Erased, Edited };

    explicit MathEditor(MathNode root = {});
    MathEditor(MathNode root, const MathCaret& caret);

    const MathNode& root() const { return root_; }
    const MathCaret& caret() const { return caret_; }
    // Puts the caret on the nearest stop to `caret`, e.g. after a click.
    void setCaret(const MathCaret& caret);

    // One typed character, UTF-8. Space is not typed: it is grow().
    Change type(std::string_view character);
    Change backspace() { return erase(true); }
    Change deleteForward() { return erase(false); }

    Change left() { return step(false); }
    Change right() { return step(true); }
    Change home();
    Change end();
    // To the next (or previous) placeholder, wrapping around.
    Change tab(bool back);

    Change grow();   // Space
    Change shrink(); // Shift+Space
    Change flip();   // Insert

    // Linear text of the selection, or of the whole region with no selection; nothing if
    // it isn't finished (a placeholder, or a number like "2e-"), since it wouldn't paste.
    std::optional<std::string> copy() const;
    // Pastes a tree, e.g. from mathFromLinear(): it replaces the selection or a
    // placeholder, and otherwise multiplies next to the caret.
    Change paste(MathNode tree);

private:
    Change typeOperator(char op);
    Change typeLeafCharacter(std::string_view ch);
    Change typeOpenParen();
    Change typeCloseParen();
    Change typeComma();
    Change typeRadical();
    Change typeStatement(char op);
    Change typeSymbolic();  // Ctrl+., or "→"
    Change typeEquation();  // Ctrl+=, or "⩵"
    Change typeSemicolon(); // another keyword
    Change erase(bool before);
    Change eraseAtLeftEdge(int id);
    Change eraseAtRightEdge(int id);
    bool removePlaceholder(int id, bool before);
    bool fillMissingOperator(char op);
    Change step(bool forward);

    // Replaces the node at the caret, or the selection, with `tree` next to what's there.
    Change multiplyBeside(MathNode tree, int caretNode);
    void select(int id, MathCaret::Side side);

    MathNode root_;
    MathCaret caret_;
    std::vector<MathCaret> grown_; // where each Space grew from, for Shift+Space
    bool afterColon_ = false;      // so that "=" right after ":" is swallowed and ":=" works
};

} // namespace lemma
