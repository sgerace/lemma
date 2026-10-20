#pragma once

#include "lemma/math_edit.h"
#include "lemma/math_layout.h"
#include "lemma/worksheet.h"

#include <QAbstractScrollArea>
#include <QFont>
#include <QPointF>
#include <QSet>
#include <QTimer>
#include <QUndoStack>

#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

class QPainter;
class QTextLayout;

// The page view: math, text and plot regions placed freely on US Letter pages, Mathcad style.
// Click anywhere to move the red crosshair, then type to start a math region there, type
// '"' to start a text region, or '@' to insert an X-Y plot. Math is edited as a 2D tree
// (docs/editor-spec.md). Text wraps at a width set by dragging its right edge; a plot is
// resized by its bottom-right corner, and Tab moves between its placeholders.
// Drag on empty space or Shift+click to select regions, then drag, nudge or delete them.
// Every edit goes through undoStack(), which is clean while the worksheet is unmodified.
//
// Document coordinates are in 1/96 inch, with (0, 0) at the top-left of the first page.
class WorksheetCanvas : public QAbstractScrollArea {
    Q_OBJECT

public:
    static constexpr double kPageWidth = 816.0;  // 8.5 in
    static constexpr double kPageHeight = 1056.0; // 11 in
    static constexpr double kMargin = 72.0;       // 0.75 in
    static constexpr double kGrid = 12.0;         // 1/8 in
    static constexpr double kMinTextWidth = 48.0;
    static constexpr double kPlotWidth = 288.0; // 3 in × 2 in, as inserted
    static constexpr double kPlotHeight = 192.0;
    static constexpr double kMinPlotWidth = 96.0;
    static constexpr double kMinPlotHeight = 72.0;
    static constexpr double kMathSize = 16.0; // the math font's em: 12 pt

    // A plot's placeholders. The traces are the region's source.
    enum PlotField { Traces, XVariable, XMin, XMax, YMin, YMax, PlotFieldCount };

    explicit WorksheetCanvas(QWidget* parent = nullptr);

    lemma::Worksheet& worksheet() { return worksheet_; }
    QUndoStack* undoStack() { return &undoStack_; }

    // Call after replacing the worksheet's contents (new document, file loaded). Clears undo.
    void worksheetReset();

    // Where a document point currently appears in the viewport.
    QPointF viewportPosition(QPointF docPos) const { return docPos + pageOrigin(); }

    // The point on a region's baseline just before character `index` (UTF-16) of its source.
    // For math, `index` counts stops instead: the caret positions Left and Right step through.
    QPointF sourcePosition(const lemma::Region& r, int index) const;
    // The middle of the caret's bar at `caret` in a math region.
    QPointF mathCaretPosition(const lemma::Region& r, const lemma::MathCaret& caret) const;
    // A math region as drawn, relative to the left end of its baseline.
    const lemma::MathLayout& mathLayout(const lemma::Region& r) const;
    // The caret in the math region being edited, if one is.
    std::optional<lemma::MathCaret> mathCaret() const;
    // The region's outline, as hit-tested and selected. A text region being edited spans
    // its whole wrap width; otherwise it is as wide as its longest line.
    QRectF regionRect(const lemma::Region& r) const;
    // Where a plot draws its curves.
    QRectF plotFrame(const lemma::Region& r) const;
    // The point on a plot field's baseline just before character `index` of its text.
    QPointF plotFieldPosition(const lemma::Region& r, int field, int index) const;

    // Regions picked by rubber band, Shift+click or Ctrl+A. Never includes the one being edited.
    const QSet<int>& selection() const { return selection_; }
    void selectAll();

    // Pages that have content, for printing.
    int pageCount() const;
    void renderPage(QPainter& painter, int page) const;

signals:
    // Something to tell the user in passing, e.g. why a paste didn't work.
    void message(const QString& text);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    bool focusNextPrevChild(bool next) override;

private:
    class Change;

    // A region as it was at some point; absent if it didn't exist then.
    struct RegionState {
        int id = 0;
        std::optional<lemma::Region> region;
    };
    using RegionStates = std::vector<RegionState>;

    // Typing and erasing in one region merge into a single undo step.
    enum class EditKind { Other, Typing, Erasing };

    // Where the caret is: an index into a text or plot field, or a place in a math tree.
    struct CaretState {
        CaretState(int i = 0) : index(i) {}
        CaretState(const lemma::MathCaret& m) : math(m) {}
        friend bool operator==(const CaretState&, const CaretState&) = default;

        int index = 0;
        lemma::MathCaret math;
    };

    RegionStates capture(const QSet<int>& ids) const;
    // Pushes an undo step from `before` to the current state of the same regions, if
    // anything changed. The change has already been made.
    void record(const QString& text, RegionStates before, EditKind kind = EditKind::Other,
                const CaretState& caretBefore = {});
    // Puts regions back as they were, for undo and redo.
    void restore(const RegionStates& states, int editId, const CaretState& caret, int field);
    CaretState currentCaret() const;

    QPointF pageOrigin() const;
    QPointF toDocument(QPointF viewportPos) const;
    static QPointF snap(QPointF pos);

    void invalidateLayouts();
    // Whether a math region shows its error. An unfilled placeholder isn't one while it is
    // still being typed.
    bool showsError(const lemma::Region& r) const;
    // Lays out a text region in document coordinates, wrapped at its width.
    void layoutText(QTextLayout& layout, const lemma::Region& r) const;

    // The text a field of a region holds: a plot's placeholder, or any region's source.
    static std::string& fieldSource(lemma::Region& r, int field);
    static const std::string& fieldSource(const lemma::Region& r, int field);
    // What a plot field shows: its text, or the fitted limit (`fitted` is set) for an empty y limit.
    QString plotFieldText(const lemma::Region& r, int field, bool* fitted = nullptr) const;
    QRectF plotFieldRect(const lemma::Region& r, int field) const;
    int plotFieldAt(const lemma::Region& r, QPointF docPos) const;

    int regionAt(QPointF docPos) const;
    // The text region whose right edge (the wrap width handle) is at docPos, or the plot
    // whose bottom-right corner is.
    int resizeHandleAt(QPointF docPos) const;
    int caretIndexAt(const lemma::Region& r, QPointF docPos) const;
    // Where Up, Down, Home or End moves the caret in a text region: by lines, not the whole region.
    int textCaretFor(const lemma::Region& r, int key) const;
    void updateCursor(QPointF viewportPos);

    void paintRegions(QPainter& painter, bool onScreen) const;
    void paintMathRegion(QPainter& painter, const lemma::Region& r, bool onScreen) const;
    void paintTextRegion(QPainter& painter, const lemma::Region& r, bool onScreen) const;
    void paintPlotRegion(QPainter& painter, const lemma::Region& r, bool onScreen) const;

    void setSelection(const QSet<int>& ids);
    void deleteSelection();
    // Moves the selected regions together. The group stops as a whole at the page's
    // top and left edges, so its layout is kept.
    void moveSelection(QPointF delta);

    void beginEditing(int id, const CaretState& caret, int field = Traces);
    void endEditing();
    void insertText(const QString& text, bool created = false);
    void eraseAtCaret(bool before);
    void moveCaret(int position);
    void caretMoved();
    // Runs an editing command on the math region being edited, and records it for undo.
    // `created` means the region was made for this command.
    lemma::MathEditor::Change editMath(const std::function<lemma::MathEditor::Change(lemma::MathEditor&)>& command,
                                       bool created = false);
    // Starts a math region at the crosshair with a command, or not at all if it does nothing.
    void startMath(const std::function<lemma::MathEditor::Change(lemma::MathEditor&)>& command);
    void copyMath(bool cut);
    std::function<lemma::MathEditor::Change(lemma::MathEditor&)> pasteCommand();
    void contentChanged();
    void updateScrollBars();

    lemma::Worksheet worksheet_;
    QFont font_;
    QFont errorFont_;
    QFont textFont_;

    QPointF crosshair_{kMargin, kMargin + kGrid * 2};

    int editingId_ = 0;
    int editingField_ = Traces; // which of a plot's placeholders; always Traces for other regions
    int caret_ = 0; // UTF-16 index into the field's text
    std::optional<lemma::MathEditor> math_; // while editing a math region
    mutable std::unordered_map<int, lemma::MathLayout> layouts_; // math regions as drawn, by id
    bool caretVisible_ = true;
    QTimer caretBlink_;
    int editSession_ = 0; // bumped on each beginEditing, so separate edits don't merge
    // A text region started with '"' and still empty. It isn't on the undo stack until its
    // first character, so it goes away without a trace if left empty.
    int freshId_ = 0;

    QSet<int> selection_;

    int pressedId_ = 0;
    QPointF pressPos_;
    QPointF pressRegionPos_;
    int pressField_ = -1; // the plot placeholder pressed, if any
    bool dragging_ = false;
    RegionStates dragBefore_;
    bool selectingMath_ = false; // dragging across the math being edited, to select

    // Dragging a text region's right edge to change its wrap width, or a plot's corner.
    int resizingId_ = 0;
    RegionStates resizeBefore_;

    // Rubber-band selection, from a press on empty page space.
    bool bandPressed_ = false;
    bool banding_ = false;
    QRectF band_;
    QSet<int> bandBase_; // selection kept under the band (Shift held)

    QUndoStack undoStack_;
};
