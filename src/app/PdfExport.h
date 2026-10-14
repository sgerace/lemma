#pragma once

#include <QString>

class WorksheetCanvas;

// Writes the worksheet's pages to a US Letter PDF.
bool exportPdf(const WorksheetCanvas& canvas, const QString& path, const QString& title);
