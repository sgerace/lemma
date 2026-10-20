# Lemma

A Mathcad-style worksheet for engineering and scientific calculations: math placed
freely on a page, with live recalculation and first-class units, saved as a portable
file you can email like any other document.

```
L := 2.5 m          P := 4 kN          E := 200 GPa
b := 50 mm          h := 100 mm
I := b h³ / 12      I = 4.167×10⁶ mm⁴
δ := P L³ / (3 E I) δ = 25 mm
```

> **Status:** early prototype. The calculation engine (units, definitions, functions)
> works and is tested, and math is edited and typeset in 2D, Mathcad style.

## Features

- **Free placement:** click anywhere on the page to place the red crosshair, then type.
- **Units everywhere:** quantities carry dimensions through every calculation, and
  mismatches like `1 m + 1 kg` are reported as errors.
- **Mathcad evaluation order:** the worksheet calculates top to bottom, then left to
  right, so a name can only be used below where it's defined. Drag a region to move it.
- **2D math:** fractions stack, exponents rise and radicals grow as you type, set in
  STIX Two Math. You edit the expression itself, not a line of text: the selection is
  always a whole subexpression, and Space grows it (see [the editing
  spec](docs/editor-spec.md)).
- **Live results:** results update as you type.
- **Text regions:** type `"` at the crosshair for a heading or note. Enter starts a new
  line, Escape finishes, and dragging the right edge sets where the text wraps. Text is
  never evaluated.
- **X-Y plots:** type `@` at the crosshair, then a function name like `f` (or
  expressions such as `f, sin(x) / 2`). Tab steps through the x variable and the axis
  limits; empty y limits fit the data. Limits can carry units, e.g. `0 m` to `200 mm`.
  Drag the corner to resize.
- **Symbolic math:** Ctrl+. evaluates symbolically (`→`), with keywords above the arrow:
  `simplify`, `expand`, `factor`, `substitute`, `solve`, `diff` and `integrate`, plus
  `keep` and `real`. Names defined above are replaced by their definitions, units
  are kept and checked, and a definition like `θ(x) := y(x) → diff, x` defines a
  derived formula to evaluate and plot. See `examples/beam-derivation.lemma` and
  `docs/symbolic-spec.md`.
- **Portable files:** `.lemma` files are versioned JSON, storing each expression as a
  tree, so they're human-readable and diff well in version control.
- **PDF export** of the page layout, US Letter.

## Typing math

Type as you would write it on one line; the editor builds the 2D expression.

| Keys | Result |
|---|---|
| `L:2m` | `L := 2 m`. `:` defines, and a letter after a number multiplies, for units |
| `L=` | Shows the result; type a unit after it, like `ft`, to choose the unit |
| `f(x):x^2` | Defines a function |
| `a/b` `x^2` `\x` | Fraction, exponent, square root |
| → | Leave a denominator or exponent: typing stays in it until you do |
| Space | Select more. The next operator applies to everything underlined: `a+b` Space Space `*c` gives (a + b)·c |
| Shift+Space | Select less |
| Tab | Go to the next placeholder ■ |
| Backspace | After an operator you just typed, removes it: `a+` Backspace gives `a` |
| Ctrl+C, Ctrl+V | Copy and paste as linear text, e.g. `w L^4/(8 E I)` |
| Ctrl+. | Evaluate symbolically: `→`, with a keyword above it like `diff, x`; in a definition, its value |
| Ctrl+= | An equation's bold equals sign, for `solve`: `M ⩵ σ I/c` |
| Ctrl+Shift+C | Copy just the result |

Letters in a row are one name (`LF`); multiply names with `*`. A name's part after `_`
is shown as a subscript (`δ_allow`).

**Built-in functions:** `sqrt`, `abs`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`,
`ln`, `log` (base 10), `exp`, `min`, `max`.

**Constants:** `π` (or `pi`), `e`, and `g` (standard gravity). As in Mathcad, grams are
`gm` because `g` is gravity.

**Units:** SI and US customary units for length, mass, time, force, energy, power,
pressure, volume, electrical quantities and angles (`rad`, `deg`), including
`m mm ft in kg lb N kN lbf kip J W hp Pa MPa psi ksi atm L gal` and more. Your own
variables can shadow unit names; the display unit after `=` is always read as a unit.

## Building

Requirements:
- CMake 3.25+
- A C++20 compiler
- Qt 6.5+ (Widgets and Test)

Catch2 is downloaded automatically at configure time.

### Qt Creator

Open `CMakeLists.txt` with **File → Open File or Project…**, choose a Qt 6 kit, then
build and run the `lemma` target.

### Command line

The `debug` and `release` presets in `CMakePresets.json` use Ninja. Point CMake at your
Qt and compiler, either on the command line or in a personal `CMakeUserPresets.json`
(gitignored).

**Windows**, e.g. with the Qt online installer's MinGW kit:

```sh
cmake --preset debug -DCMAKE_PREFIX_PATH=C:/Qt/6.12.0/mingw_64
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

Run the app with an example (Qt's `bin` folder must be on `PATH`):

```sh
build/debug/lemma examples/cantilever-beam.lemma
```

**macOS**, with the Qt online installer's default location:

```sh
cmake --preset debug -DCMAKE_PREFIX_PATH=$HOME/Qt/6.12.0/macos
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

The app is built as a bundle. Launch it with `open`, or run the binary inside the
bundle to see console output in the terminal:

```sh
open build/debug/src/app/lemma.app --args "$PWD/examples/cantilever-beam.lemma"
build/debug/src/app/lemma.app/Contents/MacOS/lemma examples/cantilever-beam.lemma
```

If CMake can't find Ninja, install it (`brew install ninja`) or add Qt's copy in
`~/Qt/Tools/Ninja` to `PATH`.

The GUI tests run on Qt's offscreen platform, so they never touch the desktop.

## Project layout

```
src/core/      calculation engine: units, parser, evaluator, worksheet (no Qt dependency)
src/app/       Qt Widgets app: page canvas, file I/O, PDF export, main window
tests/         Catch2 engine tests; tests/gui/ has the offscreen Qt Test suite
examples/      sample worksheets
```

## License

GPL-3.0. See [LICENSE](LICENSE).
