// GUI tests for the worksheet canvas. Run with QT_QPA_PLATFORM=offscreen: events are
// delivered straight to the widget, never through the real desktop.

#include "DocumentIO.h"
#include "PdfExport.h"
#include "WorksheetCanvas.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QtTest>

namespace {

QString source(const lemma::Region& r)
{
    return QString::fromStdString(r.source);
}

QString value(const lemma::Region& r)
{
    return QString::fromStdString(r.result.value);
}

} // namespace

class CanvasTest : public QObject {
    Q_OBJECT

private slots:
    void init()
    {
        canvas_ = std::make_unique<WorksheetCanvas>();
        canvas_->resize(1000, 800);
        canvas_->show();
        QVERIFY(QTest::qWaitForWindowExposed(canvas_.get()));
    }

    void cleanup() { canvas_.reset(); }

    void typingAtTheCrosshairCreatesARegion()
    {
        click({72, 480});
        QTest::keyClicks(canvas_.get(), "w: 2 kN/m");

        const auto& regions = sheet().regions();
        QCOMPARE(regions.size(), 1u);
        QCOMPARE(source(regions[0]), QStringLiteral("w:= 2 kN/m")); // ':' expands to ':='
        QCOMPARE(regions[0].x, 72.0);
        QCOMPARE(regions[0].y, 480.0);
    }

    void enterMovesBelowAndResultsUpdateLive()
    {
        click({72, 480});
        QTest::keyClicks(canvas_.get(), "w: 2 kN/m");
        QTest::keyClick(canvas_.get(), Qt::Key_Return);
        QTest::keyClicks(canvas_.get(), "w*3 m = kN");

        const auto& regions = sheet().regions();
        QCOMPARE(regions.size(), 2u);
        QVERIFY(regions[1].y > regions[0].y);
        QCOMPARE(regions[1].x, regions[0].x);
        QCOMPARE(value(regions[1]), QStringLiteral("6 kN"));

        // Editing the definition above recalculates the result below it.
        click(regionPoint(regions[0], 5)); // just after "w:= 2"
        QTest::keyClick(canvas_.get(), Qt::Key_Backspace);
        QTest::keyClicks(canvas_.get(), "5");
        QCOMPARE(source(sheet().regions()[0]), QStringLiteral("w:= 5 kN/m"));
        QCOMPARE(value(sheet().regions()[1]), QStringLiteral("15 kN"));
    }

    void caretMovementAndDeletion()
    {
        click({72, 480});
        QTest::keyClicks(canvas_.get(), "12");
        QTest::keyClick(canvas_.get(), Qt::Key_Left);
        QTest::keyClicks(canvas_.get(), "3");
        QCOMPARE(source(sheet().regions()[0]), QStringLiteral("132"));

        QTest::keyClick(canvas_.get(), Qt::Key_Backspace);
        QCOMPARE(source(sheet().regions()[0]), QStringLiteral("12"));

        QTest::keyClick(canvas_.get(), Qt::Key_Home);
        QTest::keyClick(canvas_.get(), Qt::Key_Delete);
        QCOMPARE(source(sheet().regions()[0]), QStringLiteral("2"));

        QTest::keyClick(canvas_.get(), Qt::Key_End);
        QTest::keyClicks(canvas_.get(), "0");
        QCOMPARE(source(sheet().regions()[0]), QStringLiteral("20"));
    }

    void emptiedRegionIsRemovedWhenEditingEnds()
    {
        click({72, 480});
        QTest::keyClicks(canvas_.get(), "a");
        QTest::keyClick(canvas_.get(), Qt::Key_Backspace);
        QTest::keyClick(canvas_.get(), Qt::Key_Escape);
        QVERIFY(sheet().regions().empty());
    }

    void draggingARegionChangesEvaluationOrder()
    {
        const int use = sheet().addRegion(72, 120, "x =");
        const int def = sheet().addRegion(72, 240, "x := 5");
        canvas_->worksheetReset();
        QCOMPARE(QString::fromStdString(sheet().region(use)->result.error), QStringLiteral("x is not defined"));

        // Drag the definition above the use.
        const QPoint from = canvas_->viewportPosition(regionPoint(*sheet().region(def), 1)).toPoint();
        const QPoint to = from - QPoint(0, 180);
        QWidget* viewport = canvas_->viewport();
        QTest::mousePress(viewport, Qt::LeftButton, {}, from);
        QTest::mouseMove(viewport, from - QPoint(0, 20));
        QTest::mouseMove(viewport, to);
        QTest::mouseRelease(viewport, Qt::LeftButton, {}, to);

        QVERIFY(sheet().region(def)->y < sheet().region(use)->y);
        QCOMPARE(value(*sheet().region(use)), QStringLiteral("5"));
    }

    void rubberBandSelectsAndDeleteRemoves()
    {
        const int a = sheet().addRegion(72, 120, "a := 1");
        const int b = sheet().addRegion(72, 240, "b := 2");
        const int c = sheet().addRegion(480, 120, "c := 3");
        canvas_->worksheetReset();

        drag({48, 96}, {300, 264});
        QCOMPARE(canvas_->selection(), (QSet<int>{a, b}));

        QTest::keyClick(canvas_.get(), Qt::Key_Delete);
        QCOMPARE(sheet().regions().size(), 1u);
        QCOMPARE(sheet().regions()[0].id, c);
        QVERIFY(canvas_->selection().isEmpty());
    }

    void shiftClickTogglesSelection()
    {
        const int a = sheet().addRegion(72, 120, "a := 1");
        const int b = sheet().addRegion(72, 240, "b := 2");
        canvas_->worksheetReset();

        shiftClick(regionPoint(*sheet().region(a), 1));
        shiftClick(regionPoint(*sheet().region(b), 1));
        QCOMPARE(canvas_->selection(), (QSet<int>{a, b}));

        shiftClick(regionPoint(*sheet().region(a), 1));
        QCOMPARE(canvas_->selection(), QSet<int>{b});

        // Shift+drag on empty space adds to the selection rather than replacing it.
        drag({48, 96}, {300, 132}, Qt::ShiftModifier);
        QCOMPARE(canvas_->selection(), (QSet<int>{a, b}));

        // A plain click on empty space clears it and still moves the crosshair.
        click({480, 600});
        QVERIFY(canvas_->selection().isEmpty());
        QTest::keyClicks(canvas_.get(), "1");
        QCOMPARE(sheet().regions().back().x, 480.0);
        QCOMPARE(sheet().regions().back().y, 600.0);
    }

    void draggingASelectionMovesItTogether()
    {
        const int use = sheet().addRegion(72, 120, "x + y =");
        const int x = sheet().addRegion(72, 240, "x := 5");
        const int y = sheet().addRegion(300, 240, "y := 1");
        canvas_->worksheetReset();
        QCOMPARE(value(*sheet().region(use)), QString());

        shiftClick(regionPoint(*sheet().region(x), 1));
        shiftClick(regionPoint(*sheet().region(y), 1));
        drag(regionPoint(*sheet().region(x), 1), regionPoint(*sheet().region(x), 1) - QPointF(0, 180));

        QCOMPARE(sheet().region(x)->y, 60.0);
        QCOMPARE(sheet().region(y)->y, 60.0);
        QCOMPARE(sheet().region(x)->x, 72.0);
        QCOMPARE(sheet().region(y)->x, 300.0);
        QCOMPARE(sheet().region(use)->y, 120.0);
        QCOMPARE(value(*sheet().region(use)), QStringLiteral("6")); // evaluation order follows the move
    }

    void selectAllAndArrowKeysNudge()
    {
        const int a = sheet().addRegion(72, 120, "a := 1");
        const int b = sheet().addRegion(300, 240, "b := 2");
        canvas_->worksheetReset();

        QTest::keyClick(canvas_.get(), Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(canvas_->selection(), (QSet<int>{a, b}));

        QTest::keyClick(canvas_.get(), Qt::Key_Right);
        QTest::keyClick(canvas_.get(), Qt::Key_Down);
        QCOMPARE(sheet().region(a)->x, 72.0 + WorksheetCanvas::kGrid);
        QCOMPARE(sheet().region(a)->y, 120.0 + WorksheetCanvas::kGrid);
        QCOMPARE(sheet().region(b)->x, 300.0 + WorksheetCanvas::kGrid);

        // The group stops as a whole at the page edge, keeping its layout.
        for (int i = 0; i < 20; ++i) {
            QTest::keyClick(canvas_.get(), Qt::Key_Left);
        }
        QCOMPARE(sheet().region(a)->x, 0.0);
        QCOMPARE(sheet().region(b)->x, 228.0);

        // Escape clears the selection, and arrows go back to moving the crosshair.
        QTest::keyClick(canvas_.get(), Qt::Key_Escape);
        QVERIFY(canvas_->selection().isEmpty());
        QTest::keyClick(canvas_.get(), Qt::Key_Right);
        QCOMPARE(sheet().region(a)->x, 0.0);
    }

    void saveAndLoadRoundTrip()
    {
        sheet().addRegion(72, 96, "L := 2.5 m");
        sheet().addRegion(288, 96, "δ := L / 240");
        sheet().addRegion(72, 144, "δ = mm");
        sheet().evaluate();

        QTemporaryDir dir;
        const QString path = dir.filePath("round-trip.lemma");
        QString error;
        QVERIFY2(DocumentIO::save(sheet(), path, &error), qPrintable(error));

        lemma::Worksheet loaded;
        QVERIFY2(DocumentIO::load(loaded, path, &error), qPrintable(error));
        QCOMPARE(loaded.regions().size(), 3u);
        for (std::size_t i = 0; i < 3; ++i) {
            QCOMPARE(source(loaded.regions()[i]), source(sheet().regions()[i]));
            QCOMPARE(loaded.regions()[i].x, sheet().regions()[i].x);
            QCOMPARE(loaded.regions()[i].y, sheet().regions()[i].y);
        }
        QCOMPARE(value(loaded.regions()[2]), QStringLiteral("10.417 mm"));
    }

    void loadRejectsForeignAndNewerFiles()
    {
        QTemporaryDir dir;
        const auto write = [&](const QString& name, const QJsonObject& root) {
            QFile f(dir.filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QJsonDocument(root).toJson());
        };
        write("foreign.lemma", {{"hello", "world"}});
        write("newer.lemma", {{"format", "lemma-worksheet"}, {"version", 999}});

        lemma::Worksheet ws;
        QString error;
        QVERIFY(!DocumentIO::load(ws, dir.filePath("foreign.lemma"), &error));
        QVERIFY(error.contains("not a Lemma worksheet"));
        QVERIFY(!DocumentIO::load(ws, dir.filePath("newer.lemma"), &error));
        QVERIFY(error.contains("newer version"));
        QVERIFY(!DocumentIO::load(ws, dir.filePath("missing.lemma"), &error));
    }

    void pdfExportWritesOnePagePerPageOfContent()
    {
        sheet().addRegion(72, 96, "L := 2.5 m");
        sheet().addRegion(72, 1200, "L = ft"); // on page 2
        canvas_->worksheetReset();
        QCOMPARE(canvas_->pageCount(), 2);

        QTemporaryDir dir;
        const QString path = dir.filePath("out.pdf");
        QVERIFY(exportPdf(*canvas_, path, "test"));

        QFile pdf(path);
        QVERIFY(pdf.open(QIODevice::ReadOnly));
        const QByteArray bytes = pdf.readAll();
        QVERIFY(bytes.startsWith("%PDF"));
        QCOMPARE(bytes.count("/Type /Page\n") + bytes.count("/Type /Page\r") + bytes.count("/Type /Page "), 2);
    }

    void snapshot()
    {
        // Saves what the canvas draws, for eyeballing. Set LEMMA_SNAPSHOT_DIR to keep it.
        const QString dir = qEnvironmentVariable("LEMMA_SNAPSHOT_DIR");
        if (dir.isEmpty()) {
            QSKIP("LEMMA_SNAPSHOT_DIR not set");
        }
        QString error;
        QVERIFY2(DocumentIO::load(sheet(), QStringLiteral(LEMMA_EXAMPLES_DIR "/cantilever-beam.lemma"), &error),
                 qPrintable(error));
        canvas_->worksheetReset();

        click({72, 480});
        QTest::keyClicks(canvas_.get(), "w: 2 kN/m");
        QTest::keyClick(canvas_.get(), Qt::Key_Return);
        QTest::keyClicks(canvas_.get(), "w L^4 / (8 E I) = mm");
        QTest::keyClick(canvas_.get(), Qt::Key_Return);
        QTest::keyClicks(canvas_.get(), "w L + 1 kg =");
        shiftClick(regionPoint(sheet().regions()[1], 1)); // selection shows on screen, not in the PDF

        QVERIFY(canvas_->grab().save(dir + "/canvas.png"));
        QVERIFY(exportPdf(*canvas_, dir + "/canvas.pdf", "snapshot"));
    }

private:
    lemma::Worksheet& sheet() { return canvas_->worksheet(); }

    void click(QPointF docPos)
    {
        QTest::mouseClick(canvas_->viewport(), Qt::LeftButton, {}, canvas_->viewportPosition(docPos).toPoint());
    }

    void shiftClick(QPointF docPos)
    {
        QTest::mouseClick(canvas_->viewport(), Qt::LeftButton, Qt::ShiftModifier,
                          canvas_->viewportPosition(docPos).toPoint());
    }

    // Press, move past the drag threshold, and release, all in document coordinates.
    void drag(QPointF fromDoc, QPointF toDoc, Qt::KeyboardModifiers modifiers = {})
    {
        QWidget* viewport = canvas_->viewport();
        const QPoint from = canvas_->viewportPosition(fromDoc).toPoint();
        const QPoint to = canvas_->viewportPosition(toDoc).toPoint();
        QTest::mousePress(viewport, Qt::LeftButton, modifiers, from);
        QTest::mouseMove(viewport, (from + to) / 2);
        QTest::mouseMove(viewport, to);
        QTest::mouseRelease(viewport, Qt::LeftButton, modifiers, to);
    }

    // A point inside a region, just after its first `chars` characters.
    QPointF regionPoint(const lemma::Region& r, int chars) const
    {
        return canvas_->sourcePosition(r, chars) - QPointF(0, 4);
    }

    std::unique_ptr<WorksheetCanvas> canvas_;
};

QTEST_MAIN(CanvasTest)
#include "test_canvas.moc"
