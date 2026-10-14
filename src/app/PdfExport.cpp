#include "PdfExport.h"

#include "WorksheetCanvas.h"

#include <QApplication>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>

bool exportPdf(const WorksheetCanvas& canvas, const QString& path, const QString& title)
{
    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::Letter));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));
    writer.setResolution(96); // one device pixel per document unit
    writer.setTitle(title);
    writer.setCreator(QStringLiteral("Lemma %1").arg(QApplication::applicationVersion()));

    QPainter painter(&writer);
    if (!painter.isActive()) {
        return false;
    }
    for (int page = 0; page < canvas.pageCount(); ++page) {
        if (page > 0) {
            writer.newPage();
        }
        canvas.renderPage(painter, page);
    }
    return painter.end();
}
