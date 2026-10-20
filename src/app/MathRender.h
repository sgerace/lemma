#pragma once

#include "lemma/math_layout.h"

#include <QColor>
#include <QFont>
#include <QHash>
#include <QPointF>
#include <QString>

class QPainter;

// The math font, STIX Two Math, bundled as a resource. Text is measured and drawn at a
// fixed pixel size and scaled, so layouts come out the same at any size and on any
// device: the screen and PDF agree, and nothing depends on the screen's DPI.
class MathFont : public lemma::MathFontMetrics {
public:
    static const MathFont& instance();

    Extent measure(std::string_view utf8, double size) const override;
    // Draws text with its baseline starting at `origin`, at em size `size`.
    void drawText(QPainter& painter, QPointF origin, const QString& text, double size) const;

private:
    MathFont();

    QFont font_;
    mutable QHash<QString, Extent> cache_; // at the reference size
};

// Paints a layout with its baseline starting at `origin`.
void paintMath(QPainter& painter, const lemma::MathLayout& layout, QPointF origin, const QColor& color);
