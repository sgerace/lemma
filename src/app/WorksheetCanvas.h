#pragma once

#include "lemma/worksheet.h"

#include <QAbstractScrollArea>
#include <QFont>
#include <QPointF>
#include <QSet>
#include <QTimer>
#include <QUndoStack>

#include <optional>
#include <vector>

class QPainter;
class QTextLayout;

// The page view: math and text regions placed freely on US Letter pages, Mathcad style.
// Click anywhere to move the red crosshair, then type to start a math region there, or
// type '"' to start a text region. Text wraps at a width set by dragging its right edge.
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

    explicit WorksheetCanvas(QWidget* parent = nullptr);

    lemma::Worksheet& worksheet() { return worksheet_; }
    QUndoStack* undoStack() { return &undoStack_; }

    // Call after replacing the worksheet's contents (new document, file loaded). Clears undo.
    void worksheetReset();

    // Where a document point currently appears in the viewport.
    QPointF viewportPosition(QPointF docPos) const { return docPos + pageOrigin(); }

    // The point on a region's baseline just before character `index` (UTF-16) of its source.
    QPointF sourcePosition(const lemma::Region& r, int index) const;
    // The region's outline, as hit-tested and selected. A text region being edited spans
    // its whole wrap width; otherwise it is as wide as its longest line.
    QRectF regionRect(const lemma::Region& r) const;

    // Regions picked by rubber band, Shift+click or Ctrl+A. Never includes the one being edited.
    const QSet<int>& selection() const { return selection_; }
    void selectAll();

    // Pages that have content, for printing.
    int pageCount() const;
    void renderPage(QPainter& painter, int page) const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;

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

    RegionStates capture(const QSet<int>& ids) const;
    // Pushes an undo step from `before` to the current state of the same regions, if
    // anything changed. The change has already been made.
    void record(const QString& text, RegionStates before, EditKind kind = EditKind::Other, int caretBefore = 0);
    // Puts regions back as they were, for undo and redo.
    void restore(const RegionStates& states, int editId, int caret);

    QPointF pageOrigin() const;
    QPointF toDocument(QPointF viewportPos) const;
    static QPointF snap(QPointF pos);

    // What a region shows: its source while being edited, its result otherwise.
    QString shownText(const lemma::Region& r) const;
    QString hintText(const lemma::Region& r) const;
    // Lays out a text region in document coordinates, wrapped at its width.
    void layoutText(QTextLayout& layout, const lemma::Region& r) const;
    int regionAt(QPointF docPos) const;
    // The text region whose right edge (the wrap width handle) is at docPos.
    int resizeHandleAt(QPointF docPos) const;
    int caretIndexAt(const lemma::Region& r, QPointF docPos) const;
    // Where Up, Down, Home or End moves the caret in a text region: by lines, not the whole region.
    int textCaretFor(const lemma::Region& r, int key) const;
    void updateCursor(QPointF viewportPos);

    void paintRegions(QPainter& painter, bool onScreen) const;
    void paintTextRegion(QPainter& painter, const lemma::Region& r, bool onScreen) const;

    void setSelection(const QSet<int>& ids);
    void deleteSelection();
    // Moves the selected regions together. The group stops as a whole at the page's
    // top and left edges, so its layout is kept.
    void moveSelection(QPointF delta);

    void beginEditing(int id, int caret);
    void endEditing();
    void insertText(const QString& text, bool created = false);
    void eraseAtCaret(bool before);
    void moveCaret(int position);
    void contentChanged();
    void updateScrollBars();

    lemma::Worksheet worksheet_;
    QFont font_;
    QFont errorFont_;
    QFont textFont_;

    QPointF crosshair_{kMargin, kMargin + kGrid * 2};

    int editingId_ = 0;
    int caret_ = 0; // UTF-16 index into the region's source
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
    bool dragging_ = false;
    RegionStates dragBefore_;

    // Dragging a text region's right edge to change its wrap width.
    int resizingId_ = 0;
    RegionStates resizeBefore_;

    // Rubber-band selection, from a press on empty page space.
    bool bandPressed_ = false;
    bool banding_ = false;
    QRectF band_;
    QSet<int> bandBase_; // selection kept under the band (Shift held)

    QUndoStack undoStack_;
};
