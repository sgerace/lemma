#include "WorksheetCanvas.h"

#include <QApplication>
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

WorksheetCanvas::WorksheetCanvas(QWidget* parent)
    : QAbstractScrollArea(parent)
{
    font_.setFamilies({"Cambria", "STIX Two Text", "Times New Roman"});
    font_.setPointSizeF(12);
    errorFont_ = font_;
    errorFont_.setPointSizeF(9);
    errorFont_.setItalic(true);

    viewport()->setCursor(Qt::IBeamCursor);
    viewport()->setMouseTracking(false);
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
    editingId_ = id;
    moveCaret(std::clamp(caret, 0, static_cast<int>(fromUtf8(r->source).size())));
}

void WorksheetCanvas::endEditing()
{
    if (!editingId_) {
        return;
    }
    const lemma::Region* r = worksheet_.region(editingId_);
    const bool empty = !r || fromUtf8(r->source).trimmed().isEmpty();
    if (empty) {
        worksheet_.removeRegion(editingId_);
    }
    editingId_ = 0;
    caretBlink_.stop();
    if (empty) {
        contentChanged();
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

void WorksheetCanvas::insertText(const QString& text)
{
    lemma::Region* r = worksheet_.region(editingId_);
    QString source = fromUtf8(r->source);
    source.insert(caret_, text);
    r->source = source.toStdString();
    moveCaret(caret_ + static_cast<int>(text.size()));
    contentChanged();
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
    source.remove(from, count);
    r->source = source.toStdString();
    moveCaret(from);
    contentChanged();
}

void WorksheetCanvas::contentChanged()
{
    worksheet_.evaluate();
    updateScrollBars();
    viewport()->update();
    emit modified();
}

// ---------------------------------------------------------------------------
// Input

void WorksheetCanvas::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const QPointF pos = toDocument(event->position());
    if (const int id = regionAt(pos)) {
        const lemma::Region* r = worksheet_.region(id);
        pressedId_ = id;
        pressPos_ = pos;
        pressRegionPos_ = {r->x, r->y};
        dragging_ = false;
        return;
    }
    endEditing();
    crosshair_ = snap(pos);
    viewport()->update();
}

void WorksheetCanvas::mouseMoveEvent(QMouseEvent* event)
{
    if (!pressedId_) {
        return;
    }
    const QPointF delta = toDocument(event->position()) - pressPos_;
    if (!dragging_ && delta.manhattanLength() >= QApplication::startDragDistance()) {
        dragging_ = true;
        viewport()->setCursor(Qt::SizeAllCursor);
    }
    if (dragging_) {
        lemma::Region* r = worksheet_.region(pressedId_);
        const QPointF moved = snap(pressRegionPos_ + delta);
        r->x = std::max(0.0, moved.x());
        r->y = std::max(kGrid, moved.y());
        contentChanged(); // moving a region can change evaluation order
    }
}

void WorksheetCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !pressedId_) {
        return;
    }
    if (!dragging_) {
        const lemma::Region* r = worksheet_.region(pressedId_);
        beginEditing(pressedId_, caretIndexAt(*r, pressPos_.x()));
    }
    viewport()->setCursor(Qt::IBeamCursor);
    pressedId_ = 0;
    dragging_ = false;
}

void WorksheetCanvas::keyPressEvent(QKeyEvent* event)
{
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
        QPointF step;
        switch (event->key()) {
        case Qt::Key_Left: step = {-kGrid, 0}; break;
        case Qt::Key_Right: step = {kGrid, 0}; break;
        case Qt::Key_Up: step = {0, -kGrid}; break;
        case Qt::Key_Down: step = {0, kGrid}; break;
        default: break;
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
