# Lemma

A Mathcad-style engineering worksheet: math regions placed freely on a page, with
units and live recalculation. C++20 and Qt 6 Widgets, built with CMake.

## Build and test

```sh
cmake --preset debug -DCMAKE_PREFIX_PATH=<path to Qt 6>
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

- On Windows, executables land in the build directory's root, because
  `qt_standard_project_setup()` sets the runtime output directory there (Windows only,
  so the Qt DLLs resolve). On macOS and Linux they stay in their subdirectories:
  the app is `src/app/lemma.app` (a bundle) on macOS or `src/app/lemma` on Linux, and
  the tests are in `tests/`.
- ctest runs the GUI tests with `QT_QPA_PLATFORM=offscreen` and Qt's DLL folder on PATH.
  Running a test binary directly needs both.
- Qt Test console output may not appear in some shells (e.g. Git Bash). Use
  `-o path/results.txt,txt`.
- The project opens directly in Qt Creator from `CMakeLists.txt`. Don't add a qmake
  `.pro` file.

## Architecture

- `src/core` (`lemma_core`) is the engine and **must not depend on Qt**: rationals,
  dimensions/quantities, unit registry, formatting, lexer/parser to AST, evaluator,
  worksheet, and the 2D math editor's model: the editor tree (`math.h`), editing
  commands (`math_edit.h`) and box layout (`math_layout.h`, measuring text through
  `MathFontMetrics`), and the symbolic engine: canonical expressions (`symbolic.h`),
  operations (`symbolic_ops.h`), and the bridge to and from the editor tree with the
  display rules (`symbolic_bridge.h`). Keep it testable from Catch2 without a GUI.
- `src/app` (`lemma_app` library + thin `main.cpp`) is the Qt Widgets layer:
  `WorksheetCanvas` (page, crosshair, editing, painting), `MathRender` (the bundled
  STIX Two Math font and painting math layouts), `DocumentIO` (JSON `.lemma` files),
  `PdfExport`, `MainWindow`.
- A math region is a `MathNode` tree, edited per `docs/editor-spec.md`. It is what
  files store and what's evaluated (lowered to `lemma::Node`); linear syntax is only
  for the clipboard and for building regions in tests (`addRegion(x, y, "x := 2 m")`).
  Plot placeholders are still linear text, parsed by `parser.h`.
- Document coordinates are 1/96 inch from the top-left of the first page. A region's
  `(x, y)` is the left end of its baseline. Pages are US Letter (816 × 1056).
- Evaluation order is top to bottom, then left to right, by region position. Functions
  capture the definitions above them (closure snapshot), as in Mathcad.
- Values are stored in SI base units; `Dimension` holds rational exponents so
  `sqrt(m^2)` is exactly `m`.
- `.lemma` files carry `"format": "lemma-worksheet"` and an integer `"version"`. Bump the
  version on any incompatible change. Lemma is pre-release, so don't migrate older
  versions: refuse them with a clear message. Once Lemma is released, keep loading
  every released version. Math regions store their tree (spec section 11.1).

## Planned direction

Math is edited as a tree (Mathcad-style: the selection is a subtree, and the spacebar
grows it) with TeX-style box layout; `docs/editor-spec.md` is the agreed behavior, so
change the spec along with the editor. Don't adopt a third-party math editor widget.
Still to come: matrices, literal subscripts, and the other non-goals listed in the
spec; reading layout constants from the font's `MATH` table instead of hard-coding
them. Symbolic math (`→`) is being built in-house per `docs/symbolic-spec.md`, in the steps
of its section 13; change the spec along with the engine.

## Conventions

- Match the existing style: 4-space indent, `camelCase` methods, `trailing_` members,
  `k`-prefixed constants, brief comments that explain *why*.
- Engine changes need Catch2 tests in `tests/`. Canvas or interaction changes need a
  case in `tests/gui/test_canvas.cpp`. Editing behavior is tested in
  `tests/test_math.cpp` in the spec's notation (`a + b·c|`); keep the randomized test
  there passing. Symbolic results are tested in `tests/test_symbolic.cpp` as the linear
  text Copy writes; keep its randomized check against numeric evaluation passing.
- Source strings are UTF-8 (`std::string` in core). The app converts with
  `QString::fromStdString` / `toStdString`. Errors in math regions name a node
  (`Error::node()`, `RegionResult::errorNode`); errors in parsed text (plot fields,
  pasted text) are byte offsets.

## GUI testing

Test UI behavior offscreen, never on the real desktop: don't inject OS-level input
(SendKeys, mouse_event and the like) and don't take screen captures.

- Write Qt Test cases that send events to the widget (`QTest::keyClicks`,
  `QTest::mouseClick` on `canvas->viewport()`).
- For a visual check, run the `snapshot` test with `LEMMA_SNAPSHOT_DIR` set. It saves
  `canvas.png` (via `widget->grab()`), `canvas.pdf`, `editing.png` (a region being
  edited, with a selection) and `editing-symbolic.png` (a `→` region's keyword being
  edited) offscreen. Text may render bold offscreen because of font
  fallback; the real app doesn't. Math is always in the bundled font.
