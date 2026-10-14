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
> works and is tested; equations are typed in a linear syntax for now, ahead of a
> proper 2D math editor.

## Features

- **Free placement:** click anywhere on the page to place the red crosshair, then type.
- **Units everywhere:** quantities carry dimensions through every calculation, and
  mismatches like `1 m + 1 kg` are reported as errors.
- **Mathcad evaluation order:** the worksheet calculates top to bottom, then left to
  right, so a name can only be used below where it's defined. Drag a region to move it.
- **Live results:** results update as you type.
- **Portable files:** `.lemma` files are versioned JSON, so they're human-readable and
  diff well in version control.
- **PDF export** of the page layout, US Letter.

## Syntax

| Input | Meaning |
|---|---|
| `L := 2 m` | Define a variable. Typing `:` inserts `:=`. |
| `L =` | Show a result in its default unit |
| `L = ft` | Show a result in a chosen unit |
| `f(x) := x^2` | Define a function |
| `10 N/m` | A space multiplies, like `*` (`·` and `×` work too) |
| `M (h/2)` | Parentheses after a value multiply |

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
(gitignored), e.g. for the Qt online installer's MinGW kit on Windows:

```sh
cmake --preset debug -DCMAKE_PREFIX_PATH=C:/Qt/6.12.0/mingw_64
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

Run the app with an example (on Windows, Qt's `bin` folder must be on `PATH`):

```sh
build/debug/lemma examples/cantilever-beam.lemma
```

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
