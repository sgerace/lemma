#include "MathRender.h"

#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

namespace {

// The pixel size text is measured and drawn at, then scaled to the size wanted.
constexpr double kReferenceSize = 100.0;

} // namespace

const MathFont& MathFont::instance()
{
    static const MathFont font;
    return font;
}

MathFont::MathFont()
{
    const int id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/STIXTwoMath-Regular.otf"));
    const QStringList families = QFontDatabase::applicationFontFamilies(id);
    font_.setFamilies({families.value(0, QStringLiteral("STIX Two Math")), QStringLiteral("Cambria Math")});
    font_.setPixelSize(static_cast<int>(kReferenceSize));
    // Hinting is for one size; this font is scaled to many.
    font_.setHintingPreference(QFont::PreferNoHinting);
}

lemma::MathFontMetrics::Extent MathFont::measure(std::string_view utf8, double size) const
{
    if (utf8.empty()) {
        return {};
    }
    const QString text = QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
    auto it = cache_.constFind(text);
    if (it == cache_.constEnd()) {
        const QFontMetricsF fm(font_);
        const QRectF ink = fm.tightBoundingRect(text);
        it = cache_.insert(text, {fm.horizontalAdvance(text), std::max(0.0, -ink.top()), std::max(0.0, ink.bottom())});
    }
    const double scale = size / kReferenceSize;
    return {it->width * scale, it->ascent * scale, it->descent * scale};
}

void MathFont::drawText(QPainter& painter, QPointF origin, const QString& text, double size) const
{
    painter.save();
    painter.translate(origin);
    painter.scale(size / kReferenceSize, size / kReferenceSize);
    painter.setFont(font_);
    painter.drawText(QPointF(0, 0), text);
    painter.restore();
}

void paintMath(QPainter& painter, const lemma::MathLayout& layout, QPointF origin, const QColor& color)
{
    using Kind = lemma::MathLayoutItem::Kind;
    painter.save();
    painter.translate(origin);
    for (const lemma::MathLayoutItem& item : layout.items) {
        const QRectF rect(item.x, item.y, item.width, item.height);
        switch (item.kind) {
        case Kind::Text:
            painter.setPen(color);
            MathFont::instance().drawText(painter, {item.x, item.y}, QString::fromStdString(item.text), item.size);
            break;
        case Kind::Rule:
        case Kind::Placeholder:
            painter.fillRect(rect, color);
            break;
        case Kind::MissingOperator:
            painter.setPen(QPen(color, std::max(0.8, item.width * 0.12)));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(rect);
            break;
        case Kind::Radical: {
            QPolygonF line;
            for (const auto& [x, y] : item.points) {
                line << QPointF(x, y);
            }
            painter.setPen(QPen(color, item.thickness, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
            painter.setBrush(Qt::NoBrush);
            painter.drawPolyline(line);
            break;
        }
        case Kind::Delimiter: {
            // A parenthesis as a curve, bulging away from what it encloses.
            const bool open = item.text == "(";
            const double outer = open ? rect.left() + rect.width() * 0.2 : rect.right() - rect.width() * 0.2;
            const double inner = open ? rect.right() - rect.width() * 0.15 : rect.left() + rect.width() * 0.15;
            QPainterPath path({inner, rect.top()});
            path.cubicTo(outer, rect.top() + rect.height() * 0.2, outer, rect.bottom() - rect.height() * 0.2, inner,
                         rect.bottom());
            painter.setPen(QPen(color, item.thickness * 1.3, Qt::SolidLine, Qt::RoundCap));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);
            break;
        }
        }
    }
    painter.restore();
}
