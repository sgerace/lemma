#pragma once

#include "lemma/worksheet.h"

#include <QAbstractScrollArea>
#include <QFont>
#include <QPointF>
#include <QTimer>

class QPainter;

// The page view: math regions placed freely on US Letter pages, Mathcad style.
// Click anywhere to move the red crosshair, then type to start a region there.
//
// Document coordinates are in 1/96 inch, with (0, 0) at the top-left of the first page.
class WorksheetCanvas : public QAbstractScrollArea {
    Q_OBJECT

public:
    static constexpr double kPageWidth = 816.0;  // 8.5 in
    static constexpr double kPageHeight = 1056.0; // 11 in
    static constexpr double kMargin = 72.0;       // 0.75 in
    static constexpr double kGrid = 12.0;         // 1/8 in

    explicit WorksheetCanvas(QWidget* parent = nullptr);

    lemma::Worksheet& worksheet() { return worksheet_; }

    // Call after replacing the worksheet's contents (new document, file loaded).
    void worksheetReset();

    // Where a document point currently appears in the viewport.
    QPointF viewportPosition(QPointF docPos) const { return docPos + pageOrigin(); }

    // The point on a region's baseline just before character `index` (UTF-16) of its source.
    QPointF sourcePosition(const lemma::Region& r, int index) const;

    // Pages that have content, for printing.
    int pageCount() const;
    void renderPage(QPainter& painter, int page) const;

signals:
    void modified();

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
    QPointF pageOrigin() const;
    QPointF toDocument(QPointF viewportPos) const;
    static QPointF snap(QPointF pos);

    // What a region shows: its source while being edited, its result otherwise.
    QString shownText(const lemma::Region& r) const;
    QString hintText(const lemma::Region& r) const;
    QRectF regionRect(const lemma::Region& r) const;
    int regionAt(QPointF docPos) const;
    int caretIndexAt(const lemma::Region& r, double docX) const;

    void paintRegions(QPainter& painter, bool onScreen) const;

    void beginEditing(int id, int caret);
    void endEditing();
    void insertText(const QString& text);
    void eraseAtCaret(bool before);
    void moveCaret(int position);
    void contentChanged();
    void updateScrollBars();

    lemma::Worksheet worksheet_;
    QFont font_;
    QFont errorFont_;

    QPointF crosshair_{kMargin, kMargin + kGrid * 2};

    int editingId_ = 0;
    int caret_ = 0; // UTF-16 index into the region's source
    bool caretVisible_ = true;
    QTimer caretBlink_;

    int pressedId_ = 0;
    QPointF pressPos_;
    QPointF pressRegionPos_;
    bool dragging_ = false;
};
