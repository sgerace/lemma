# Symbolic math: design

Status: **agreed** (issue #16). Nothing here is implemented yet; section 13 is the
plan. Change this note along with the engine.

Lemma evaluates numerically: every expression becomes a `Quantity`. This note adds
Mathcad's symbolic evaluation operator, `→`, which returns an expression instead of
a number. Engineers use it to simplify, expand or factor an expression, to solve an
equation for a variable, to differentiate or integrate, and to check a derivation
before plugging numbers in. The result is typeset with the 2D layout from #7.

## 1. Decisions in brief

| Question (from #16) | Decision | Section |
|---|---|---|
| Engine | A small in-house engine in `lemma_core`, behind a narrow back-end interface so a full CAS (e.g. Giac) can be added later for operations beyond its scope | 2, 3 |
| Placement | `lemma_core`, no Qt, no new dependency, so it builds wherever Lemma does (#8) | 3 |
| Syntax | `→` (Ctrl+. or the `→` character), with an optional keyword above the arrow: `simplify`, `expand`, `factor`, `substitute`, `solve`, `diff`, `integrate` | 6 |
| Definitions | Names defined above are replaced by their definitions, as in Mathcad; the keyword `keep` opts names out | 7 |
| Assumptions | Free symbols are positive reals; the keyword `real` opts a name out | 5.3 |
| Symbolic definitions | `θ(x) := y(x) → diff, x` defines a function from a symbolic result, to evaluate or plot | 7.1 |
| Units | Unit names are symbols with known dimensions, kept as typed and combined like other powers; mismatches are errors, as in numeric evaluation | 8 |
| Conversion | Tree ↔ engine expressions; errors name the tree node they arise in | 4 |
| Storage | The `→` statement is a new root node, so `.lemma` goes to version 5. Results are recomputed, not stored | 10 |
| Numbers | Exact arithmetic throughout, shown as decimals once a decimal number is involved | 5.2 |
| First cut | All seven keywords, each within a documented scope (section 9), and symbolic definitions | 9, 12, 13 |

Two assumptions in #16 changed with #7: errors now name a tree node, not a byte range,
and since files store trees, a new statement node does need a version bump.

## 2. Goals and non-goals

Goals:

- Worksheet algebra done well: polynomial and rational expressions in many symbols,
  rational exponents (so roots are exact), and the elementary functions as atoms with
  known rules. This covers textbook derivations in statics, beams, columns, heat
  transfer and fluids.
- **Readable results.** `w L⁴/(8 E I)`, not `(1/8)*w*L**4/(I*E)`. Presentation is a
  first-class part of the engine (section 11), not an afterthought.
- **Units throughout.** Symbolic results carry units and are checked like numeric ones.
- **A known ceiling.** Beyond its scope, an operation returns its input unchanged with
  a note ("Couldn't integrate this"), never a wrong answer.
- Numeric evaluation, and every existing worksheet, behave exactly as before.

Non-goals (for the in-house engine; a back end may add them later):

- General integration (the Risch algorithm), special functions, limits and series.
- Multivariate factoring, Gröbner bases, systems of nonlinear equations.
- Trigonometric identities beyond `sin² + cos² = 1` and simple angle forms.
- General assumptions (`x > 2`, `n` an integer): free symbols are positive reals, or
  real with the `real` keyword (section 5.3), and nothing finer.
- Matrices (they come with #27).

## 3. Architecture

```
MathNode tree ──lower──▶ Expr (canonical) ──operation──▶ Expr ──raise──▶ MathNode ──layout──▶ screen/PDF
                              ▲                    │
                     definitions above       SymbolicBackend
                     (section 7)             (in-house first)
```

New files in `src/core`, all without Qt:

- `symbolic.h`: `Expr`, the canonical expression, and its construction (section 5).
- `symbolic_ops.h`: simplify, expand, factor, substitute, solve, diff, integrate.
- `symbolic_bridge.h`: `MathNode` ↔ `Expr` (section 4) and the display rules
  (section 11).

**The back-end boundary.** The operations that are hard to do in general (integrate,
factor, solve) go through an interface:

```cpp
class SymbolicBackend {
public:
    virtual ~SymbolicBackend() = default;
    // Each returns nothing when the expression is beyond the back end's scope.
    virtual std::optional<Expr> integrate(const Expr& e, const Symbol& x) const = 0;
    virtual std::optional<Expr> factor(const Expr& e) const = 0;
    virtual std::optional<std::vector<Expr>> solve(const Expr& lhs, const Expr& rhs, const Symbol& x) const = 0;
};
```

The in-house engine is the first back end. A later one (Giac, say) would sit behind
the same interface, converting to and from `Expr`. Units, definitions and display
stay in Lemma's own layer whichever back end runs. That's why the in-house core
lasts even if a back end is added: it is the part that makes results readable and
dimensionally sound.

## 4. The bridge

**Tree to `Expr`.** Lowering works like numeric lowering (`lowerMath`), node by node,
and an error raised while building a node's expression ("Units don't match",
"Division by zero") names that node, so it underlines the right part of the region. Groups
disappear, fractions become products with negative exponents, radicals become
exponent ½, and juxtaposition is multiplication.

**`Expr` to tree.** Raising builds a fresh `MathNode` tree for the result, following
the display rules of section 11, and the 2D layout typesets it. The result tree is not
editable, like a numeric result today; Copy (section 6.4) can take it as text.

## 5. The expression

### 5.1 Forms

An `Expr` is immutable and shared (`std::shared_ptr<const ...>`), in one of these
forms:

| Form | Holds | Example |
|---|---|---|
| Number | an exact `Rational`, or a decimal (section 5.2) | `3/8`, `2.5` |
| Symbol | a name, and what it is: a variable, a unit (with its `Dimension`), or a constant (π, e, g) | `L`, `kN`, `π` |
| Sum | a numeric term plus terms with coefficients | `3 + 2 x − y` |
| Product | a numeric coefficient times factors, each a base raised to an exponent (a number, or any expression); a single power is a product of one factor | `3/8 w L⁴ E⁻¹ I⁻¹`, `x²`, `e^(k t)` |
| Function | an elementary function applied to an expression: sin, cos, tan, asin, acos, atan, ln, log and abs (√ and exp are powers) | `sin(θ)`, `ln(x)`, `\|x − y\|` |
| Call | an undefined function applied to arguments, kept as is | `f(x)` |

### 5.2 Canonical form

Construction always gives the canonical form, so equal expressions are identical and
can be compared and hashed:

- Numbers fold: `2 + 3` is `5`, `x · 1` is `x`, `x⁰` is `1`.
- Like terms collect (`2 x + 3 x` is `5 x`) and like factors combine (`x² · x³` is
  `x⁵`; `√x · √x` is `x`).
- A sum's terms and a product's factors are kept in a fixed total order. This is the
  storage order only; the display order is chosen separately (section 11).
- A multiple of a sum is multiplied out inside a sum (`1 − (z + 3)` is `−z − 2`),
  but kept on its own (`2 (x + 1)`).
- Number roots are reduced: `√8` is `2 √2`, `√2 · √6` is `2 √3`.
- Integers and fractions are exact. A number too large for an exact fraction (64-bit
  numerator and denominator) becomes a decimal, as does anything computed from one.
- A number typed with a decimal point (or in scientific notation) is exact too
  (2.5 is 5/2), but marked as decimal, and a result coefficient with decimal input
  in it is shown as a decimal to the worksheet's precision (`2.5 x` stays `2.5 x`,
  not `5/2 x`). This matches what people expect from Mathcad.

### 5.3 Built-in assumptions

Symbols are positive reals. So `√(x²)` is `x`, `ln(a b)` can be `ln(a) + ln(b)`,
and `(a b)^(1/2)` is `√a √b`. Sums aren't assumed positive: `√((x − y)²)` is
`|x − y|`, and a root splits only over the factors known to be positive, so
`√(x (y − 1)(y − 2))` is `√x √((y − 1)(y − 2))`. This is the one deliberate departure from a
general-purpose CAS (Mathcad's symbols are complex until `assume` says otherwise): it
is right for lengths, loads and moduli, and it is what keeps results short.

What limits the risk:

- **It only touches free names.** Definitions are substituted (7), so a defined name
  arrives as its value with its sign: with `y := −3 mm`, `√(y²)` is `3 mm`. The
  assumption applies only to names with no definition above.
- **An opt-out.** The keyword `real, y` makes `y` a real of either sign for the
  region: `√(y²)` is then `|y|`, and `ln(a y)` stays whole. Like `keep`, it combines
  with other keywords (`real, y; diff, x`).
- **`solve` doesn't use it.** It never discards a root on sign: `F ⩵ k d²` gives both
  `d = √(F/k)` and `d = −√(F/k)` (9.5). Two roots that differ only in a radical's sign
  are shown as one, with `±`.

## 6. Syntax and editing

### 6.1 The statement

A symbolic evaluation is a new root node, `Symbolic`, with an expression, one or more
keyword slots, and a computed result:

```
expression  →  result         keyword above the arrow when given:   solve, σ
                                                          expression  ────→  result
```

- **expression**: any expression, or an equation (6.3).
- **keywords**: one slot to start with, a placeholder when none is given; `;` adds
  another. A slot holds a keyword's name (`simplify`), or a `Keyword` node: a name
  and one or more arguments (`solve, σ`). The keywords are shown above the arrow,
  smaller, the arrow lengthening to fit, and hidden when all are empty and the region
  isn't being edited, as an evaluation's unit slot is (editor spec, section 8.4). Like
  a function's name, a keyword's name takes only letters, digits and `_`.
- **result**: computed, not a slot and not a stop, like a numeric result.

### 6.2 Keys

| Key | Does | Notes |
|---|---|---|
| Ctrl+. (or typing `→`) | Turns a bare expression into `Symbolic(expression, ■)`, caret on the keyword placeholder. In a definition, it acts on the right-hand side (7.1) | Mathcad's key. Acts on the whole region (or right-hand side), like `:` and `=` |
| A name in the keyword slot, then `,` | Starts the keyword's arguments: `solve,` gives `solve, ■` | Arguments are separated by `,` |
| `;` in the keyword slot | Starts another keyword. Keywords apply left to right: `keep, P; diff, x` | Mathcad stacks keywords the same way |
| Ctrl+= | An equation's equals sign, `⩵` (drawn as a bold `=`) | Only meaningful as a symbolic expression; elsewhere it's an error |
| Backspace right after `→` | With an empty keyword, turns the region back into the bare expression | Like rule 6 for `=` and `:=` |

Everything else edits as in the editor spec: the keyword and its arguments are
ordinary slots.

### 6.3 Equations

`lhs ⩵ rhs` is a new `Equation` node, used by `solve` (`M ⩵ σ I / c`, then
`solve, σ`). With any other keyword, or none, both sides are simplified. A bare
expression given to `solve` means `expression ⩵ 0`.

### 6.4 Linear form

For the clipboard (editor spec, section 11.3):

| Tree | Linear |
|---|---|
| `x² − 1 →` | `x^2 - 1 ->` |
| `x² − 5x + 6 →` with `factor` | `x^2 - 5*x + 6 -> factor` |
| `M ⩵ σ I/c →` with `solve, σ` | `M == σ*I/c -> solve, σ` |

Copy (Ctrl+C) gives this form for the whole region, as for any region. Copy Result
(Ctrl+Shift+C) gives just the result, e.g. `M*c/I`, so it can be pasted into a new
region; it works for numeric results too. The parser learns `->`, `==` and the keyword
list.

## 7. Definitions

As in Mathcad, a name defined above the region is replaced by its definition:

- **Variables.** Replaced by the right-hand side of their definition, as an
  expression, not by its numeric value. `P := 4 kN` makes `P` mean `4 kN`, keeping the
  unit as typed. Substitution repeats until only undefined names, units and constants
  remain, and stops at the region's own position (top to bottom, then left to right),
  as numeric evaluation does.
- **Functions.** Inlined, with the arguments substituted for the parameters. A
  function's body sees the definitions above *its* definition, as numerically
  (closure snapshot).
- **Built-in functions** (sin, ln, …) are the elementary functions of section 5.1;
  `min` and `max` are kept as calls.
- **Opting out.** The keyword `keep, L, P` leaves the names after it symbolic. It
  combines with another keyword (6.2): `keep, P; diff, x`. Mathcad's idiom of
  redefining a name as itself (`x := x`) also works, since substitution then gives
  `x` back.

Why substitute by default: worksheets are written top to bottom with values defined
first, and `→` then shows the formula with the numbers in it, which is how an engineer
checks their work. To derive a formula, they use undefined names, or `keep`.

### 7.1 Symbolic definitions

A definition's right-hand side may be a symbolic evaluation:

```
θ(x) := y(x) → diff, x  →  P x (2 L − x)/(2 E I)
```

- **Meaning.** The name is defined as the *result*, as if it had been typed there.
  Later regions evaluate it numerically, plot it, or use it in another `→`, like any
  definition. The result is shown after the arrow, as in a plain `→` region.
- **Parameters.** A function's parameters are never substituted, even when a name
  like `x` is defined above: inside `θ(x) := …`, `x` is the parameter.
- **Order.** The result is computed at the definition's position, from the
  definitions above it, then captured like any function's body (closure snapshot).
- **Beyond scope.** When the operation couldn't finish (an integral left unevaluated),
  the definition still shows the result with its note, and using the name numerically
  is an error at the use: "θ has no closed form". Numeric integration comes later.
- **Variables** work the same way: `M_max := w L²/8 → keep, w` defines `M_max` as
  the expression, which evaluates numerically wherever `w` is defined.
- Errors in the symbolic step are reported on the definition, and later uses of the
  name are errors ("θ isn't defined"), as with a numeric definition that fails.

## 8. Units

- **Which names are units.** Several unit names are also everyday variable names
  (`L` is a litre, and `C`, `N`, `A`, `K`, `R`, `s` and `m` are units too), so a name
  the worksheet doesn't define is a unit only where it's used as one: in the units
  right after a number (`2.5 m`, `4 kN·m`, `9.81 m/s²`), or in a definition's value
  that is substituted (`P := 4 kN`). Anywhere else it's a plain symbol, whose
  dimension is unknown: in `w L⁴/(8 E I)`, `L` is a length, not a litre. A unit
  becomes a unit Symbol with its `Dimension`.
- Units are kept as typed and combine like any factor: `2 m · 3 m` is `6 m²`, and
  `4 kN · 2.5 m` is `10 kN·m`.
- Terms of the same dimension in different units are converted to the unit of the
  first term: `2 m + 3 mm` is `2.003 m`. The factor is exact when the units' sizes are
  (mm is 1/1000 m); otherwise it's a decimal.
- **Checking.** Adding terms whose dimensions are known and different is an error with
  the numeric evaluator's message (`1 m + 1 kg`: "Units don't match: m and kg").
  Terms with an unknown dimension (anything with a plain symbol in it) can't be
  checked and are allowed, as Mathcad does.
- Exponents must be unitless where known, as numerically.

## 9. Operations

Each operation has a documented scope. Beyond it, the result is the input,
simplified, with a note under the region in the error style but not red ("Couldn't
factor this further"). That is not an error: the region still counts as evaluated.

### 9.1 No keyword: simplify

The canonical form, plus:

- Cancelling common factors of a fraction's numerator and denominator, including
  polynomial factors found by `factor` (`(x² − 1)/(x − 1)` is `x + 1`).
- Combining fractions over a common denominator when that makes the result shorter.
- `sin²θ + cos²θ = 1`; `e^(ln x) = x`; `ln(e^x) = x`; values at standard angles
  (`sin(π/6) = 1/2`).
- Choosing between expanded and factored forms by a size measure: the shorter wins.

### 9.2 `expand`

Multiplies out products and integer powers of sums, and splits fractions over a sum
in their numerator: `(a + b)³` is `a³ + 3a²b + 3ab² + b³`. Guarded against blowup: more
than a few thousand terms is an error ("This expands to too many terms").

### 9.3 `factor`

- Takes out common factors: `6 a x + 9 a` is `3 a (2 x + 3)`.
- Factors univariate polynomials with rational roots (`x² − 5x + 6` is
  `(x − 2)(x − 3)`), and quadratics over the reals in general (the quadratic formula,
  when its roots are real).
- Recognizes `a² − b²`, `a² ± 2ab + b²`, and `a³ ± b³`.
- Beyond that, through the back end; the in-house one stops.

### 9.4 `substitute, x ⩵ value` (repeatable: `substitute, x ⩵ 1, y ⩵ 2`)

Replaces each named symbol, then simplifies. `value` is any expression.

### 9.5 `solve, x`

- Linear in `x` (after clearing denominators): one solution.
- Quadratic in `x`: two solutions by the quadratic formula, simplified.
- `x` inside one invertible function or power: undoes it (`F ⩵ k d²` gives
  `d = √(F/k)` and `d = −√(F/k)`; `y ⩵ e^(k t)` gives `t = ln(y)/k`).
- Several solutions are shown as a list, `σ = …, σ = …`, until matrices (#27) allow a
  column; two that differ only in a radical's sign are shown as one, with `±` (5.3).
- Beyond that, through the back end; the in-house one returns "Couldn't solve this
  for x".

### 9.6 `diff, x` (and `diff, x, n`)

Differentiates with respect to `x`, `n` times, with the sum, product, quotient, power
and chain rules and the elementary functions' derivatives. In scope for everything the
engine can represent.

### 9.7 `integrate, x` (and `integrate, x, a, b`)

- Indefinite: no constant of integration is added (as in Mathcad).
- In scope: linearity; polynomials, and powers of `x` including `x⁻¹` (ln x); `sin`,
  `cos`, `exp` and `1/x` of a linear argument (`sin(k x + φ)`); integration by parts
  for a polynomial times `sin`, `cos` or `exp` of a linear argument; rational functions
  whose denominator is linear or a power of one.
- Definite: the indefinite integral, evaluated at the limits and simplified. Limits
  may be symbols or have units, and the result's dimension is the integrand's times
  `x`'s.
- Beyond that, the integral is returned unevaluated with "Couldn't integrate this".

## 10. Evaluation and storage

- **Order.** A symbolic region takes its place in the top-to-bottom order like any
  other. It defines nothing, so it never changes numeric results.
- **Results** go in `RegionResult` as a tree (alongside `value`, `number` and `unit`),
  are recomputed on every evaluation, and are never saved.
- **Files.** `Symbolic`, `Keyword` and `Equation` are new node kinds, so `.lemma` goes
  to version 5. The JSON forms: `{"kind": "symbolic", "expr": …, "keywords": […]}`
  (as a region's root, or as a definition's `rhs`, 7.1), where each keyword slot is a
  placeholder, an identifier, or `{"kind": "keyword", "name": …, "args": […]}`; and
  `{"kind": "equation", "lhs": …, "rhs": …}`, only as a whole slot (the region, the
  expression, or an argument). Version 4 files are refused, as pre-release policy
  says.
- **Cost.** Every recalculation reruns symbolic regions. Worksheet-size expressions are
  cheap, and expansion is capped (9.2), so a runaway region gives an error rather than
  a hang.

## 11. Display

Raising an `Expr` to a tree chooses a readable form:

- **Fractions.** Factors with negative exponents form the denominator:
  `w L⁴/(8 E I)`. Numeric coefficients go in front, or into the fraction when they
  are its only numerator (`3/(2 x)`). Negative fractional exponents become radicals in
  the denominator.
- **Main variable.** The keyword's `x`, or else the variable of highest degree, then
  the most used, then the last to appear.
- **Order of factors.** Numbers first, then constants (π), then variables in the
  order they first appear in the region's expression, with the main variable last
  among them (`a x²`, `k t`), then roots (`2 x √y`), functions, sums, and units last
  and upright (`10 kN·m`, `P L³/(3 E I)`).
- **Order of terms.** For a polynomial in a variable of degree above one, descending
  powers: `x² − 5x + 6`. Otherwise, the order of first appearance, with the terms
  that have the main variable after those that don't (`2 L − x`). A constant goes
  last.
- **Signs.** A negative term is written as a subtraction (`a − b`, not `a + (−1) b`);
  a sum whose terms are all negative is negated as a whole.
- **Roots.** Exponent ½ is `√`; other fractional exponents stay as powers (`x^(2/3)`)
  until #30 gives n-th roots.
- **Juxtaposition.** Factors are juxtaposed, as in a textbook (`3 x`, `w L⁴`,
  `x sin(k x)`), except before a number, which would read as part of the one before
  (`3·2^(1/3)`), and between units (`kN·m`).

## 12. Worked examples

Each assumes nothing is defined above unless shown.

| Region | Result | Note |
|---|---|---|
| `(x² − 1)/(x − 1) →` | `x + 1` | 9.1 cancels the common factor |
| `(a + b)³ →` with `expand` | `a³ + 3 a² b + 3 a b² + b³` | |
| `x² − 5x + 6 →` with `factor` | `(x − 2) (x − 3)` | |
| `6 a x + 9 a →` with `factor` | `3 a (2 x + 3)` | |
| `M ⩵ σ I/c →` with `solve, σ` | `σ = M c/I` | |
| `a x² + b x + c →` with `solve, x` | `x = (−b ± √(b² − 4 a c))/(2 a)` | two roots, shown with `±` |
| `y(x) := P x² (3 L − x)/(6 E I)` above; `y(x) →` with `diff, x` | `P x (2 L − x)/(2 E I)` | the function is inlined (7) |
| `w x →` with `integrate, x, 0, L` | `w L²/2` | |
| `x sin(k x) →` with `integrate, x` | `sin(k x)/k² − x cos(k x)/k` | by parts |
| `P := 4 kN`, `L := 2.5 m` above; `P L →` | `10 kN·m` | definitions substituted, units kept as typed |
| same; `P L →` with `keep, P` | `2.5 P m` | units last (11) |
| `2 m + 3 mm →` | `2.003 m` | 8 |
| `1 m + 1 kg →` | Units don't match: m and kg | an error, as numerically |
| `e^(x²) →` with `integrate, x` | `∫ e^(x²) dx`, "Couldn't integrate this" | beyond the scope; not an error |
| `√(x²) →` | `x` | symbols are positive (5.3) |
| `√(y²) →` with `real, y` | `\|y\|` | the opt-out (5.3) |
| `θ(x) := y(x) →` with `diff, x`, with `y` as above | `P x (2 L − x)/(2 E I)`; `θ(L) =` then evaluates | symbolic definition (7.1) |

## 13. Plan

Each step lands with Catch2 tests; the editor and canvas steps also with GUI cases.

1. **Expressions:** `Expr`, canonical construction, the bridge both ways, and the
   display rules. No keyword yet: `→` simplifies (9.1, without polynomial
   cancellation).
2. **Editor and file format:** the `Symbolic` and `Equation` nodes, Ctrl+. and Ctrl+=,
   the keyword slot, linear form, version 5, and results laid out and painted.
3. **Definitions and units:** substitution (7), `keep`, `real`, unit symbols and
   checking (8).
4. **`expand`, `substitute`, `diff`.**
5. **`factor`, then simplify's polynomial cancellation** (9.1), which uses it.
6. **`solve` and `integrate`**, behind `SymbolicBackend`.
7. **Symbolic definitions** (7.1): `Symbolic` as a definition's right-hand side, its
   result defining the name, and plots of it.

The worksheet demo (a beam derivation from load to deflection, then plotted) is
possible after step 7.

## 14. Review

Decided:

1. **Definitions are substituted by default** (7), as in Mathcad.
2. **The keyword goes above the arrow** (6.1), as in Mathcad Prime.
3. **Symbolic definitions are in the first cut** (7.1, plan step 7).
4. **Decimal display** (5.2): exact arithmetic, shown as decimals once a decimal
   number is involved, as on a calculator.
5. **Free symbols are positive reals** (5.3), with the `real` keyword to opt a name
   out, and `solve` keeping every root.
