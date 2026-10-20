#include "WorksheetCanvas.h"

#include "MathRender.h"

#include "lemma/error.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QScrollBar>
#include <QTextLayout>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kGutter = 24.0; // space around the page on screen

const QColor kDeskColor(0xe4, 0xe6, 0xea);
const QColor kTextColor(0x1a, 0x1a, 0x1a);
const QColor kHintColor(0x80, 0x88, 0x94);
const QColor kErrorColor(0xc6, 0x28, 0x28);
const QColor kEditBoxColor(0x5b, 0x8d, 0xd9);
const QColor kGuideColor(0xc8, 0xcc, 0xd2);
const QColor kSelectionFill(0x5b, 0x8d, 0xd9, 0x30);
const QColor kPlotFrameColor(0x60, 0x66, 0x70);
const QColor kPlotGridColor(0xe8, 0xea, 0xee);
const QColor kPlotAxisColor(0xb0, 0xb4, 0xba);
const QColor kTraceColors[] = {{0x1f, 0x5f, 0xbf}, {0xc6, 0x28, 0x28}, {0x2e, 0x8b, 0x3a}, {0x8e, 0x44, 0xad}, {0xd9, 0x7a, 0x00}};

constexpr double kPlotGap = 6.0; // between a plot's frame and its placeholders

QString fromUtf8(const std::string& s)
{
    return QString::fromStdString(s);
}

} // namespace

// ---------------------------------------------------------------------------
// Undo

namespace {

constexpr int kEditMergeId = 1;

bool sameState(const std::optional<lemma::Region>& a, const std::optional<lemma::Region>& b)
{
    if (!a || !b) {
        return !a && !b;
    }
    return a->kind == b->kind && a->x == b->x && a->y == b->y && a->width == b->width && a->height == b->height
           && a->source == b->source && a->axes == b->axes && a->math.sameAs(b->math);
}

} // namespace

// One undo step: the regions an edit touched, before and after. A region that is absent
// on one side was created or removed, so this one command covers every kind of edit.
class WorksheetCanvas::Change : public QUndoCommand {
public:
    Change(WorksheetCanvas* canvas, const QString& text, EditKind kind, RegionStates before, RegionStates after,
           int session, int field, CaretState caretBefore, CaretState caretAfter)
        : QUndoCommand(text)
        , canvas_(canvas)
        , kind_(kind)
        , before_(std::move(before))
        , after_(std::move(after))
        , session_(session)
        , field_(field)
        , caretBefore_(caretBefore)
        , caretAfter_(caretAfter)
    {
    }

    int id() const override { return kind_ == EditKind::Typing || kind_ == EditKind::Erasing ? kEditMergeId : -1; }

    void undo() override { canvas_->restore(before_, editId(), caretBefore_, field_); }

    void redo() override
    {
        // The canvas made the change before pushing it.
        if (pushed_) {
            pushed_ = false;
            return;
        }
        canvas_->restore(after_, editId(), caretAfter_, field_);
    }

    bool mergeWith(const QUndoCommand* other) override
    {
        // Typing and erasing are always about a single region.
        const auto* next = static_cast<const Change*>(other);
        if (next->session_ != session_ || next->field_ != field_ || next->before_.front().id != before_.front().id) {
            return false;
        }
        // Removing a region emptied by erasing folds into the erase, so undo brings back its text.
        const bool removal = !next->after_.front().region;
        if (!removal && (next->kind_ != kind_ || !(next->caretBefore_ == caretAfter_))) {
            return false;
        }
        after_ = next->after_;
        caretAfter_ = next->caretAfter_;
        setObsolete(sameState(before_.front().region, after_.front().region)); // e.g. typed a space, then left
        return true;
    }

private:
    // Undoing an edit of a region's contents puts the caret back in the region.
    int editId() const { return kind_ == EditKind::Other ? 0 : before_.front().id; }

    WorksheetCanvas* canvas_;
    EditKind kind_;
    RegionStates before_;
    RegionStates after_;
    int session_;
    int field_; // the plot placeholder typed in
    CaretState caretBefore_;
    CaretState caretAfter_;
    bool pushed_ = true;
};

WorksheetCanvas::RegionStates WorksheetCanvas::capture(const QSet<int>& ids) const
{
    RegionStates states;
    for (const int id : ids) {
        const lemma::Region* r = worksheet_.region(id);
        states.push_back({id, r ? std::optional(*r) : std::nullopt});
    }
    return states;
}

void WorksheetCanvas::record(const QString& text, RegionStates before, EditKind kind, const CaretState& caretBefore)
{
    RegionStates after;
    bool changed = false;
    for (const RegionState& s : before) {
        const lemma::Region* r = worksheet_.region(s.id);
        after.push_back({s.id, r ? std::optional(*r) : std::nullopt});
        changed = changed || !sameState(s.region, after.back().region);
    }
    if (changed) {
        undoStack_.push(new Change(this, text, kind, std::move(before), std::move(after), editSession_, editingField_,
                                   caretBefore, currentCaret()));
    }
}

void WorksheetCanvas::restore(const RegionStates& states, int editId, const CaretState& caret, int field)
{
    // Drop any edit or drag as is. Ending an edit normally can push a command, which
    // mustn't happen from inside undo, and these states are already on the stack.
    if (freshId_) {
        worksheet_.removeRegion(freshId_); // never recorded, so nothing would bring it back or take it away
        freshId_ = 0;
    }
    editingId_ = 0;
    math_.reset();
    caretBlink_.stop();
    pressedId_ = resizingId_ = 0;
    dragging_ = bandPressed_ = banding_ = selectingMath_ = false;
    updateCursor(viewport()->mapFromGlobal(QCursor::pos()));

    QSet<int> present;
    for (const RegionState& s : states) {
        if (!s.region) {
            worksheet_.removeRegion(s.id);
        } else if (lemma::Region* r = worksheet_.region(s.id)) {
            *r = *s.region;
            present.insert(s.id);
        } else {
            worksheet_.restoreRegion(*s.region);
            present.insert(s.id);
        }
    }
    selection_.clear();
    contentChanged();

    // Show what changed: the caret for text, a selection for regions.
    if (editId && worksheet_.region(editId)) {
        beginEditing(editId, caret, field);
    } else {
        setSelection(present);
    }
}

WorksheetCanvas::WorksheetCanvas(QWidget* parent)
    : QAbstractScrollArea(parent)
{
    font_.setFamilies({"Cambria", "STIX Two Text", "Times New Roman"});
    font_.setPointSizeF(12);
    errorFont_ = font_;
    errorFont_.setPointSizeF(9);
    errorFont_.setItalic(true);
    textFont_.setFamilies({"Segoe UI", "Helvetica Neue", "Arial"});
    textFont_.setPointSizeF(11);

    viewport()->setMouseTracking(true); // to pick the cursor on hover
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled);

    caretBlink_.setInterval(QApplication::cursorFlashTime() / 2);
    connect(&caretBlink_, &QTimer::timeout, this, [this] {
        caretVisible_ = !caretVisible_;
        viewport()->update();
    });

    verticalScrollBar()->setSingleStep(20);
    horizontalScrollBar()->setSingleStep(20);
    updateScrollBars();
}

WorksheetCanvas::CaretState WorksheetCanvas::currentCaret() const
{
    return math_ ? CaretState(math_->caret()) : CaretState(caret_);
}

void WorksheetCanvas::worksheetReset()
{
    editingId_ = freshId_ = 0;
    math_.reset();
    caretBlink_.stop();
    pressedId_ = resizingId_ = 0;
    bandPressed_ = banding_ = false;
    selection_.clear();
    undoStack_.clear();
    crosshair_ = {kMargin, kMargin + kGrid * 2};
    worksheet_.evaluate();
    invalidateLayouts();
    updateScrollBars();
    verticalScrollBar()->setValue(0);
    viewport()->update();
}

// ---------------------------------------------------------------------------
// Geometry

QPointF WorksheetCanvas::pageOrigin() const
{
    const double x = std::max(kGutter, (viewport()->width() - kPageWidth) / 2.0);
    return {x - horizontalScrollBar()->value(), kGutter - verticalScrollBar()->value()};
}

QPointF WorksheetCanvas::toDocument(QPointF viewportPos) const
{
    return viewportPos - pageOrigin();
}

QPointF WorksheetCanvas::snap(QPointF pos)
{
    return {std::round(pos.x() / kGrid) * kGrid, std::round(pos.y() / kGrid) * kGrid};
}

void WorksheetCanvas::invalidateLayouts()
{
    layouts_.clear();
}

const lemma::MathLayout& WorksheetCanvas::mathLayout(const lemma::Region& r) const
{
    auto it = layouts_.find(r.id);
    if (it == layouts_.end()) {
        lemma::MathLayoutOptions options;
        options.size = kMathSize;
        options.upright = r.result.units;
        if (r.result.status == lemma::RegionResult::Status::Ok && r.result.kind == lemma::Statement::Kind::Evaluation) {
            options.resultNumber = r.result.number;
            options.resultUnit = r.result.unit ? &*r.result.unit : nullptr;
        }
        options.showEmptyUnit = r.id == editingId_;
        it = layouts_.emplace(r.id, lemma::layoutMath(r.math, MathFont::instance(), options)).first;
    }
    return it->second;
}

bool WorksheetCanvas::showsError(const lemma::Region& r) const
{
    if (r.result.status != lemma::RegionResult::Status::Error) {
        return false;
    }
    const lemma::MathNode* n = lemma::findMathNode(r.math, r.result.errorNode);
    const bool incomplete = n && (n->kind == lemma::MathNode::Kind::Placeholder
                                  || (n->kind == lemma::MathNode::Kind::Binary && n->op == 0));
    return !(incomplete && r.id == editingId_);
}

std::optional<lemma::MathCaret> WorksheetCanvas::mathCaret() const
{
    return math_ ? std::optional(math_->caret()) : std::nullopt;
}

QPointF WorksheetCanvas::mathCaretPosition(const lemma::Region& r, const lemma::MathCaret& caret) const
{
    const lemma::MathCaretGeometry g = mathLayout(r).caretGeometry(caret);
    return {r.x + g.x, r.y + (g.top + g.bottom) / 2};
}

void WorksheetCanvas::layoutText(QTextLayout& layout, const lemma::Region& r) const
{
    QString text = fromUtf8(r.source);
    text.replace(QLatin1Char('\n'), QChar::LineSeparator); // same length, so caret indices still match
    layout.setText(text);
    layout.setFont(textFont_);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);

    // The region's (x, y) is the first line's baseline, as for math.
    layout.beginLayout();
    double top = 0.0;
    for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
        line.setLineWidth(r.width);
        if (line.lineNumber() == 0) {
            top = r.y - line.ascent();
        }
        line.setPosition({r.x, top});
        top += line.height();
    }
    layout.endLayout();
}

QRectF WorksheetCanvas::regionRect(const lemma::Region& r) const
{
    if (r.kind == lemma::Region::Kind::Text) {
        QTextLayout layout;
        layoutText(layout, r);
        const QTextLine first = layout.lineAt(0);
        const QTextLine last = layout.lineAt(layout.lineCount() - 1);
        double width = r.id == editingId_ ? r.width : 4.0;
        for (int i = 0; i < layout.lineCount(); ++i) {
            width = std::max(width, layout.lineAt(i).naturalTextWidth());
        }
        return QRectF(r.x, first.y(), width, last.y() + last.height() - first.y());
    }
    if (r.kind == lemma::Region::Kind::Plot) {
        QRectF rect = plotFrame(r);
        for (int field = 0; field < PlotFieldCount; ++field) {
            rect = rect.united(plotFieldRect(r, field));
        }
        if (r.result.status == lemma::RegionResult::Status::Error) {
            const QFontMetricsF efm(errorFont_);
            rect.setWidth(std::max(rect.width(), efm.horizontalAdvance(fromUtf8(r.result.error))));
            rect.setBottom(rect.bottom() + efm.height() + 2);
        }
        return rect;
    }

    const lemma::MathLayout& layout = mathLayout(r);
    QRectF rect(r.x, r.y - layout.ascent, std::max(layout.width, 4.0), layout.ascent + layout.descent);
    if (showsError(r)) {
        const QFontMetricsF efm(errorFont_);
        rect.setWidth(std::max(rect.width(), efm.horizontalAdvance(fromUtf8(r.result.error))));
        rect.setHeight(rect.height() + efm.height() + 4);
    }
    return rect;
}

std::string& WorksheetCanvas::fieldSource(lemma::Region& r, int field)
{
    if (r.kind != lemma::Region::Kind::Plot) {
        return r.source;
    }
    switch (field) {
    case XVariable: return r.axes.x;
    case XMin: return r.axes.xMin;
    case XMax: return r.axes.xMax;
    case YMin: return r.axes.yMin;
    case YMax: return r.axes.yMax;
    default: return r.source;
    }
}

const std::string& WorksheetCanvas::fieldSource(const lemma::Region& r, int field)
{
    return fieldSource(const_cast<lemma::Region&>(r), field);
}

QRectF WorksheetCanvas::plotFrame(const lemma::Region& r) const
{
    // The traces sit on the region's baseline, above the frame, like a title.
    return QRectF(r.x, r.y + QFontMetricsF(font_).descent() + kPlotGap, r.width, r.height);
}

QString WorksheetCanvas::plotFieldText(const lemma::Region& r, int field, bool* fitted) const
{
    const QString text = fromUtf8(fieldSource(r, field));
    const bool isFitted = (field == YMin || field == YMax) && text.trimmed().isEmpty()
                          && !(r.id == editingId_ && field == editingField_)
                          && r.result.status == lemma::RegionResult::Status::Ok;
    if (fitted) {
        *fitted = isFitted;
    }
    if (isFitted) {
        const lemma::PlotData& plot = r.result.plot;
        return fromUtf8(lemma::formatQuantity({field == YMin ? plot.yMin : plot.yMax, plot.yDim}, worksheet_.format));
    }
    return text;
}

QPointF WorksheetCanvas::plotFieldPosition(const lemma::Region& r, int field, int index) const
{
    const QFontMetricsF fm(font_);
    const QRectF frame = plotFrame(r);
    const QString text = plotFieldText(r, field);
    // An empty placeholder still needs room to click and to show the caret.
    const double width = text.isEmpty() ? fm.averageCharWidth() : fm.horizontalAdvance(text);
    const double bottomBaseline = frame.bottom() + kPlotGap + fm.ascent();
    QPointF start;
    switch (field) {
    case Traces: start = {r.x, r.y}; break;
    case XVariable: start = {frame.center().x() - width / 2, bottomBaseline}; break;
    case XMin: start = {frame.left(), bottomBaseline}; break;
    case XMax: start = {frame.right() - width, bottomBaseline}; break;
    case YMin: start = {frame.left() - kPlotGap - width, frame.bottom()}; break;
    case YMax: start = {frame.left() - kPlotGap - width, frame.top() + fm.ascent()}; break;
    default: break;
    }
    return start + QPointF(fm.horizontalAdvance(text.left(index)), 0);
}

QRectF WorksheetCanvas::plotFieldRect(const lemma::Region& r, int field) const
{
    const QFontMetricsF fm(font_);
    const QString text = plotFieldText(r, field);
    const QPointF start = plotFieldPosition(r, field, 0);
    const double width = text.isEmpty() ? fm.averageCharWidth() : fm.horizontalAdvance(text);
    return QRectF(start.x(), start.y() - fm.ascent(), width, fm.ascent() + fm.descent());
}

int WorksheetCanvas::plotFieldAt(const lemma::Region& r, QPointF docPos) const
{
    for (int field = 0; field < PlotFieldCount; ++field) {
        if (plotFieldRect(r, field).adjusted(-3, -2, 3, 2).contains(docPos)) {
            return field;
        }
    }
    return -1;
}

int WorksheetCanvas::regionAt(QPointF docPos) const
{
    // Later regions are drawn on top, so they win.
    const auto& regions = worksheet_.regions();
    for (auto it = regions.rbegin(); it != regions.rend(); ++it) {
        if (regionRect(*it).adjusted(-3, -3, 3, 3).contains(docPos)) {
            return it->id;
        }
    }
    return 0;
}

int WorksheetCanvas::resizeHandleAt(QPointF docPos) const
{
    const auto& regions = worksheet_.regions();
    for (auto it = regions.rbegin(); it != regions.rend(); ++it) {
        if (it->kind == lemma::Region::Kind::Plot) {
            const QPointF corner = plotFrame(*it).bottomRight();
            if (std::abs(docPos.x() - corner.x()) <= 5 && std::abs(docPos.y() - corner.y()) <= 5) {
                return it->id;
            }
            continue;
        }
        if (it->kind != lemma::Region::Kind::Text) {
            continue;
        }
        // The edge of the box drawn around the region, give or take a few pixels.
        const QRectF rect = regionRect(*it).adjusted(-3, -2, 3, 2);
        if (std::abs(docPos.x() - rect.right()) <= 4 && docPos.y() >= rect.top() && docPos.y() <= rect.bottom()) {
            return it->id;
        }
    }
    return 0;
}

QPointF WorksheetCanvas::sourcePosition(const lemma::Region& r, int index) const
{
    if (r.kind == lemma::Region::Kind::Text) {
        QTextLayout layout;
        layoutText(layout, r);
        const QTextLine line = layout.lineForTextPosition(index);
        return {line.cursorToX(index), line.y() + line.ascent()};
    }
    if (r.kind == lemma::Region::Kind::Plot) {
        return plotFieldPosition(r, Traces, index);
    }
    const std::vector<lemma::MathCaret> stops = lemma::mathStops(r.math);
    return mathCaretPosition(r, stops[static_cast<std::size_t>(std::clamp(index, 0, static_cast<int>(stops.size()) - 1))]);
}

int WorksheetCanvas::caretIndexAt(const lemma::Region& r, QPointF docPos) const
{
    if (r.kind == lemma::Region::Kind::Text) {
        QTextLayout layout;
        layoutText(layout, r);
        QTextLine line = layout.lineAt(layout.lineCount() - 1);
        for (int i = 0; i < layout.lineCount(); ++i) {
            if (docPos.y() < layout.lineAt(i).y() + layout.lineAt(i).height()) {
                line = layout.lineAt(i);
                break;
            }
        }
        return line.xToCursor(docPos.x());
    }
    if (r.kind == lemma::Region::Kind::Plot) {
        const int field = std::max(0, plotFieldAt(r, docPos));
        const int length = static_cast<int>(fromUtf8(fieldSource(r, field)).size());
        int best = 0;
        for (int i = 1; i <= length; ++i) {
            if (std::abs(docPos.x() - plotFieldPosition(r, field, i).x())
                < std::abs(docPos.x() - plotFieldPosition(r, field, best).x())) {
                best = i;
            }
        }
        return best;
    }
    return 0; // math regions place the caret with mathLayout().caretAt()
}

int WorksheetCanvas::textCaretFor(const lemma::Region& r, int key) const
{
    QTextLayout layout;
    layoutText(layout, r);
    const QString& text = layout.text();
    const QTextLine line = layout.lineForTextPosition(caret_);
    switch (key) {
    case Qt::Key_Home:
        return line.textStart();
    case Qt::Key_End: {
        // Stop before a line break, or the space a wrapped line ends with, so the caret stays on this line.
        int end = line.textStart() + line.textLength();
        if (end > line.textStart() && end < text.size()
            && (text.at(end - 1) == QChar::LineSeparator || text.at(end - 1).isSpace())) {
            --end;
        }
        return end;
    }
    case Qt::Key_Up:
    case Qt::Key_Down: {
        const int target = line.lineNumber() + (key == Qt::Key_Up ? -1 : 1);
        if (target < 0) {
            return 0;
        }
        if (target >= layout.lineCount()) {
            return static_cast<int>(text.size());
        }
        return layout.lineAt(target).xToCursor(line.cursorToX(caret_));
    }
    default:
        return caret_;
    }
}

int WorksheetCanvas::pageCount() const
{
    double bottom = 0.0;
    for (const lemma::Region& r : worksheet_.regions()) {
        bottom = std::max(bottom, regionRect(r).bottom());
    }
    return std::max(1, static_cast<int>(std::ceil(bottom / kPageHeight)));
}

void WorksheetCanvas::updateScrollBars()
{
    // Always leave an empty page below the content to click into.
    const double height = std::max(pageCount() + 1, static_cast<int>(crosshair_.y() / kPageHeight) + 2) * kPageHeight;
    const QSize view = viewport()->size();
    horizontalScrollBar()->setRange(0, std::max(0, static_cast<int>(kPageWidth + 2 * kGutter) - view.width()));
    horizontalScrollBar()->setPageStep(view.width());
    verticalScrollBar()->setRange(0, std::max(0, static_cast<int>(height + 2 * kGutter) - view.height()));
    verticalScrollBar()->setPageStep(view.height());
}

// ---------------------------------------------------------------------------
// Painting

void WorksheetCanvas::paintRegions(QPainter& painter, bool onScreen) const
{
    for (const lemma::Region& r : worksheet_.regions()) {
        switch (r.kind) {
        case lemma::Region::Kind::Text: paintTextRegion(painter, r, onScreen); break;
        case lemma::Region::Kind::Plot: paintPlotRegion(painter, r, onScreen); break;
        case lemma::Region::Kind::Math: paintMathRegion(painter, r, onScreen); break;
        }
    }
}

void WorksheetCanvas::paintMathRegion(QPainter& painter, const lemma::Region& r, bool onScreen) const
{
    const lemma::MathLayout& layout = mathLayout(r);
    const QPointF origin(r.x, r.y);
    paintMath(painter, layout, origin, kTextColor);

    if (showsError(r)) {
        // Underline what the error is about, or the whole region if it's about no one part.
        const lemma::MathNodeBox* box = layout.box(r.result.errorNode);
        const double x0 = r.x + (box ? box->left : 0.0);
        const double x1 = std::max(x0 + 6.0, r.x + (box ? box->right : layout.width));
        const double y = r.y + (box ? box->bottom : layout.descent) + 1.5;
        QPainterPath squiggle({x0, y});
        for (double x = x0; x < x1; x += 2.0) {
            squiggle.lineTo(std::min(x + 2.0, x1), y + ((static_cast<int>((x - x0) / 2.0) % 2) ? 0.0 : 1.5));
        }
        painter.setPen(QPen(kErrorColor, 0.8));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(squiggle);

        painter.setFont(errorFont_);
        const QFontMetricsF efm(errorFont_);
        painter.drawText(QPointF(r.x, r.y + layout.descent + 4 + efm.ascent()), fromUtf8(r.result.error));
    }

    if (onScreen && selection_.contains(r.id)) {
        painter.setPen(QPen(kEditBoxColor, 1.0));
        painter.setBrush(kSelectionFill);
        painter.drawRect(regionRect(r).adjusted(-3, -2, 3, 2));
    }

    if (onScreen && r.id == editingId_ && math_) {
        painter.setPen(QPen(kEditBoxColor, 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(regionRect(r).adjusted(-3, -2, 3, 2));

        // Mathcad's editing lines: the selection underlined, and the bar at its side.
        const lemma::MathCaret caret = math_->caret();
        const lemma::MathCaretGeometry bar = layout.caretGeometry(caret);
        double bottom = bar.bottom;
        if (caret.selected) {
            if (const lemma::MathNodeBox* box = layout.box(caret.node)) {
                bottom = std::max(bottom, box->bottom) + 2.0;
                painter.setPen(QPen(kEditBoxColor, 1.4));
                painter.drawLine(origin + QPointF(box->left, bottom), origin + QPointF(box->right, bottom));
            }
        }
        if (caretVisible_ || caret.selected) {
            painter.setPen(QPen(caret.selected ? kEditBoxColor : kTextColor, caret.selected ? 1.4 : 1.2));
            painter.drawLine(origin + QPointF(bar.x, bar.top), origin + QPointF(bar.x, bottom));
        }
    }
}

void WorksheetCanvas::paintTextRegion(QPainter& painter, const lemma::Region& r, bool onScreen) const
{
    QTextLayout layout;
    layoutText(layout, r);
    painter.setPen(kTextColor);
    layout.draw(&painter, {0, 0});

    if (onScreen && selection_.contains(r.id)) {
        painter.setPen(QPen(kEditBoxColor, 1.0));
        painter.setBrush(kSelectionFill);
        painter.drawRect(regionRect(r).adjusted(-3, -2, 3, 2));
    }

    if (onScreen && r.id == editingId_) {
        const QRectF box = regionRect(r).adjusted(-3, -2, 3, 2);
        painter.setPen(QPen(kEditBoxColor, 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(box);
        // A grip on the right edge, which sets the wrap width.
        painter.setBrush(kEditBoxColor);
        painter.drawRect(QRectF(box.right() - 2, box.center().y() - 4, 4, 8));
        if (caretVisible_) {
            const QTextLine line = layout.lineForTextPosition(caret_);
            const double x = line.cursorToX(caret_);
            painter.setPen(QPen(kTextColor, 1.2));
            painter.drawLine(QPointF(x, line.y()), QPointF(x, line.y() + line.height()));
        }
    }
}

void WorksheetCanvas::paintPlotRegion(QPainter& painter, const lemma::Region& r, bool onScreen) const
{
    const QFontMetricsF fm(font_);
    const QRectF frame = plotFrame(r);
    const bool ok = r.result.status == lemma::RegionResult::Status::Ok;
    const lemma::PlotData& plot = r.result.plot;
    const auto toFrame = [&](double x, double y) {
        return QPointF(frame.left() + (x - plot.xMin) / (plot.xMax - plot.xMin) * frame.width(),
                       frame.bottom() - (y - plot.yMin) / (plot.yMax - plot.yMin) * frame.height());
    };

    painter.save();
    painter.setClipRect(frame, Qt::IntersectClip);
    painter.setBrush(Qt::NoBrush);
    if (ok) {
        // Light grid lines at round values, and darker axes through zero.
        const double xStep = lemma::niceStep(plot.xMax - plot.xMin);
        const double yStep = lemma::niceStep(plot.yMax - plot.yMin);
        painter.setPen(QPen(kPlotGridColor, 1.0));
        for (double x = std::ceil(plot.xMin / xStep) * xStep; x <= plot.xMax; x += xStep) {
            painter.drawLine(toFrame(x, plot.yMin), toFrame(x, plot.yMax));
        }
        for (double y = std::ceil(plot.yMin / yStep) * yStep; y <= plot.yMax; y += yStep) {
            painter.drawLine(toFrame(plot.xMin, y), toFrame(plot.xMax, y));
        }
        painter.setPen(QPen(kPlotAxisColor, 1.0));
        if (plot.xMin < 0 && plot.xMax > 0) {
            painter.drawLine(toFrame(0, plot.yMin), toFrame(0, plot.yMax));
        }
        if (plot.yMin < 0 && plot.yMax > 0) {
            painter.drawLine(toFrame(plot.xMin, 0), toFrame(plot.xMax, 0));
        }

        for (std::size_t i = 0; i < plot.traces.size(); ++i) {
            painter.setPen(QPen(kTraceColors[i % std::size(kTraceColors)], 1.5));
            for (const auto& segment : plot.traces[i].segments) {
                QPolygonF line;
                for (const lemma::PlotPoint& p : segment) {
                    line << toFrame(p.x, p.y);
                }
                painter.drawPolyline(line);
            }
        }
    }
    painter.restore();
    painter.setPen(QPen(kPlotFrameColor, 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(frame);

    // The placeholders. Each trace is written in the color it is drawn in.
    painter.setFont(font_);
    const bool editing = onScreen && r.id == editingId_;
    for (int field = 0; field < PlotFieldCount; ++field) {
        bool fitted = false;
        const QString text = plotFieldText(r, field, &fitted);
        const QPointF start = plotFieldPosition(r, field, 0);
        if (text.isEmpty()) {
            if (onScreen) { // an empty placeholder, Mathcad's small black box
                painter.setPen(QPen(kTextColor, 1.0));
                painter.drawRect(QRectF(start.x() + 1, start.y() - fm.ascent() * 0.7, fm.averageCharWidth() - 2,
                                        fm.ascent() * 0.7));
            }
            continue;
        }
        painter.setPen(fitted ? kHintColor : kTextColor);
        painter.drawText(start, text);
        if (field == Traces && ok && !editing) {
            const std::string& src = r.source;
            for (std::size_t i = 0; i < plot.traces.size(); ++i) {
                const int from = static_cast<int>(fromUtf8(src.substr(0, plot.traces[i].begin)).size());
                const QString piece = fromUtf8(src.substr(plot.traces[i].begin, plot.traces[i].end - plot.traces[i].begin));
                painter.setPen(kTraceColors[i % std::size(kTraceColors)]);
                painter.drawText(plotFieldPosition(r, Traces, from), piece);
            }
        }
    }

    if (!ok) {
        painter.setPen(kErrorColor);
        painter.setFont(errorFont_);
        const QFontMetricsF efm(errorFont_);
        const double bottom = plotFieldRect(r, XMin).bottom();
        painter.drawText(QPointF(r.x, bottom + 2 + efm.ascent()), fromUtf8(r.result.error));
    }

    if (onScreen && selection_.contains(r.id)) {
        painter.setPen(QPen(kEditBoxColor, 1.0));
        painter.setBrush(kSelectionFill);
        painter.drawRect(regionRect(r).adjusted(-3, -2, 3, 2));
    }
    if (editing) {
        painter.setPen(QPen(kEditBoxColor, 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(plotFieldRect(r, editingField_).adjusted(-3, -2, 3, 2));
        // A grip on the frame's corner, which resizes the plot.
        painter.setBrush(kEditBoxColor);
        painter.drawRect(QRectF(frame.bottomRight() - QPointF(3, 3), QSizeF(6, 6)));
        if (caretVisible_) {
            const QPointF caret = plotFieldPosition(r, editingField_, caret_);
            painter.setPen(QPen(kTextColor, 1.2));
            painter.drawLine(caret - QPointF(0, fm.ascent()), caret + QPointF(0, fm.descent()));
        }
    }
}

void WorksheetCanvas::paintEvent(QPaintEvent*)
{
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(viewport()->rect(), kDeskColor);
    painter.translate(pageOrigin());

    const double height = verticalScrollBar()->maximum() + viewport()->height() - 2 * kGutter;
    const QRectF paper(0, 0, kPageWidth, height);
    painter.fillRect(paper.translated(2, 2), QColor(0, 0, 0, 30));
    painter.fillRect(paper, Qt::white);

    // Page breaks and the right margin, like Mathcad's dashed guides.
    painter.setPen(QPen(kGuideColor, 1.0, Qt::DashLine));
    for (double y = kPageHeight; y < height; y += kPageHeight) {
        painter.drawLine(QPointF(0, y), QPointF(kPageWidth, y));
    }
    painter.drawLine(QPointF(kPageWidth - kMargin, 0), QPointF(kPageWidth - kMargin, height));

    paintRegions(painter, true);

    if (banding_) {
        painter.setPen(QPen(kEditBoxColor, 1.0, Qt::DashLine));
        painter.setBrush(kSelectionFill);
        painter.drawRect(band_);
    }

    if (!editingId_ && hasFocus()) {
        painter.setPen(QPen(kErrorColor, 1.0));
        const QPointF c = crosshair_;
        painter.drawLine(c - QPointF(6, 0), c + QPointF(6, 0));
        painter.drawLine(c - QPointF(0, 6), c + QPointF(0, 6));
    }
}

void WorksheetCanvas::renderPage(QPainter& painter, int page) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setClipRect(QRectF(0, 0, kPageWidth, kPageHeight));
    painter.translate(0, -page * kPageHeight);
    paintRegions(painter, false);
    painter.restore();
}

// ---------------------------------------------------------------------------
// Selection

void WorksheetCanvas::setSelection(const QSet<int>& ids)
{
    if (ids != selection_) {
        selection_ = ids;
        viewport()->update();
    }
}

void WorksheetCanvas::selectAll()
{
    endEditing();
    QSet<int> all;
    for (const lemma::Region& r : worksheet_.regions()) {
        all.insert(r.id);
    }
    setSelection(all);
}

void WorksheetCanvas::deleteSelection()
{
    RegionStates before = capture(selection_);
    for (const int id : std::as_const(selection_)) {
        worksheet_.removeRegion(id);
    }
    selection_.clear();
    contentChanged();
    record(tr("Delete"), std::move(before));
}

void WorksheetCanvas::moveSelection(QPointF delta)
{
    double dx = delta.x();
    double dy = delta.y();
    for (const int id : std::as_const(selection_)) {
        const lemma::Region* r = worksheet_.region(id);
        // min() with 0 so a region already past the edge never forces a move.
        dx = std::max(dx, std::min(0.0, -r->x));
        dy = std::max(dy, std::min(0.0, kGrid - r->y));
    }
    if (dx == 0.0 && dy == 0.0) {
        return;
    }
    for (const int id : std::as_const(selection_)) {
        lemma::Region* r = worksheet_.region(id);
        r->x += dx;
        r->y += dy;
    }
    contentChanged(); // moving regions can change evaluation order
}

// ---------------------------------------------------------------------------
// Editing

void WorksheetCanvas::beginEditing(int id, const CaretState& caret, int field)
{
    if (editingId_ && editingId_ != id) {
        endEditing();
    }
    const lemma::Region* r = worksheet_.region(id);
    if (!r) {
        return;
    }
    setSelection({});
    editingId_ = id;
    editingField_ = r->kind == lemma::Region::Kind::Plot ? field : Traces;
    ++editSession_;
    if (r->kind == lemma::Region::Kind::Math) {
        math_.emplace(r->math, caret.math);
        invalidateLayouts(); // an evaluation shows its unit placeholder while edited
        caretMoved();
        return;
    }
    math_.reset();
    moveCaret(std::clamp(caret.index, 0, static_cast<int>(fromUtf8(fieldSource(*r, editingField_)).size())));
}

void WorksheetCanvas::endEditing()
{
    if (!editingId_) {
        return;
    }
    const lemma::Region* r = worksheet_.region(editingId_);
    // An emptied plot stays, with its placeholders to fill in, as in Mathcad.
    bool empty = !r;
    if (r && r->kind == lemma::Region::Kind::Math) {
        empty = r->math.kind == lemma::MathNode::Kind::Placeholder;
    } else if (r && r->kind == lemma::Region::Kind::Text) {
        empty = fromUtf8(r->source).trimmed().isEmpty();
    }
    const bool fresh = editingId_ == freshId_;
    const CaretState caret = currentCaret();
    freshId_ = 0;
    RegionStates before;
    if (empty) {
        before = capture({editingId_});
        worksheet_.removeRegion(editingId_);
    }
    editingId_ = 0;
    math_.reset();
    caretBlink_.stop();
    invalidateLayouts();
    if (empty) {
        contentChanged();
        if (!fresh) {
            record(tr("Delete"), std::move(before), EditKind::Erasing, caret);
        }
    } else {
        viewport()->update();
    }
}

void WorksheetCanvas::moveCaret(int position)
{
    caret_ = position;
    caretMoved();
}

void WorksheetCanvas::caretMoved()
{
    caretVisible_ = true;
    caretBlink_.start();
    viewport()->update();
}

lemma::MathEditor::Change WorksheetCanvas::editMath(
    const std::function<lemma::MathEditor::Change(lemma::MathEditor&)>& command, bool created)
{
    using Change = lemma::MathEditor::Change;
    RegionStates before = created ? RegionStates{{editingId_, std::nullopt}} : capture({editingId_});
    const CaretState caretBefore = currentCaret();
    const Change change = command(*math_);
    if (change == Change::None) {
        return change;
    }
    caretMoved();
    if (change == Change::Moved) {
        return change;
    }
    worksheet_.region(editingId_)->math = math_->root();
    contentChanged();
    const EditKind kind = change == Change::Typed ? EditKind::Typing
                          : change == Change::Erased ? EditKind::Erasing
                                                     : EditKind::Structure;
    record(kind == EditKind::Erasing ? tr("Delete") : tr("Typing"), std::move(before), kind, caretBefore);
    return change;
}

void WorksheetCanvas::startMath(const std::function<lemma::MathEditor::Change(lemma::MathEditor&)>& command)
{
    const int id = worksheet_.addRegion(crosshair_.x(), crosshair_.y());
    beginEditing(id, {});
    if (editMath(command, true) == lemma::MathEditor::Change::None) {
        // A key that does nothing on its own, like ")", leaves no region behind.
        editingId_ = 0;
        math_.reset();
        caretBlink_.stop();
        worksheet_.removeRegion(id);
        contentChanged();
    }
}

void WorksheetCanvas::copyMath(bool cut)
{
    const std::optional<std::string> text = math_->copy();
    if (!text) {
        emit message(tr("Finish this before copying it: fill in the placeholders and numbers."));
        return;
    }
    QGuiApplication::clipboard()->setText(fromUtf8(*text));
    if (cut) {
        editMath([](lemma::MathEditor& e) {
            if (!e.caret().selected) {
                e.setCaret({e.root().id, 0, lemma::MathCaret::Side::Right, true}); // all of it
            }
            return e.backspace();
        });
    }
}

std::function<lemma::MathEditor::Change(lemma::MathEditor&)> WorksheetCanvas::pasteCommand()
{
    // Pasting parses linear text; it doesn't replay it as keystrokes (section 10).
    const QString text = QGuiApplication::clipboard()->text().trimmed();
    std::optional<lemma::MathNode> tree;
    if (!text.isEmpty()) {
        try {
            tree = lemma::mathFromLinear(text.toStdString());
        } catch (const lemma::Error& e) {
            emit message(tr("Can't paste that as math: %1").arg(fromUtf8(e.what())));
        }
    }
    return [tree](lemma::MathEditor& e) { return tree ? e.paste(*tree) : lemma::MathEditor::Change::None; };
}

void WorksheetCanvas::insertText(const QString& text, bool created)
{
    // A region created for this text didn't exist before it, as far as undo is concerned.
    if (editingId_ == freshId_) {
        created = true;
        freshId_ = 0;
    }
    RegionStates before = created ? RegionStates{{editingId_, std::nullopt}} : capture({editingId_});
    const int caretBefore = caret_;
    lemma::Region* r = worksheet_.region(editingId_);
    std::string& field = fieldSource(*r, editingField_);
    QString source = fromUtf8(field);
    source.insert(caret_, text);
    field = source.toStdString();
    moveCaret(caret_ + static_cast<int>(text.size()));
    contentChanged();
    record(tr("Typing"), std::move(before), EditKind::Typing, caretBefore);
}

void WorksheetCanvas::eraseAtCaret(bool before)
{
    lemma::Region* r = worksheet_.region(editingId_);
    std::string& field = fieldSource(*r, editingField_);
    QString source = fromUtf8(field);
    int from = before ? caret_ - 1 : caret_;
    if (from < 0 || from >= source.size()) {
        return;
    }
    int count = 1;
    // Keep surrogate pairs (characters outside the BMP) together.
    if (before && from > 0 && source.at(from).isLowSurrogate()) {
        --from;
        ++count;
    } else if (!before && source.at(from).isHighSurrogate() && from + 1 < source.size()) {
        ++count;
    }
    RegionStates previous = capture({editingId_});
    const int caretBefore = caret_;
    source.remove(from, count);
    field = source.toStdString();
    moveCaret(from);
    contentChanged();
    record(tr("Delete"), std::move(previous), EditKind::Erasing, caretBefore);
}

void WorksheetCanvas::contentChanged()
{
    worksheet_.evaluate();
    invalidateLayouts();
    updateScrollBars();
    viewport()->update();
}

// ---------------------------------------------------------------------------
// Input

void WorksheetCanvas::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const QPointF pos = toDocument(event->position());
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    pressPos_ = pos;
    if (const int id = resizeHandleAt(pos)) {
        resizingId_ = id;
        resizeBefore_ = capture({id});
        return;
    }
    if (const int id = regionAt(pos)) {
        if (shift) {
            endEditing();
            if (worksheet_.region(id)) { // ending the edit removes an emptied region
                QSet<int> ids = selection_;
                if (!ids.remove(id)) {
                    ids.insert(id);
                }
                setSelection(ids);
            }
            return;
        }
        const lemma::Region* r = worksheet_.region(id);
        pressedId_ = id;
        pressRegionPos_ = {r->x, r->y};
        pressField_ = r->kind == lemma::Region::Kind::Plot ? plotFieldAt(*r, pos) : Traces;
        dragging_ = false;
        return;
    }
    endEditing();
    bandPressed_ = true;
    banding_ = false;
    bandBase_ = shift ? selection_ : QSet<int>{};
    if (!shift) {
        setSelection({});
    }
}

void WorksheetCanvas::updateCursor(QPointF viewportPos)
{
    // An I-beam where a click edits text, an arrow on empty page space.
    const QPointF pos = toDocument(viewportPos);
    if (const int id = resizeHandleAt(pos)) {
        const bool plot = worksheet_.region(id)->kind == lemma::Region::Kind::Plot;
        viewport()->setCursor(plot ? Qt::SizeFDiagCursor : Qt::SizeHorCursor);
    } else if (const int id = regionAt(pos)) {
        const lemma::Region* r = worksheet_.region(id);
        const bool text = r->kind != lemma::Region::Kind::Plot || plotFieldAt(*r, pos) >= 0;
        viewport()->setCursor(text ? Qt::IBeamCursor : Qt::ArrowCursor);
    } else {
        viewport()->setCursor(Qt::ArrowCursor);
    }
}

void WorksheetCanvas::mouseMoveEvent(QMouseEvent* event)
{
    if (!(event->buttons() & Qt::LeftButton)) {
        updateCursor(event->position());
        return;
    }
    const QPointF pos = toDocument(event->position());
    const bool pastThreshold = (pos - pressPos_).manhattanLength() >= QApplication::startDragDistance();

    if (resizingId_) {
        // The edge follows the mouse, snapped to the grid.
        lemma::Region* r = worksheet_.region(resizingId_);
        if (r->kind == lemma::Region::Kind::Plot) {
            const QRectF frame = plotFrame(*r);
            r->width = std::max(kMinPlotWidth, std::round((pos.x() - frame.left()) / kGrid) * kGrid);
            r->height = std::max(kMinPlotHeight, std::round((pos.y() - frame.top()) / kGrid) * kGrid);
        } else {
            r->width = std::max(kMinTextWidth, std::round((pos.x() - 3 - r->x) / kGrid) * kGrid);
        }
        contentChanged();
        return;
    }

    if (bandPressed_) {
        banding_ = banding_ || pastThreshold;
        if (banding_) {
            band_ = QRectF(pressPos_, pos).normalized();
            QSet<int> ids = bandBase_;
            for (const lemma::Region& r : worksheet_.regions()) {
                if (regionRect(r).intersects(band_)) {
                    ids.insert(r.id);
                }
            }
            setSelection(ids);
            viewport()->update();
        }
        return;
    }

    if (!pressedId_) {
        return;
    }
    if (selectingMath_ || (pastThreshold && !dragging_ && pressedId_ == editingId_ && math_)) {
        // Dragging across the math being edited selects the smallest subtree with both ends.
        selectingMath_ = true;
        const lemma::Region* r = worksheet_.region(editingId_);
        const lemma::MathLayout& layout = mathLayout(*r);
        const lemma::MathCaret from = layout.caretAt(r->math, pressPos_.x() - r->x, pressPos_.y() - r->y);
        const lemma::MathCaret to = layout.caretAt(r->math, pos.x() - r->x, pos.y() - r->y);
        std::vector<int> ancestors;
        for (const lemma::MathNode* n = lemma::findMathNode(r->math, from.node); n; n = lemma::parentOf(r->math, n->id)) {
            ancestors.push_back(n->id);
        }
        const lemma::MathNode* common = lemma::findMathNode(r->math, to.node);
        while (std::find(ancestors.begin(), ancestors.end(), common->id) == ancestors.end()) {
            common = lemma::parentOf(r->math, common->id);
        }
        const auto side = pos.x() >= pressPos_.x() ? lemma::MathCaret::Side::Right : lemma::MathCaret::Side::Left;
        math_->setCaret({common->id, 0, side, true});
        caretMoved();
        return;
    }
    if (!dragging_ && pastThreshold) {
        // Dragging a region outside the selection drags just that region.
        if (!selection_.contains(pressedId_)) {
            endEditing();
            if (!worksheet_.region(pressedId_)) {
                pressedId_ = 0; // it was empty, and ending the edit removed it
                return;
            }
            setSelection({pressedId_});
        }
        dragging_ = true;
        dragBefore_ = capture(selection_);
        viewport()->setCursor(Qt::SizeAllCursor);
    }
    if (dragging_) {
        // Snap the grabbed region to the grid and carry the rest of the selection along.
        const lemma::Region* r = worksheet_.region(pressedId_);
        moveSelection(snap(pressRegionPos_ + (pos - pressPos_)) - QPointF(r->x, r->y));
    }
}

void WorksheetCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }
    if (resizingId_) {
        if (resizingId_ != freshId_) { // a fresh region's width is recorded with its first character
            record(tr("Resize"), std::move(resizeBefore_));
        }
        resizingId_ = 0;
        updateCursor(event->position());
        return;
    }
    if (bandPressed_) {
        if (!banding_) {
            crosshair_ = snap(pressPos_); // a plain click on empty space
        }
        bandPressed_ = banding_ = false;
        viewport()->update();
        return;
    }
    if (!pressedId_) {
        return;
    }
    if (selectingMath_) {
        selectingMath_ = false;
    } else if (dragging_) {
        record(tr("Move"), std::move(dragBefore_));
    } else if (pressField_ < 0) {
        // A click on a plot's frame selects it, so it can be deleted or nudged.
        endEditing();
        if (worksheet_.region(pressedId_)) {
            setSelection({pressedId_});
        }
    } else if (const lemma::Region* r = worksheet_.region(pressedId_); r->kind == lemma::Region::Kind::Math) {
        beginEditing(pressedId_, mathLayout(*r).caretAt(r->math, pressPos_.x() - r->x, pressPos_.y() - r->y));
    } else {
        beginEditing(pressedId_, caretIndexAt(*r, pressPos_), pressField_);
    }
    pressedId_ = 0;
    updateCursor(event->position());
    dragging_ = false;
}

void WorksheetCanvas::mouseDoubleClickEvent(QMouseEvent* event)
{
    // In the math being edited, a double click selects the leaf or box under the pointer.
    const QPointF pos = toDocument(event->position());
    const lemma::Region* r = worksheet_.region(editingId_);
    if (event->button() == Qt::LeftButton && math_ && r && regionAt(pos) == editingId_) {
        math_->setCaret(mathLayout(*r).caretAt(r->math, pos.x() - r->x, pos.y() - r->y));
        math_->grow();
        caretMoved();
        return;
    }
    mousePressEvent(event);
}

void WorksheetCanvas::keyPressEvent(QKeyEvent* event)
{
    if (event->matches(QKeySequence::SelectAll)) {
        selectAll();
        return;
    }
    if (editingId_ && math_) {
        using Editor = lemma::MathEditor;
        if (event->matches(QKeySequence::Copy) || event->matches(QKeySequence::Cut)) {
            copyMath(event->matches(QKeySequence::Cut));
            return;
        }
        if (event->matches(QKeySequence::Paste)) {
            editMath(pasteCommand());
            return;
        }
        const lemma::Region* r = worksheet_.region(editingId_);
        const bool shift = event->modifiers() & Qt::ShiftModifier;
        switch (event->key()) {
        case Qt::Key_Left: editMath([](Editor& e) { return e.left(); }); return;
        case Qt::Key_Right: editMath([](Editor& e) { return e.right(); }); return;
        case Qt::Key_Home: editMath([](Editor& e) { return e.home(); }); return;
        case Qt::Key_End: editMath([](Editor& e) { return e.end(); }); return;
        case Qt::Key_Backspace: editMath([](Editor& e) { return e.backspace(); }); return;
        case Qt::Key_Delete: editMath([](Editor& e) { return e.deleteForward(); }); return;
        case Qt::Key_Insert: editMath([](Editor& e) { return e.flip(); }); return;
        case Qt::Key_Space: editMath([shift](Editor& e) { return shift ? e.shrink() : e.grow(); }); return;
        case Qt::Key_Tab:
        case Qt::Key_Backtab: {
            const bool back = event->key() == Qt::Key_Backtab || shift;
            editMath([back](Editor& e) { return e.tab(back); });
            return;
        }
        case Qt::Key_Up:
        case Qt::Key_Down:
            if (const auto next = mathLayout(*r).vertical(r->math, math_->caret(), event->key() == Qt::Key_Up)) {
                math_->setCaret(*next);
                caretMoved();
            }
            return;
        default:
            break;
        }
    }
    if (editingId_) {
        const lemma::Region* r = worksheet_.region(editingId_);
        const int length = static_cast<int>(fromUtf8(fieldSource(*r, editingField_)).size());
        const bool isText = r->kind == lemma::Region::Kind::Text;
        if (r->kind == lemma::Region::Kind::Plot && (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab)) {
            // Tab steps through the placeholders, as in Mathcad.
            const bool back = event->key() == Qt::Key_Backtab || (event->modifiers() & Qt::ShiftModifier);
            const int step = back ? PlotFieldCount - 1 : 1;
            const int field = (editingField_ + step) % PlotFieldCount;
            beginEditing(editingId_, static_cast<int>(fromUtf8(fieldSource(*r, field)).size()), field);
            return;
        }
        if (isText) {
            switch (event->key()) {
            case Qt::Key_Return:
            case Qt::Key_Enter:
                insertText(QStringLiteral("\n"));
                return;
            case Qt::Key_Up:
            case Qt::Key_Down:
            case Qt::Key_Home:
            case Qt::Key_End:
                moveCaret(textCaretFor(*r, event->key()));
                return;
            default:
                break;
            }
        }
        switch (event->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Escape: {
            // Like Mathcad, leave the region and put the crosshair below it.
            const QRectF rect = regionRect(*r);
            crosshair_ = snap({r->x, rect.bottom() + kGrid * 2});
            endEditing();
            updateScrollBars();
            return;
        }
        case Qt::Key_Left:
            moveCaret(std::max(0, caret_ - 1));
            return;
        case Qt::Key_Right:
            moveCaret(std::min(length, caret_ + 1));
            return;
        case Qt::Key_Home:
            moveCaret(0);
            return;
        case Qt::Key_End:
            moveCaret(length);
            return;
        case Qt::Key_Backspace:
            eraseAtCaret(true);
            return;
        case Qt::Key_Delete:
            eraseAtCaret(false);
            return;
        default:
            break;
        }
    } else {
        if (event->matches(QKeySequence::Paste)) {
            startMath(pasteCommand()); // a new region at the crosshair
            return;
        }
        if (!selection_.isEmpty()) {
            switch (event->key()) {
            case Qt::Key_Delete:
            case Qt::Key_Backspace:
                deleteSelection();
                return;
            case Qt::Key_Escape:
                setSelection({});
                return;
            default:
                break;
            }
        }
        QPointF step;
        switch (event->key()) {
        case Qt::Key_Left: step = {-kGrid, 0}; break;
        case Qt::Key_Right: step = {kGrid, 0}; break;
        case Qt::Key_Up: step = {0, -kGrid}; break;
        case Qt::Key_Down: step = {0, kGrid}; break;
        default: break;
        }
        if (!step.isNull() && !selection_.isEmpty()) {
            RegionStates before = capture(selection_);
            moveSelection(step); // nudge
            record(tr("Move"), std::move(before));
            return;
        }
        if (!step.isNull()) {
            crosshair_ = QPointF(std::max(0.0, crosshair_.x() + step.x()), std::max(kGrid, crosshair_.y() + step.y()));
            viewport()->update();
            return;
        }
    }

    QString text = event->text();
    const bool printable = !text.isEmpty() && std::all_of(text.begin(), text.end(), [](QChar c) { return c.isPrint(); });
    if (!printable || (event->modifiers() & Qt::ControlModifier)) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
    if (!editingId_ && text == QStringLiteral("\"")) {
        // Mathcad's shortcut for a text region. It wraps at the right margin until resized.
        const double width = std::max(kMinTextWidth, kPageWidth - kMargin - crosshair_.x());
        const int id = worksheet_.addTextRegion(crosshair_.x(), crosshair_.y(), width);
        beginEditing(id, 0);
        freshId_ = id;
        contentChanged();
        return;
    }
    if (!editingId_ && text == QStringLiteral("@")) {
        // Mathcad's shortcut for an X-Y plot. Its traces start out empty, ready to type.
        const int id = worksheet_.addPlotRegion(crosshair_.x(), crosshair_.y(), kPlotWidth, kPlotHeight);
        contentChanged();
        record(tr("Insert Plot"), {{id, std::nullopt}});
        beginEditing(id, 0, Traces);
        return;
    }
    // Math is typed into the tree a character at a time; ":" makes a definition, as in Mathcad.
    const auto typeMath = [text](lemma::MathEditor& e) {
        using Change = lemma::MathEditor::Change;
        Change result = Change::None;
        for (const char32_t c : text.toUcs4()) {
            const Change change = e.type(QString::fromUcs4(&c, 1).toStdString());
            result = std::max(result, change); // Edited outranks Typed, which outranks moving
        }
        return result;
    };
    if (!editingId_) {
        if (text != QStringLiteral(" ")) { // Space selects; it doesn't start a region
            startMath(typeMath);
        }
        return;
    }
    if (math_) {
        editMath(typeMath);
        return;
    }
    insertText(text);
}

void WorksheetCanvas::focusOutEvent(QFocusEvent* event)
{
    caretBlink_.stop();
    caretVisible_ = false;
    viewport()->update();
    QAbstractScrollArea::focusOutEvent(event);
}

void WorksheetCanvas::resizeEvent(QResizeEvent* event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateScrollBars();
}

bool WorksheetCanvas::focusNextPrevChild(bool next)
{
    // Tab moves between a plot's placeholders rather than to the next widget.
    // and between a math region's placeholders.
    const lemma::Region* r = worksheet_.region(editingId_);
    if (math_ || (r && r->kind == lemma::Region::Kind::Plot)) {
        return false;
    }
    return QAbstractScrollArea::focusNextPrevChild(next);
}

void WorksheetCanvas::scrollContentsBy(int, int)
{
    viewport()->update();
}
