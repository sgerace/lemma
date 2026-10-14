# Lemma

A Mathcad-style engineering worksheet: math regions placed freely on a page, with
units and live recalculation. C++20 and Qt 6 Widgets, built with CMake.

## Build and test

```sh
cmake --preset debug -DCMAKE_PREFIX_PATH=<path to Qt 6>
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

- Executables land in the build directory's root, not a subdirectory, because
  `qt_standard_project_setup()` sets the runtime output directory.
- ctest runs the GUI tests with `QT_QPA_PLATFORM=offscreen` and Qt's DLL folder on PATH.
  Running a test binary directly needs both.
- Qt Test console output may not appear in some shells (e.g. Git Bash). Use
  `-o path/results.txt,txt`.
- The project opens directly in Qt Creator from `CMakeLists.txt`. Don't add a qmake
  `.pro` file.

## Architecture

- `src/core` (`lemma_core`) is the engine and **must not depend on Qt**: rationals,
  dimensions/quantities, unit registry, formatting, lexer/parser to AST, evaluator,
  worksheet. Keep it testable from Catch2 without a GUI.
- `src/app` (`lemma_app` library + thin `main.cpp`) is the Qt Widgets layer:
  `WorksheetCanvas` (page, crosshair, editing, painting), `DocumentIO` (JSON `.lemma`
  files), `PdfExport`, `MainWindow`.
- Document coordinates are 1/96 inch from the top-left of the first page. A region's
  `(x, y)` is the left end of its baseline. Pages are US Letter (816 × 1056).
- Evaluation order is top to bottom, then left to right, by region position. Functions
  capture the definitions above them (closure snapshot), as in Mathcad.
- Values are stored in SI base units; `Dimension` holds rational exponents so
  `sqrt(m^2)` is exactly `m`.
- `.lemma` files carry `"format": "lemma-worksheet"` and an integer `"version"`. Bump the
  version on any incompatible change and keep loading older versions.

## Planned direction

The current linear text editor is a stopgap. The goal is a custom 2D math editor that
edits the expression tree directly (Mathcad-style: the selection is a subtree, and the
spacebar grows it), with TeX-style box layout. Don't adopt a third-party math editor
widget. Symbolic math (e.g. SymEngine) comes later and is not part of the MVP.

## Conventions

- Match the existing style: 4-space indent, `camelCase` methods, `trailing_` members,
  `k`-prefixed constants, brief comments that explain *why*.
- Engine changes need Catch2 tests in `tests/`. Canvas or interaction changes need a
  case in `tests/gui/test_canvas.cpp`.
- Source strings are UTF-8 (`std::string` in core). The app converts with
  `QString::fromStdString` / `toStdString`. Error positions are byte offsets.

## GUI testing

Test UI behavior offscreen, never on the real desktop: don't inject OS-level input
(SendKeys, mouse_event and the like) and don't take screen captures.

- Write Qt Test cases that send events to the widget (`QTest::keyClicks`,
  `QTest::mouseClick` on `canvas->viewport()`).
- For a visual check, run the `snapshot` test with `LEMMA_SNAPSHOT_DIR` set. It saves
  `canvas.png` (via `widget->grab()`) and `canvas.pdf` offscreen. Text may render bold
  offscreen because of font fallback; the real app doesn't.
