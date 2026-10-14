#include "MainWindow.h"

#include "DocumentIO.h"
#include "PdfExport.h"
#include "WorksheetCanvas.h"

#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenuBar>
#include <QMessageBox>

namespace {

const QString kFileFilter = QObject::tr("Lemma worksheets (*.lemma)");

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , canvas_(new WorksheetCanvas(this))
{
    setCentralWidget(canvas_);
    connect(canvas_, &WorksheetCanvas::modified, this, [this] { setWindowModified(true); });

    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&New"), QKeySequence::New, this, &MainWindow::newFile);
    file->addAction(tr("&Open..."), QKeySequence::Open, this, &MainWindow::open);
    file->addAction(tr("&Save"), QKeySequence::Save, this, &MainWindow::save);
    file->addAction(tr("Save &As..."), QKeySequence::SaveAs, this, &MainWindow::saveAs);
    file->addSeparator();
    file->addAction(tr("&Export PDF..."), QKeySequence(tr("Ctrl+E")), this, &MainWindow::exportPdf);
    file->addSeparator();
    file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&Quick Reference"), QKeySequence::HelpContents, this, &MainWindow::showQuickReference);
    help->addAction(tr("&About Lemma"), this, &MainWindow::showAbout);

    resize(1000, 900);
    setCurrentPath({});
    canvas_->setFocus();
}

bool MainWindow::openFile(const QString& path)
{
    QString error;
    if (!DocumentIO::load(canvas_->worksheet(), path, &error)) {
        QMessageBox::warning(this, tr("Open"), tr("Couldn't open %1:\n%2").arg(QFileInfo(path).fileName(), error));
        canvas_->worksheet().clear();
        canvas_->worksheetReset();
        setCurrentPath({});
        return false;
    }
    canvas_->worksheetReset();
    setCurrentPath(path);
    return true;
}

void MainWindow::newFile()
{
    if (!maybeSave()) {
        return;
    }
    canvas_->worksheet().clear();
    canvas_->worksheetReset();
    setCurrentPath({});
}

void MainWindow::open()
{
    if (!maybeSave()) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, tr("Open Worksheet"), {}, kFileFilter);
    if (!path.isEmpty()) {
        openFile(path);
    }
}

bool MainWindow::save()
{
    return currentPath_.isEmpty() ? saveAs() : saveTo(currentPath_);
}

bool MainWindow::saveAs()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Save Worksheet"), currentPath_, kFileFilter);
    return !path.isEmpty() && saveTo(path);
}

bool MainWindow::saveTo(const QString& path)
{
    QString error;
    if (!DocumentIO::save(canvas_->worksheet(), path, &error)) {
        QMessageBox::warning(this, tr("Save"), tr("Couldn't save %1:\n%2").arg(QFileInfo(path).fileName(), error));
        return false;
    }
    setCurrentPath(path);
    return true;
}

void MainWindow::exportPdf()
{
    QString suggested = currentPath_;
    if (!suggested.isEmpty()) {
        suggested = QFileInfo(suggested).path() + "/" + QFileInfo(suggested).completeBaseName() + ".pdf";
    }
    const QString path = QFileDialog::getSaveFileName(this, tr("Export PDF"), suggested, tr("PDF files (*.pdf)"));
    if (path.isEmpty()) {
        return;
    }

    if (!::exportPdf(*canvas_, path, QFileInfo(currentPath_).completeBaseName())) {
        QMessageBox::warning(this, tr("Export PDF"), tr("Couldn't write %1.").arg(QFileInfo(path).fileName()));
    }
}

void MainWindow::showQuickReference()
{
    QMessageBox::information(this, tr("Quick Reference"), tr(
        "<p>Click anywhere on the page to place the red crosshair, then start typing.</p>"
        "<table cellspacing='6'>"
        "<tr><td><code>L := 2 m</code></td><td>Define a variable (typing <code>:</code> inserts <code>:=</code>)</td></tr>"
        "<tr><td><code>L =</code></td><td>Show a result</td></tr>"
        "<tr><td><code>L = ft</code></td><td>Show a result in a chosen unit</td></tr>"
        "<tr><td><code>f(x) := x^2</code></td><td>Define a function</td></tr>"
        "<tr><td><code>10 N/m</code></td><td>A space multiplies, like <code>*</code></td></tr>"
        "</table>"
        "<p>Worksheets calculate top to bottom, then left to right. Drag a region to move it.</p>"
        "<p>Built-ins: sqrt, abs, sin, cos, tan, asin, acos, atan, ln, log, exp, min, max; "
        "π (or pi), e, and g (gravity; grams are <code>gm</code>).</p>"));
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("About Lemma"),
                       tr("<b>Lemma %1</b><p>A Mathcad-style worksheet for engineering calculations.</p>")
                           .arg(QApplication::applicationVersion()));
}

bool MainWindow::maybeSave()
{
    if (!isWindowModified()) {
        return true;
    }
    const auto answer = QMessageBox::warning(this, tr("Lemma"), tr("Save changes to this worksheet?"),
                                             QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (answer == QMessageBox::Save) {
        return save();
    }
    return answer == QMessageBox::Discard;
}

void MainWindow::setCurrentPath(const QString& path)
{
    currentPath_ = path;
    setWindowModified(false);
    const QString name = path.isEmpty() ? tr("Untitled") : QFileInfo(path).fileName();
    setWindowTitle(tr("%1[*] - Lemma").arg(name));
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (maybeSave()) {
        event->accept();
    } else {
        event->ignore();
    }
}
