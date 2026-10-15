#include "WorksheetCanvas.h"

#include <QApplication>
#include <QCursor>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScrollBar>

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

QString fromUtf8(const std::string& s)
{
    return QString::fromStdString(s);
}

// Typeset linear input a little for display: "b*h^3" becomes "b·h³".
QString prettify(QString text)
{
    static const QString kSuperscripts = QStringLiteral("⁰¹²³⁴⁵⁶⁷⁸⁹");
    static const QRegularExpression kPower(QStringLiteral("\\^(-?)(\\d+)(?![\\d.])"));

    text.replace('*', QChar(0x00B7));
    QString out;
    qsizetype last = 0;
    for (auto it = kPower.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        out += text.mid(last, m.capturedStart() - last);
        if (!m.captured(1).isEmpty()) {
            out += QChar(0x207B); // superscript minus
        }
        for (const QChar digit : m.captured(2)) {
            out += kSuperscripts.at(digit.digitValue());
        }
        last = m.capturedEnd();
    }
    return out + text.mid(last);
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
    return a->x == b->x && a->y == b->y && a->source == b->source;
}

} // namespace

// One undo step: the regions an edit touched, before and after. A region that is absent
// on one side was created or removed, so this one command covers every kind of edit.
class WorksheetCanvas::Change : public QUndoCommand {
public:
    Change(WorksheetCanvas* canvas, const QString& text, EditKind kind, RegionStates before, RegionStates after,
           int session, int caretBefore, int caretAfter)
        : QUndoCommand(text)
        , canvas_(canvas)
        , kind_(kind)
        , before_(std::move(before))
        , after_(std::move(after))
        , session_(session)
        , caretBefore_(caretBefore)
        , caretAfter_(caretAfter)
    {
    }

    int id() const override { return kind_ == EditKind::Other ? -1 : kEditMergeId; }

    void undo() override { canvas_->restore(before_, editId(), caretBefore_); }

    void redo() override
    {
        // The canvas made the change before pushing it.
        if (pushed_) {
            pushed_ = false;
            return;
        }
        canvas_->restore(after_, editId(), caretAfter_);
    }

    bool mergeWith(const QUndoCommand* other) override
    {
        // Typing and erasing are always about a single region.
        const auto* next = static_cast<const Change*>(other);
        if (next->session_ != session_ || next->before_.front().id != before_.front().id) {
            return false;
        }
        // Removing a region emptied by erasing folds into the erase, so undo brings back its text.
        const bool removal = !next->after_.front().region;
        if (!removal && (next->kind_ != kind_ || next->caretBefore_ != caretAfter_)) {
            return false;
        }
        after_ = next->after_;
        caretAfter_ = next->caretAfter_;
        setObsolete(sameState(before_.front().region, after_.front().region)); // e.g. typed a space, then left
        return true;
    }

private:
    // Undoing typing puts the caret back in the region.
    int editId() const { return kind_ == EditKind::Other ? 0 : before_.front().id; }

    WorksheetCanvas* canvas_;
    EditKind kind_;
    RegionStates before_;
    RegionStates after_;
    int session_;
    int caretBefore_;
    int caretAfter_;
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

void WorksheetCanvas::record(const QString& text, RegionStates before, EditKind kind, int caretBefore)
{
    RegionStates after;
    bool changed = false;
    for (const RegionState& s : before) {
        const lemma::Region* r = worksheet_.region(s.id);
        after.push_back({s.id, r ? std::optional(*r) : std::nullopt});
        changed = changed || !sameState(s.region, after.back().region);
    }
    if (changed) {
        undoStack_.push(new Change(this, text, kind, std::move(before), std::move(after), editSession_, caretBefore, caret_));
    }
}

void WorksheetCanvas::restore(const RegionStates& states, int editId, int caret)
{
    // Drop any edit or drag as is. Ending an edit normally can push a command, which
    // mustn't happen from inside undo, and these states are already on the stack.
    editingId_ = 0;
    caretBlink_.stop();
    pressedId_ = 0;
    dragging_ = bandPressed_ = banding_ = false;
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
        beginEditing(editId, caret);
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

void WorksheetCanvas::worksheetReset()
{
    editingId_ = 0;
    caretBlink_.stop();
    pressedId_ = 0;
    bandPressed_ = banding_ = false;
    selection_.clear();
    undoStack_.clear();
    crosshair_ = {kMargin, kMargin + kGrid * 2};
    worksheet_.evaluate();
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

QString WorksheetCanvas::shownText(const lemma::Region& r) const
{
    if (r.id == editingId_ || r.result.status == lemma::RegionResult::Status::Error) {
        return fromUtf8(r.source);
    }
    return prettify(fromUtf8(r.result.display));
}

QString WorksheetCanvas::hintText(const lemma::Region& r) const
{
    // While editing an evaluation, show its live result after the caret text.
    if (r.id == editingId_ && r.result.status == lemma::RegionResult::Status::Ok
        && r.result.kind == lemma::Statement::Kind::Evaluation) {
        return QStringLiteral("  → ") + fromUtf8(r.result.value);
    }
    return {};
}

QRectF WorksheetCanvas::regionRect(const lemma::Region& r) const
{
    const QFontMetricsF fm(font_);
    const double width = std::max(fm.horizontalAdvance(shownText(r) + hintText(r)), 4.0);
    QRectF rect(r.x, r.y - fm.ascent(), width, fm.ascent() + fm.descent());
    if (r.result.status == lemma::RegionResult::Status::Error) {
        const QFontMetricsF efm(errorFont_);
        rect.setWidth(std::max(rect.width(), efm.horizontalAdvance(fromUtf8(r.result.error))));
        rect.setHeight(rect.height() + efm.height() + 2);
    }
    return rect;
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

QPointF WorksheetCanvas::sourcePosition(const lemma::Region& r, int index) const
{
    return {r.x + QFontMetricsF(font_).horizontalAdvance(fromUtf8(r.source).left(index)), r.y};
}

int WorksheetCanvas::caretIndexAt(const lemma::Region& r, double docX) const
{
    const QFontMetricsF fm(font_);
    const QString text = fromUtf8(r.source);
    int best = 0;
    double bestDistance = std::abs(docX - r.x);
    for (int i = 1; i <= text.size(); ++i) {
        const double distance = std::abs(docX - (r.x + fm.horizontalAdvance(text.left(i))));
        if (distance < bestDistance) {
            best = i;
            bestDistance = distance;
        }
    }
    return best;
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
    const QFontMetricsF fm(font_);
    for (const lemma::Region& r : worksheet_.regions()) {
        const bool isError = r.result.status == lemma::RegionResult::Status::Error;
        const QString text = shownText(r);
        const QPointF baseline(r.x, r.y);

        painter.setFont(font_);
        painter.setPen(isError ? kErrorColor : kTextColor);
        painter.drawText(baseline, text);

        if (const QString hint = hintText(r); !hint.isEmpty()) {
            painter.setPen(kHintColor);
            painter.drawText(baseline + QPointF(fm.horizontalAdvance(text), 0), hint);
        }

        if (isError) {
            // Underline the part of the source the error points at.
            const std::string& src = r.source;
            const auto clampByte = [&](std::size_t b) { return std::min(b, src.size()); };
            const QString before = fromUtf8(src.substr(0, clampByte(r.result.errorBegin)));
            QString marked = fromUtf8(src.substr(clampByte(r.result.errorBegin),
                                                 clampByte(r.result.errorEnd) - clampByte(r.result.errorBegin)));
            if (marked.isEmpty()) {
                marked = QStringLiteral(" "); // e.g. "incomplete expression" at the end
            }
            const double x0 = r.x + fm.horizontalAdvance(before);
            const double x1 = x0 + fm.horizontalAdvance(marked);
            const double y = r.y + fm.descent() * 0.6;
            QPainterPath squiggle({x0, y});
            for (double x = x0; x < x1; x += 2.0) {
                squiggle.lineTo(std::min(x + 2.0, x1), y + ((static_cast<int>((x - x0) / 2.0) % 2) ? 0.0 : 1.5));
            }
            painter.setPen(QPen(kErrorColor, 0.8));
            painter.drawPath(squiggle);

            painter.setFont(errorFont_);
            const QFontMetricsF efm(errorFont_);
            painter.drawText(QPointF(r.x, r.y + fm.descent() + 2 + efm.ascent()), fromUtf8(r.result.error));
        }

        if (onScreen && selection_.contains(r.id)) {
            painter.setPen(QPen(kEditBoxColor, 1.0));
            painter.setBrush(kSelectionFill);
            painter.drawRect(regionRect(r).adjusted(-3, -2, 3, 2));
        }

        if (onScreen && r.id == editingId_) {
            painter.setPen(QPen(kEditBoxColor, 1.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(regionRect(r).adjusted(-3, -2, 3, 2));
            if (caretVisible_) {
                const double x = r.x + fm.horizontalAdvance(fromUtf8(r.source).left(caret_));
                painter.setPen(QPen(kTextColor, 1.2));
                painter.drawLine(QPointF(x, r.y - fm.ascent()), QPointF(x, r.y + fm.descent()));
            }
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

void WorksheetCanvas::beginEditing(int id, int caret)
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
    ++editSession_;
    moveCaret(std::clamp(caret, 0, static_cast<int>(fromUtf8(r->source).size())));
}

void WorksheetCanvas::endEditing()
{
    if (!editingId_) {
        return;
    }
    const lemma::Region* r = worksheet_.region(editingId_);
    const bool empty = !r || fromUtf8(r->source).trimmed().isEmpty();
    RegionStates before;
    if (empty) {
        before = capture({editingId_});
        worksheet_.removeRegion(editingId_);
    }
    editingId_ = 0;
    caretBlink_.stop();
    if (empty) {
        contentChanged();
        record(tr("Delete"), std::move(before), EditKind::Erasing, caret_);
    } else {
        viewport()->update();
    }
}

void WorksheetCanvas::moveCaret(int position)
{
    caret_ = position;
    caretVisible_ = true;
    caretBlink_.start();
    viewport()->update();
}

void WorksheetCanvas::insertText(const QString& text, bool created)
{
    // A region created for this text didn't exist before it, as far as undo is concerned.
    RegionStates before = created ? RegionStates{{editingId_, std::nullopt}} : capture({editingId_});
    const int caretBefore = caret_;
    lemma::Region* r = worksheet_.region(editingId_);
    QString source = fromUtf8(r->source);
    source.insert(caret_, text);
    r->source = source.toStdString();
    moveCaret(caret_ + static_cast<int>(text.size()));
    contentChanged();
    record(tr("Typing"), std::move(before), EditKind::Typing, caretBefore);
}

void WorksheetCanvas::eraseAtCaret(bool before)
{
    lemma::Region* r = worksheet_.region(editingId_);
    QString source = fromUtf8(r->source);
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
    r->source = source.toStdString();
    moveCaret(from);
    contentChanged();
    record(tr("Delete"), std::move(previous), EditKind::Erasing, caretBefore);
}

void WorksheetCanvas::contentChanged()
{
    worksheet_.evaluate();
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
    viewport()->setCursor(regionAt(toDocument(viewportPos)) ? Qt::IBeamCursor : Qt::ArrowCursor);
}

void WorksheetCanvas::mouseMoveEvent(QMouseEvent* event)
{
    if (!(event->buttons() & Qt::LeftButton)) {
        updateCursor(event->position());
        return;
    }
    const QPointF pos = toDocument(event->position());
    const bool pastThreshold = (pos - pressPos_).manhattanLength() >= QApplication::startDragDistance();

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
    if (dragging_) {
        record(tr("Move"), std::move(dragBefore_));
    } else {
        const lemma::Region* r = worksheet_.region(pressedId_);
        beginEditing(pressedId_, caretIndexAt(*r, pressPos_.x()));
    }
    pressedId_ = 0;
    updateCursor(event->position());
    dragging_ = false;
}

void WorksheetCanvas::keyPressEvent(QKeyEvent* event)
{
    if (event->matches(QKeySequence::SelectAll)) {
        selectAll();
        return;
    }
    if (editingId_) {
        const lemma::Region* r = worksheet_.region(editingId_);
        const int length = static_cast<int>(fromUtf8(r->source).size());
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
    if (text == ":") {
        text = ":="; // Mathcad's shortcut for definitions
    }
    if (!editingId_) {
        const int id = worksheet_.addRegion(crosshair_.x(), crosshair_.y());
        beginEditing(id, 0);
        insertText(text, true);
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

void WorksheetCanvas::scrollContentsBy(int, int)
{
    viewport()->update();
}
