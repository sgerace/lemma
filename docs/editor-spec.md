# 2D math editor: editing model

Status: **agreed in review** (issue #7; see section 12) and implemented:
`src/core/include/lemma/math.h` (the tree), `math_edit.h` (the commands) and
`math_layout.h` (box layout). Section 13 records where the implementation settles
things this spec left open. Change this document along with the editor.

This spec defines how a math region is edited once it is a 2D, Mathcad-style
expression instead of a line of text: what the editor's tree holds, where the caret
can be, what the selection is, and what every key does. Layout, fonts and painting
are covered only as far as editing depends on them.

## 1. Goals and non-goals

Goals:

- Edit the expression tree directly. The selection is always a whole subtree.
- Typing a formula the way you'd write it on one line (`a*b+c`, `9.81 m/s^2`) gives
  the expected tree, without having to think about the tree.
- When the user wants different grouping, they select a subtree (Space grows it) and
  the next operator wraps exactly that.
- Every state the editor can reach is a valid tree. Incomplete input shows
  placeholders, never a parse error.
- The tree is the source of truth: it is what `.lemma` files store and what the
  engine evaluates (section 11). The linear syntax is only a way in and out, for paste
  and copy, so it never has to reproduce a tree exactly.

Not in the first version (each can be added later without changing the model):
matrices, literal subscripts (Mathcad's `x.1`), absolute value bars, n-th roots,
sums and products (derivatives and integrals came with #41), Greek-letter shortcuts,
`=` after a definition (`x := 2 m = 2 m`), and 2D editing of plot placeholders (they
stay linear text).

## 2. The editor tree

The editor works on its own tree (in `lemma_core`, no Qt), separate from
`lemma::Node`. It differs from the AST in three ways: it can hold placeholders, it
keeps parentheses the user typed, and it keeps the statement structure (`:=`, `=`) as
nodes. It is saved as is, and lowered to `lemma::Node` for evaluation (section 11).

The linear form column is what Copy writes (section 11.3).

| Node | Children (slots) | Shown as | Linear form |
|---|---|---|---|
| `Number` | text, e.g. `9.81`, `1e-3` | the text | the text |
| `Identifier` | text, e.g. `L`, `x1`, `kN` | the text, italic for variables | the text |
| `Placeholder` | none | `■` (small filled square) | none: not copyable |
| `Binary` | lhs, rhs; op is `+`, `−`, `·`, juxtaposition, or an operator placeholder | `a + b`, `a − b`, `a·b`, `2 m`, `a □ b` | `a + b`, `a - b`, `a*b`, `2 m` |
| `Negate` | operand | `−a` | `-a` |
| `Fraction` | numerator, denominator | stacked, with a rule | `num/den` |
| `Power` | base, exponent | exponent raised and smaller | `base^exp` |
| `Radical` | radicand | `√` with an overbar | `sqrt(x)` |
| `Group` | contents | `( … )` | `( … )` |
| `Call` | name (an identifier), arguments 1..n | `f(a, b)` | `f(a, b)` |
| `Derivative` | variable (a name), the order of an n-th derivative, operand | `d/dx f`, `dⁿ/dxⁿ f` | `diff(f, x)`, `diff(f, x, n)` |
| `Integral` | the lower and upper limits of a definite integral, integrand, variable (a name) | `∫ f dx`, `∫ₐᵇ f dx` | `integrate(f, x)`, `integrate(f, x, a, b)` |
| `Definition` | lhs, rhs | `lhs := rhs` | `lhs := rhs` |
| `Evaluation` | expression, unit (may be a placeholder) | `expr = result unit` | `expr =` or `expr = unit` |
| `Symbolic` | expression, keyword slots 1..n | `expr → result`, keywords above the arrow | `expr ->` or `expr -> keyword; keyword` |
| `Keyword` | name, arguments 1..n | `solve, σ` | `solve, σ` |
| `Equation` | lhs, rhs | `lhs = rhs`, with a bold `=` | `lhs == rhs` |

`Symbolic`, `Keyword` and `Equation` belong to symbolic evaluation, and their editing
is in `docs/symbolic-spec.md`, section 6.

Terms used below:

- A **leaf** is a `Number`, `Identifier` or `Placeholder`.
- A **box** is a node that typesets its children in their own slots: `Fraction`,
  `Power`, `Radical`, `Group`, `Call`, `Derivative`, `Integral`. Each child of a box
  is the root of a **slot**.
- The **statement** is the root: a `Definition`, an `Evaluation`, a `Symbolic`, or a
  bare expression. Its children are slots too.
- An **operator chain** is the part of a slot made of `Binary` and `Negate` nodes. It
  lays out on one line; boxes interrupt it.

Juxtaposition is a `Binary` multiplication whose style is "juxtaposed". It is created
by typing a letter right after a number (`2m`) and by pasting the linear form `10 N`,
and it is shown as a thin space. It evaluates exactly like `·`. Two letters in a row
are always one name: `LF` is the variable `LF`, and `L·F` needs `*`.

**Implied parentheses.** Parentheses that the tree's precedence requires are drawn
by layout and are not nodes. `·` over `a` and `b + c` draws as `a·(b + c)`; the
parentheses belong to the `b + c` box and are not selectable on their own. A `Group`
is only created when the user types `(` (section 8.2). A fraction's numerator never
needs implied parentheses, since the rule groups it.

**Precedence**, loosest to tightest, matching the parser: `:=` and `=` (statement
only); `+ −`; `· /` and juxtaposition; unary `−`; `^` (right associative). `Fraction`
counts as `/` when deciding implied parentheses around it (for example `−[a/b]` needs
none, `[a/b]^2` does). `Derivative` and `Integral` count as `·`: `2 ∫ f dx` and
`−d/dx f` need none, `(d/dx f)²` does. A derivative's operand is drawn in implied
parentheses when it is a sum, `d/dx (x² + 1)`, as for `·`; an integrand never is,
since `∫` and `dx` already enclose it.

## 3. Caret and selection

The editing state is exactly one of:

- An **insertion point**: a position between things, drawn as a vertical bar. It is
  either inside a leaf's text at a character offset (`(leaf, offset)`), or at one side
  of a non-leaf node (`(node, Left | Right)`). An insertion point on a placeholder is
  `(placeholder, Left)`; it is drawn as the bar in front of the `■`.
- A **selection**: a subtree `S` plus a side, `Left` or `Right`. Drawn as an
  underline under `S`'s box with the vertical bar at that side, as in Mathcad.

Several insertion points are visually the same spot: after `b` in `a + b` is both
`(b, 1)` and `(Binary, Right)`. Each position has one canonical form so that each spot
is one stop for the arrow keys: **the deepest leaf offset wins**, and otherwise the
deepest node. So after `b` the caret is `(b, 1)`, and after a fraction it is
`(Fraction, Right)`.

The difference that matters for typing:

- With an **insertion point**, an operator applies to the operand that precedence
  gives, the way linear typing would (section 5.1). It never leaves the current slot.
- With a **selection**, an operator applies to exactly the selected subtree.

The underline is shown if and only if the next operator wraps exactly what is
underlined. The user never has to guess.

## 4. Notation for examples

- `|` is an insertion point.
- `⟨…⟩|` is a selection with the bar on the right, `|⟨…⟩` on the left.
- `■` is a placeholder and `□` an operator placeholder.
- `[n / d]` is a stacked fraction, `b^[e]` a power with its exponent slot,
  `√[x]` a radical, `f(a, b)` a call, `d/dx f` and `d/dx^[n] f` a derivative (the
  order written once, on `x`), and `∫ f dx` and `∫_[a]^[b] f dx` an integral. Plain `( )` is a `Group` or implied parentheses,
  as noted.
- `→` separates states; each step names the key.

## 5. Operators

### 5.1 Which operand an operator takes

When the user types a binary operator `op` (`+ - * / ^`), the editor first finds the
**operand** `S`:

- With a selection, `S` is the selection.
- With an insertion point, start from the node at the caret (the leaf it's in, or the
  node it's beside) and climb toward the root while the parent is in the **same
  slot** and binds at least as tightly as `op`. Left-associative operators climb over
  an equal-precedence parent; `^` does not. Climbing stops at a slot root: it never
  leaves a numerator, denominator, exponent, radicand, group or argument.

An insertion point in the middle of a leaf (`ab|c`) first splits the leaf: `ab` and
`c` joined by the new operator, so `ab|c` then `+` gives `ab + |c`.

### 5.2 Wrapping

With the bar (insertion point side, or selection side) on the **right** of `S`, `S`
becomes the operator's first operand; on the **left**, its last operand. The other
operand is a new placeholder, and the caret moves to it.

| Key | Bar right of `S` | Bar left of `S` |
|---|---|---|
| `+` | `S + |■` | `|■ + S` |
| `-` | `S − |■` | `|■ − S` |
| `*` | `S·|■` | `|■·S` |
| `/` | `[S / |■]` | `[|■ / S]` |
| `^` | `S^[|■]` | `|■^[S]` |

`-` on a placeholder, at the start of a slot, or right after another operator, creates
`Negate` instead: `|■` then `-` gives `−|■`.

If `S` is a placeholder (`a + |■` then `*`), the operator still wraps it:
`a + ■·|■`. This is rare and harmless; Backspace removes it (section 9).

### 5.3 Worked examples

Typing linear input, starting from an empty region (`|■`):

| Keys | Result |
|---|---|
| `a + b * c` | `a + b·c|` (the `*` climbs from `b` but stops below `+`) |
| `a * b + c` | `a·b + c|` (the `+` climbs over `a·b`) |
| `a - b - c` | `(a − b) − c|` as a tree, drawn `a − b − c` |
| `2 ^ 3 ^ 2` | `2^[3^[2|]]` (the second `^` is typed in the first exponent, matching `^`'s right associativity) |
| `9 . 8 1 m / s ^ 2` | `[9.81 m / s^[2|]]` (`m` after a number juxtaposes; `/` climbs over `9.81 m`) |
| `a + b / c` | `a + [b / c|]` |

Typing stays in a slot until the user leaves it. This is the main difference from
linear typing, and it is Mathcad's behavior:

| Keys | Result | Why |
|---|---|---|
| `x ^ 2 + 1` | `x^[2 + 1|]` | `+` doesn't climb out of the exponent |
| `x ^ 2 →  + 1` | `x^[2] + 1|` | Right arrow leaves the exponent (6.1) |
| `x ^ 2 Space Space + 1` | `x^[2] + 1|` | Space selects `2`, then `x²`; `+` wraps it |
| `a / b * c` | `[a / b·c|]` | `*` stays in the denominator |
| `a / b Space Space * c` | `[a / b]·c|` | |

Overriding precedence with a selection:

| State | Keys | Result |
|---|---|---|
| `a + b|` | `Space Space * c` | `(a + b)·c|` with implied parentheses |
| `a + b|` | `Space Space / c` | `[a + b / c|]` (the rule groups `a + b`) |
| `a·b|` | `Space + c` | `a·(b + c|)` (`Space` selected just `b`) |
| `|⟨x⟩` (bar left) | `^ e` | `e|^[x]` |

## 6. Moving

### 6.1 Left and Right

Left and Right step through the **stops** of the region in reading order, one at a
time: each character boundary in a leaf, and the sides of boxes. Going right through a
box enters its first slot, and leaving the last slot lands right after the box:

- `Fraction`: numerator, then denominator.
- `Power`: base, then exponent.
- `Radical`, `Group`: contents.
- `Call`: the name, then each argument.
- `Derivative`: the variable, its order (if any), then the operand.
- `Integral`: the lower and upper limits (if any), the integrand, then the variable.
- `Definition`, `Evaluation`: lhs, then rhs or unit. The result of an evaluation is not
  a stop.

Example, pressing Right from the start of `[a / b]·c`:
`|[a / b]·c` → `[|a / b]·c` → `[a| / b]·c` → `[a / |b]·c` → `[a / b|]·c` →
`[a / b]|·c` → `[a / b]·|c` → `[a / b]·c|`.

Note that `[a / b]|·c` and `[a / b]·|c` are distinct stops: the first is
`(Fraction, Right)`, the second `(c, 0)`. Typing `x` at the first inserts a
multiplication (6.4); at the second it extends the identifier (`xc`).

With a selection, Left and Right collapse it to an insertion point at that side of `S`
(no movement), like a text editor.

At the first or last stop, Left and Right do nothing. (They don't leave the region:
only Enter, Escape and clicking elsewhere do, as today.)

### 6.2 Up and Down

Up and Down move between slots stacked vertically: numerator ↔ denominator, base
↔ exponent (Up from the base goes to the exponent), a derivative's variable ↔ its
order, and an integral's lower ↔ upper limit. The caret goes to the stop nearest
the caret's current x position. With no slot above or below, they do nothing.

### 6.3 Home and End

Home and End go to the first and last stop of the region.

### 6.4 Typing next to a box

A letter or digit typed at an insertion point beside a non-leaf node (not inside a
leaf) inserts an operand next to it, joined by multiplication: `[a / b]|` then `x`
gives `[a / b]·x|`. A digit after a box gives the same, `(a + b)·2`.

### 6.5 Tab

Tab and Shift+Tab move to the next or previous placeholder in the region, wrapping
around. With no placeholders, they do nothing. (Plot regions keep their own Tab.)

## 7. Space: growing the selection

Space grows the selection one visible step toward the root, keeping the side of the
bar:

1. From an insertion point, it selects the node at the caret: the leaf it's in, or the
   node it's beside. `a + b|` → `a + ⟨b⟩|`.
2. From a selection `S`, it selects `S`'s parent. Because chains are left
   associative, growing from `c` in `a + b + c` goes straight to the whole
   expression, while from `b` it goes `b` → `a + b` → `a + b + c`. A call's name
   grows to the whole call.
3. From the statement root, it goes back to the insertion point it started from.

Shift+Space shrinks one step back down the path Space took. It does nothing if the
selection wasn't made by Space.

Insert flips the bar to the other side of the selection or insertion point's node
(Mathcad's Insert key), without changing what is selected.

Mouse:

- Click: the insertion point at the stop nearest the click, found by hit-testing the
  box tree (the deepest box containing the point, then the nearest stop by x).
- Drag: selects the smallest subtree containing both ends (their lowest common
  ancestor), with the bar at the end where the drag stopped.
- Double-click: selects the leaf or box under the pointer.

## 8. Typing other characters

### 8.1 Letters, digits, `.`

- In a leaf, characters insert at the offset, with these rules:
  - A digit or `.` in a number extends the number. `e` or `E` in a number is an
    exponent only if a digit follows, as in the parser; otherwise it starts an
    identifier joined by juxtaposition (`2e` stays `2 e` until a digit makes it `2e3`).
  - A letter right after a number's last character starts a new identifier, joined by
    juxtaposition: `10|` then `N` gives `10 N|`.
  - Letters, digits and `_` in an identifier extend it (`x1`).
- On a placeholder, the character replaces it with a new leaf.
- Beside a non-leaf node: see 6.4.
- With a selection, the character replaces the whole selection with a new leaf, as
  typing over a selection does in a text editor.

### 8.2 Parentheses and commas

- `(` at an insertion point:
  - right after an identifier's last character, makes a call: `f|` → `f(|■)`;
  - on a placeholder, makes a group: `|■` → `(|■)`;
  - right after a number or a box, multiplies by a new group (6.4):
    `2|` → `2·(|■)`, `[a / b]|` → `[a / b]·(|■)`.
- `(` with a selection wraps it in a group: `⟨b + c⟩|` → `(b + c)|`. (Mathcad uses `'`
  for this. Lemma uses `(`, since the linear syntax already does.)
- `)` moves the insertion point to just after the innermost enclosing `Group` or
  `Call`: `(a + b|)` → `(a + b)|`. If there is none, it wraps the largest subtree in the
  current slot whose right edge is the caret: `a·b + c|` then `)` gives
  `(a·b + c)|`, and with the caret after `b`, `(a·b)| + c`. This lets linear typing
  like `a+b)` work.
- `,` inside a call's argument adds a new argument after it: `f(a|)` → `f(a, |■)`.
  Elsewhere it is ignored.

### 8.3 Radical

`\` (Mathcad's key) on a placeholder gives `√[|■]`. With a selection, it wraps it:
`⟨x + 1⟩` → `√[x + 1]|`. Next to existing content it multiplies by a new radical
(6.4): `2|` → `2·√[|■]`.

### 8.4 Definition and evaluation

These act on the whole statement, wherever the caret is, because they are statement
structure.

- `:` turns a bare expression into `Definition(expression, ■)`, with the caret on the
  rhs placeholder. A `=` typed right after it is swallowed, so typing `:=` also works.
  The lhs must be a name (`x`) or a call whose arguments are all names (`f(x, y)`);
  any other lhs is kept and shown with the parser's error ("Only a name or a function
  can be defined with :="), so it can be fixed in place.
- `=` turns a bare expression into `Evaluation(expression, ■)`. The result is shown
  after `=`, and the unit placeholder after it, while the region is being edited. The
  caret moves to the unit placeholder. An empty unit placeholder is not an error and
  isn't shown once editing ends.
- `:` or `=` on a statement that is already a definition or evaluation is ignored.

Spaces typed as text are not needed anywhere: Space is the grow key (7).

### 8.5 Derivatives and integrals

The keys are Mathcad's. Each acts like `\` (8.3): on a placeholder it gives the empty
operator, with a selection it takes the selection as its operand or integrand, and
next to existing content it multiplies by a new one (6.4).

| Key | On `|■` | On `⟨y(x)⟩|` |
|---|---|---|
| `?` | `d/d|■ ■` | `d/d|■ y(x)` |
| `&` | `∫_[■]^[■] |■ d■` (definite) | `∫_[|■]^[■] y(x) d■` |
| Ctrl+I, or typing `∫` | `∫ |■ d■` (indefinite) | `∫ y(x) d|■` |

- The caret goes to the first empty slot in the order of 6.1, except that `&` on a
  placeholder starts at the integrand, which is usually written first.
- The variable slot is a name slot, like a function's name: only a name goes there,
  and operators can't be typed in it.
- `^` at the end of a derivative's variable adds the order, or goes to the one it
  has: `d/dx| ■` then `^2` gives `d/dx^[2|] ■`, shown `d²/dx²`. The order is one slot,
  on `x`, and drawn on `d` too, as in Mathcad. It takes a whole number from 1 to 99;
  anything else is an error on it.
- A call typed by name, `diff(y(x), x)` or `integrate(f, x, 0, L)`, stays a call
  and means the same as the operator, so linear typing works. Paste makes the box
  (11.3).

## 9. Deleting

Backspace deletes to the left of the caret, Delete to the right. Rules, in order:

1. **Selection:** replaces the selection with a placeholder: `a + ⟨b·c⟩|` →
   `a + |■`. If the selection is the statement root, the region becomes empty
   (`|■`), and is removed when editing ends, as an empty region is today.
2. **Inside a leaf:** deletes one character. A leaf left empty becomes a placeholder.
3. **On a placeholder** (Backspace with `|■` after an operator, or Delete before one):
   removes the placeholder together with the operator or box that owned it, keeping
   the other operand. This is the "wrong operator" undo:
   `a + |■` Backspace → `a|`; `[a / |■]` Backspace → `a|`; `x^[|■]` Backspace → `x|`;
   `−|■` Backspace → `|■`; `f(a, |■)` Backspace → `f(a|)`.
4. **Next to a binary operator** (Backspace at the start of its rhs, or Delete at the
   end of its lhs): replaces the operator with an operator placeholder:
   `a + |b` → `a |□ b`. Typing an operator fills it (`*` gives `a·|b`); Backspace
   again on `□` joins the two sides only if one is a placeholder (rule 3), and
   otherwise does nothing.
5. **At the start of a slot whose box has content** (Backspace at the start of a
   denominator, exponent, radicand, group, or argument):
   - `Group` and `Radical`: remove the box and keep its contents: `(|a + b)` →
     `|a + b`; `√[|x]` → `|x`. (Unwrapping is what's wanted here.)
   - `Fraction`, `Power`, `Call`: select the whole box. A second Backspace deletes it
     by rule 1. This stops a single keystroke from deleting a large expression.
6. **Before a statement operator** (Backspace right after `:=` or `=`): if the rhs or
   unit is a placeholder, turns the statement back into a bare expression:
   `x := |■` → `x|`; `a + b = |■` → `a + b|`. Otherwise it selects the rhs.
7. **In a derivative or integral.** On an empty slot:
   - an order is removed: `d/dx^[|■] f` → `d/dx| f`;
   - a limit is removed with the other one when that's empty too, making the
     integral indefinite: `∫_[|■]^[■] f dx` → `∫ |f dx`;
   - otherwise, when nothing but the operand is filled in, the operator goes and the
     operand stays, undoing `?`, `&` or Ctrl+I typed by mistake:
     `d/d|■ y(x)` → `|y(x)`, `∫ y(x) d|■` → `y(x)|`;
   - otherwise the whole derivative or integral is selected, as it is from the start
     of a slot with content (rule 5): `∫_[0]^[|■] f dx` → `⟨∫_[0]^[■] f dx⟩|`.
8. **At the first or last stop of the region:** nothing.

Delete mirrors Backspace (left ↔ right, end of slot ↔ start of slot).

## 10. Clipboard and undo

- Cut and Copy put the selection on the clipboard as linear text (section 11.3), so
  it pastes anywhere as readable math. With an insertion point, they copy the whole
  region. A selection containing a placeholder has no linear form; Copy then does
  nothing.
- Paste parses the clipboard as linear syntax and inserts the resulting tree (11.3):
  it replaces a selection or placeholder, and otherwise multiplies, as typing an
  operand does (6.4). If the text doesn't parse, nothing is pasted and the error is
  shown. Pasting does not replay keystrokes, so `x^2+1` pastes as `x² + 1`, not
  `x^[2 + 1]`.
- Undo and redo work as they do now: each structural edit (operator, wrap, delete,
  paste) is its own step; consecutive characters typed into one leaf, and consecutive
  characters erased from one leaf, merge into one step. Undo restores the caret and
  selection as they were before the step.

## 11. Storage, evaluation and linear text

The editor tree is the only representation of a math region. It is saved as a tree,
evaluated from the tree, and converted to and from linear text only for the
clipboard. Nothing requires tree → text → tree to come back identical.

### 11.1 File format

`.lemma` stays JSON. A math region stores its tree instead of a `source` string, as
nested objects with full kind names and named slots. For `I := b h^3 / 12`:

```json
{"kind": "definition",
 "lhs": {"kind": "identifier", "text": "I"},
 "rhs": {"kind": "fraction",
         "num": {"kind": "multiply", "style": "juxtaposed",
                 "lhs": {"kind": "identifier", "text": "b"},
                 "rhs": {"kind": "power",
                         "base": {"kind": "identifier", "text": "h"},
                         "exp": {"kind": "number", "text": "3"}}},
         "den": {"kind": "number", "text": "12"}}}
```

- `Binary` saves as `add`, `subtract` or `multiply` (with `"style": "dot"` or
  `"juxtaposed"`), or `missing-operator` for an operator placeholder.
- Numbers keep the text as typed (`9.810`), so editing them shows what the user typed.
- Placeholders and operator placeholders are ordinary nodes, so a half-finished region
  saves and loads like any other.
- Node ids are not saved; they are assigned on load.
- Text regions and plot placeholders keep their strings.
- Derivatives and integrals (#41) save as
  `{"kind": "derivative", "expr": …, "var": …, "order": …}`, with `order` only for an
  n-th derivative, and `{"kind": "integral", "expr": …, "var": …, "from": …, "to": …}`,
  with `from` and `to` only for a definite integral. They are new node kinds, so the
  version goes from 5 to 6, and version 5 files are refused.

The full schema is settled with the "file format" item of the issue. The version goes
from 3 to 4. Lemma is pre-release, so version 4 does not load older files: they are
refused with a clear message, not migrated.

### 11.2 Evaluation

The tree is lowered directly to `lemma::Node`, so the evaluator, units and their tests
are unchanged and no text is parsed on the way.

- Each `lemma::Node` records the id of the editor node it came from. Errors refer to
  that node, so the editor highlights the offending subtree. Byte offsets remain only
  for text that is still parsed: plot placeholders and pasted text.
- `Group` lowers to its contents, juxtaposition to `*`, `Radical` to a call of `sqrt`,
  `Fraction` to `/`. `Derivative` and `Integral` lower to calls in their linear form,
  `diff(f, x, n)` and `integrate(f, x, a, b)`, which is also what a plot's trace
  parses to; the evaluator computes them with the symbolic engine (symbolic spec,
  section 15).
- A region containing a placeholder or an operator placeholder is incomplete. It is
  not evaluated and shows "Fill in the placeholder" on the first one.

### 11.3 Linear text (copy and paste)

Copy writes linear text from the tree. Its only promise is that the text parses back
to the same value:

- `Fraction` writes `num/den`, adding parentheses around either side only when linear
  precedence needs them: `[a + b / c]` → `(a + b)/c`, `[a / b·c]` → `a/(b*c)`.
- `Power`, `Radical`, `Call`, `Group`, `Derivative`, `Integral`, juxtaposition and
  the statement nodes write their linear forms (section 2). Implied parentheses are
  written.

Paste parses linear text with the existing parser and converts the AST node by node:
`/` becomes a `Fraction`, `sqrt(x)` a `Radical`, `10 N` a juxtaposition, and a call
of `diff` or `integrate` whose second argument is a name a `Derivative` or `Integral`.
Parentheses in pasted text become no `Group`: the parser drops them, and layout draws the ones
precedence needs. So `(a)*b` pastes as `a·b`. That is acceptable for a paste, and it
avoids changing the parser.

## 12. Decisions

Settled in review:

1. **Slots capture typing** (5.3): typing `x^2+1` keystroke by keystroke gives
   `x^[2 + 1]`, as in Mathcad. The user leaves a slot with Right or Space.
2. **Operator placeholder** (9, rule 4): Backspace after `a + |b` gives `a □ b`, as in
   Mathcad 15, rather than merging `a` and `b` or deleting `b`.
3. **Juxtaposition** (2, 8.1): only a letter after a number juxtaposes (`10 N`).
   Letters in a row are one name (`LF`); multiplying names needs `*`.
4. **`(` wraps a selection** (8.2), rather than Mathcad's `'`, so `(` means one thing.
5. **The tree is the file format and the engine's input** (11). Linear text is only for
   the clipboard. The file stays JSON: worksheets are small, and JSON is readable,
   diffable and already supported by Qt. Protobuf would add a build dependency and
   binary files for no gain at this size; XML is more verbose to no benefit, and
   MathML is better offered as a separate export. Pre-release, older file versions are
   refused rather than migrated.
6. **Space first selects the leaf** (3, 7): from an insertion point, the first Space
   underlines just the leaf at the caret, so the next operator wraps only it
   (`a·b|` then `Space + c` gives `a·(b + c)`). It is the only keyboard way to wrap the
   last operand alone, and it keeps Space consistent between leaves and boxes. An
   underline always means the next operator wraps exactly what is underlined.

## 13. Implementation notes

Where the spec was silent, the implementation settles it like this. Each is tested in
`tests/test_math.cpp` or `tests/gui/test_canvas.cpp`.

- **Exponents.** After `2e` (shown `2 e`), a digit, `+` or `-` continues the number's
  exponent: `2e-3`. Until a digit follows the sign, the number is incomplete and is
  reported as one. Backspace back to `2e` gives `2 e` again.
- **Juxtaposition has no operator to leave behind.** Emptying either side of a
  juxtaposition removes it: `10 N` Backspace gives `10`, not `10 □`.
- **Next to a box.** Backspace right after a box, or Delete right before one, selects
  it, as rule 5 does from inside. Backspace right after a minus sign, or Delete right
  before one, removes the sign.
- **`(` at the start of a leaf** multiplies a new group in front: `|x` gives `(|■)·x`.
- **Function names.** A name never splits and never takes an operator: `f|(x)` then
  `+` does nothing. Before the name is before the whole call: `|f(x)` then `-` gives
  `−f(x)`.
- **Dragging** selects only in the region being edited. Dragging any other region moves
  it, as before.
- **Incomplete input.** While a region is being edited, an unfilled placeholder isn't
  reported as an error; it is once editing ends.
- **Derivatives and integrals** are typeset as in a textbook. `d` is upright, as ISO
  80000-2 writes it, so it never reads as a variable `d`. `d/dx` is a fraction at full
  size, with the order as a superscript on `d` and on the variable; the caret goes
  only to the one on the variable. `∫` is the math font's glyph, scaled to cover the
  integrand and at least 1.5 times the text size, with its limits at its upper and
  lower right, and the integrand followed by a thin space and `dx`. The sizes are
  hard-coded until #35 reads the font's `MATH` table.
- **Typesetting.** Names that meant units are upright and other names italic (math
  italic code points), as are built-in function names. A result with no unit given is
  shown in its default unit, typeset as a tree (`J`, `kg·m/s²`). A name's part after
  `_` is a subscript (`δ_allow`). Fractions keep their contents at full size.
- **Starting a region.** Typing at the crosshair starts a region only if the key does
  something there: `)` or Space alone leaves no region behind. Paste at the crosshair
  starts a region with the pasted math.
- **Paste errors** are shown in the status bar.
- **A call always has an argument slot.** Pasted `f()` becomes `f(■)`.
- **Erasing never leaves a broken leaf.** Backspace or Delete does nothing if it would
  leave text that isn't a number or a name, like `2-3` from `2e-3`, or a function name
  starting with a digit. Copy likewise refuses anything that wouldn't paste back.
- **Undo** of any edit to a region's contents, not only typing, goes back to editing it.
