#include "MainWindow.h"

#include "DocumentIO.h"
#include "PdfExport.h"
#include "WorksheetCanvas.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>

namespace {

const QString kFileFilter = QObject::tr("Lemma worksheets (*.lemma)");

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , canvas_(new WorksheetCanvas(this))
{
    setCentralWidget(canvas_);
    // Undoing back to the saved state clears the modified marker.
    connect(canvas_->undoStack(), &QUndoStack::cleanChanged, this, [this](bool clean) { setWindowModified(!clean); });
    connect(canvas_, &WorksheetCanvas::message, this, [this](const QString& text) { statusBar()->showMessage(text, 5000); });

    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&New"), QKeySequence::New, this, &MainWindow::newFile);
    file->addAction(tr("&Open..."), QKeySequence::Open, this, &MainWindow::open);
    file->addAction(tr("&Save"), QKeySequence::Save, this, &MainWindow::save);
    file->addAction(tr("Save &As..."), QKeySequence::SaveAs, this, &MainWindow::saveAs);
    file->addSeparator();
    file->addAction(tr("&Export PDF..."), QKeySequence(tr("Ctrl+E")), this, &MainWindow::exportPdf);
    file->addSeparator();
    file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    QAction* undo = canvas_->undoStack()->createUndoAction(this, tr("&Undo"));
    undo->setShortcuts(QKeySequence::Undo);
    edit->addAction(undo);
    QAction* redo = canvas_->undoStack()->createRedoAction(this, tr("&Redo"));
    // Ctrl+Y (Cmd+Y on macOS) everywhere, as on Windows, plus the platform's redo keys.
    // It goes first because menus show only the first shortcut.
    const QKeySequence ctrlY(Qt::CTRL | Qt::Key_Y);
    QList<QKeySequence> redoKeys = QKeySequence::keyBindings(QKeySequence::Redo);
    redoKeys.removeAll(ctrlY);
    redoKeys.prepend(ctrlY);
    redo->setShortcuts(redoKeys);
    edit->addAction(redo);

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
        "<tr><td><code>L:2.5m</code></td><td>Define a variable: <code>:</code> gives <code>:=</code>, "
        "and a letter after a number multiplies (<code>2.5 m</code>)</td></tr>"
        "<tr><td><code>L=</code></td><td>Show a result; type a unit after it, like <code>ft</code></td></tr>"
        "<tr><td><code>f(x):x^2</code></td><td>Define a function</td></tr>"
        "<tr><td>Ctrl+.</td><td>Evaluate symbolically: <code>2x+3x</code> then Ctrl+. "
        "shows <code>5 x</code>. Names defined above are replaced by their definitions; the keyword "
        "<code>keep, P</code> (above the arrow) leaves <code>P</code> as it is. Other keywords: "
        "<code>expand</code>, <code>diff, x</code>, <code>substitute, x == 2</code></td></tr>"
        "<tr><td>Ctrl+=</td><td>The bold equals sign of an equation, for symbolic evaluation</td></tr>"
        "<tr><td>Ctrl+Shift+C</td><td>Copy just the result, to paste into another region</td></tr>"
        "<tr><td><code>/</code> <code>^</code> <code>\\</code></td><td>Fraction, exponent, square root. "
        "Typing stays in a denominator or exponent; press → to leave it</td></tr>"
        "<tr><td>Space</td><td>Select more: the next operator applies to everything underlined "
        "(Shift+Space selects less)</td></tr>"
        "<tr><td>Tab</td><td>Go to the next placeholder ■</td></tr>"
        "<tr><td><code>\"</code></td><td>Start a text region, for headings and notes</td></tr>"
        "<tr><td><code>@</code></td><td>Insert an X-Y plot: type a function like <code>f</code>, "
        "then Tab through the axis placeholders</td></tr>"
        "</table>"
        "<p>Worksheets calculate top to bottom, then left to right. Drag a region to move it; "
        "drag across the math you're editing to select part of it.</p>"
        "<p>Drag on empty space or Shift+click to select regions (Ctrl+A selects all). "
        "Drag or use the arrow keys to move the selection; Delete removes it.</p>"
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
    canvas_->undoStack()->setClean();
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
