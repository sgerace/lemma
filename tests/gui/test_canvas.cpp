// GUI tests for the worksheet canvas. Run with QT_QPA_PLATFORM=offscreen: events are
// delivered straight to the widget, never through the real desktop.

#include "DocumentIO.h"
#include "MainWindow.h"
#include "PdfExport.h"
#include "WorksheetCanvas.h"

#include <QFile>
#include <QJsonArray>
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

    void definitionsDisplayWithSpacingAroundAssign()
    {
        click({72, 480});
        QTest::keyClicks(canvas_.get(), "w: 2 kN/m");
        QTest::keyClick(canvas_.get(), Qt::Key_Return);
        QTest::keyClicks(canvas_.get(), "v:3 m");
        QTest::keyClick(canvas_.get(), Qt::Key_Return);

        // Only the display is normalized; the source keeps the spacing as typed.
        const auto& regions = sheet().regions();
        QCOMPARE(regions.size(), 2u);
        QCOMPARE(source(regions[0]), QStringLiteral("w:= 2 kN/m"));
        QCOMPARE(source(regions[1]), QStringLiteral("v:=3 m"));
        QCOMPARE(QString::fromStdString(regions[0].result.display), QStringLiteral("w := 2 kN/m"));
        QCOMPARE(QString::fromStdString(regions[1].result.display), QStringLiteral("v := 3 m"));

        // Clicking places the caret by source position, unaffected by the display spacing.
        click(regionPoint(sheet().regions()[1], 4)); // just after "v:=3"
        QTest::keyClicks(canvas_.get(), "0");
        QCOMPARE(source(sheet().regions()[1]), QStringLiteral("v:=30 m"));
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

    void typingUndoesAsOneStep()
    {
        click({72, 480});
        QTest::keyClicks(canvas_.get(), "w: 2 kN/m");
        QCOMPARE(undo()->count(), 1); // the region and everything typed into it

        undo()->undo();
        QVERIFY(sheet().regions().empty());
        undo()->redo();
        QCOMPARE(sheet().regions().size(), 1u);
        QCOMPARE(source(sheet().regions()[0]), QStringLiteral("w:= 2 kN/m"));

        // Redo leaves the caret at the end, and new typing is a step of its own.
        QTest::keyClicks(canvas_.get(), "m");
        QCOMPARE(source(sheet().regions()[0]), QStringLiteral("w:= 2 kN/mm"));
        QCOMPARE(undo()->count(), 2);

        // Moving the caret starts a new step too.
        QTest::keyClick(canvas_.get(), Qt::Key_Home);
        QTest::keyClicks(canvas_.get(), "q");
        undo()->undo();
        QCOMPARE(source(sheet().regions()[0]), QStringLiteral("w:= 2 kN/mm"));
        undo()->undo();
        QCOMPARE(source(sheet().regions()[0]), QStringLiteral("w:= 2 kN/m"));
    }

    void erasingUndoesAsOneStep()
    {
        const int a = sheet().addRegion(72, 120, "x := 125");
        canvas_->worksheetReset();

        click(regionPoint(*sheet().region(a), 8));
        QTest::keyClick(canvas_.get(), Qt::Key_Backspace);
        QTest::keyClick(canvas_.get(), Qt::Key_Backspace);
        QCOMPARE(source(*sheet().region(a)), QStringLiteral("x := 1"));

        undo()->undo();
        QCOMPARE(source(*sheet().region(a)), QStringLiteral("x := 125"));
        undo()->redo();
        QCOMPARE(source(*sheet().region(a)), QStringLiteral("x := 1"));
    }

    void undoBringsBackAnEmptiedRegion()
    {
        const int a = sheet().addRegion(72, 120, "ab");
        canvas_->worksheetReset();

        click(regionPoint(*sheet().region(a), 2));
        QTest::keyClick(canvas_.get(), Qt::Key_Backspace);
        QTest::keyClick(canvas_.get(), Qt::Key_Backspace);
        QTest::keyClick(canvas_.get(), Qt::Key_Escape);
        QVERIFY(sheet().regions().empty());

        // Erasing and the removal it caused are one step.
        QCOMPARE(undo()->count(), 1);
        undo()->undo();
        QCOMPARE(source(*sheet().region(a)), QStringLiteral("ab"));
    }

    void deletingRegionsUndoes()
    {
        const int use = sheet().addRegion(72, 240, "x =");
        const int def = sheet().addRegion(72, 120, "x := 5");
        sheet().addRegion(72, 360, "y := 1");
        canvas_->worksheetReset();

        drag({48, 96}, {300, 264});
        QTest::keyClick(canvas_.get(), Qt::Key_Delete);
        QCOMPARE(sheet().regions().size(), 1u);

        // They come back with their ids and order, selected, and evaluate again.
        undo()->undo();
        QCOMPARE(sheet().regions().size(), 3u);
        QCOMPARE(sheet().regions()[0].id, use);
        QCOMPARE(sheet().regions()[1].id, def);
        QCOMPARE(canvas_->selection(), (QSet<int>{use, def}));
        QCOMPARE(value(*sheet().region(use)), QStringLiteral("5"));

        undo()->redo();
        QCOMPARE(sheet().regions().size(), 1u);
        QVERIFY(canvas_->selection().isEmpty());
    }

    void movingRegionsUndoes()
    {
        const int use = sheet().addRegion(72, 120, "x =");
        const int def = sheet().addRegion(72, 240, "x := 5");
        canvas_->worksheetReset();

        drag(regionPoint(*sheet().region(def), 1), regionPoint(*sheet().region(def), 1) - QPointF(0, 180));
        QCOMPARE(value(*sheet().region(use)), QStringLiteral("5"));
        QCOMPARE(undo()->count(), 1); // one step for the whole drag

        undo()->undo();
        QCOMPARE(sheet().region(def)->y, 240.0);
        QCOMPARE(QString::fromStdString(sheet().region(use)->result.error), QStringLiteral("x is not defined"));

        // Each nudge is its own step.
        QTest::keyClick(canvas_.get(), Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(canvas_.get(), Qt::Key_Right);
        QTest::keyClick(canvas_.get(), Qt::Key_Right);
        undo()->undo();
        QCOMPARE(sheet().region(def)->x, 72.0 + WorksheetCanvas::kGrid);
        QCOMPARE(sheet().region(use)->x, 72.0 + WorksheetCanvas::kGrid);
    }

    void undoingToTheSavedStateClearsModified()
    {
        canvas_.reset();
        MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        QVERIFY(window.openFile(QStringLiteral(LEMMA_EXAMPLES_DIR "/cantilever-beam.lemma")));
        auto* canvas = window.findChild<WorksheetCanvas*>();
        QVERIFY(!window.isWindowModified());

        QTest::mouseClick(canvas->viewport(), Qt::LeftButton, {}, canvas->viewportPosition({72, 960}).toPoint());
        QTest::keyClicks(canvas, "1");
        QVERIFY(window.isWindowModified());

        QTest::keySequence(&window, QKeySequence::Undo);
        QVERIFY(!window.isWindowModified());
        QTest::keySequence(&window, QKeySequence::Redo);
        QVERIFY(window.isWindowModified());

        // Ctrl+Y (Cmd+Y on macOS) redoes on every platform, and is the one the menu shows.
        const auto actions = window.findChildren<QAction*>();
        const auto redo = std::find_if(actions.begin(), actions.end(),
                                       [](QAction* a) { return a->text().startsWith(QStringLiteral("&Redo")); });
        QVERIFY(redo != actions.end());
        QCOMPARE((*redo)->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_Y));
        QTest::keySequence(&window, QKeySequence::Undo);
        QVERIFY(!window.isWindowModified());
        QTest::keyClick(&window, Qt::Key_Y, Qt::ControlModifier);
        QVERIFY(window.isWindowModified());
    }

    void cursorIsAnIBeamOnlyOverRegions()
    {
        const int a = sheet().addRegion(72, 120, "x := 5");
        canvas_->worksheetReset();
        QWidget* viewport = canvas_->viewport();

        QTest::mouseMove(viewport, canvas_->viewportPosition(regionPoint(*sheet().region(a), 1)).toPoint());
        QCOMPARE(viewport->cursor().shape(), Qt::IBeamCursor);
        QTest::mouseMove(viewport, canvas_->viewportPosition({480, 600}).toPoint());
        QCOMPARE(viewport->cursor().shape(), Qt::ArrowCursor);

        // After a drag, the cursor goes back to what is under the mouse.
        drag(regionPoint(*sheet().region(a), 1), regionPoint(*sheet().region(a), 1) + QPointF(0, 120));
        QCOMPARE(viewport->cursor().shape(), Qt::IBeamCursor);
    }

    void quoteStartsATextRegionThatIsntEvaluated()
    {
        click({72, 120});
        QTest::keyClicks(canvas_.get(), "\"x: 5");
        QTest::keyClick(canvas_.get(), Qt::Key_Return); // a new line, not the end of the region
        QTest::keyClicks(canvas_.get(), "then x = 5");
        QTest::keyClick(canvas_.get(), Qt::Key_Escape);
        QTest::keyClicks(canvas_.get(), "x =");

        const auto& regions = sheet().regions();
        QCOMPARE(regions.size(), 2u);
        QCOMPARE(regions[0].kind, lemma::Region::Kind::Text);
        QCOMPARE(source(regions[0]), QStringLiteral("x: 5\nthen x = 5")); // ':' stays as typed in text
        QCOMPARE(regions[0].x, 72.0);
        QCOMPARE(regions[0].y, 120.0);
        QCOMPARE(regions[0].width, WorksheetCanvas::kPageWidth - 2 * WorksheetCanvas::kMargin);
        QCOMPARE(regions[0].result.status, lemma::RegionResult::Status::Empty);

        // Escape put the crosshair below both lines, and the math there doesn't see the text.
        QCOMPARE(regions[1].kind, lemma::Region::Kind::Math);
        QVERIFY(regions[1].y > canvas_->regionRect(regions[0]).bottom());
        QCOMPARE(QString::fromStdString(regions[1].result.error), QStringLiteral("x is not defined"));

        // The text region and everything typed into it is one undo step.
        undo()->undo();
        undo()->undo();
        QVERIFY(sheet().regions().empty());
    }

    void emptyTextRegionLeavesNoTrace()
    {
        click({72, 120});
        QTest::keyClick(canvas_.get(), Qt::Key_QuoteDbl, Qt::ShiftModifier);
        QCOMPARE(sheet().regions().size(), 1u);
        QTest::keyClick(canvas_.get(), Qt::Key_Escape);
        QVERIFY(sheet().regions().empty());
        QCOMPARE(undo()->count(), 0);

        // Undo while it is still empty takes it away too, rather than leaving it behind.
        sheet().addRegion(72, 240, "a := 1");
        canvas_->worksheetReset();
        click(regionPoint(sheet().regions()[0], 6));
        QTest::keyClicks(canvas_.get(), "0");
        click({72, 480});
        QTest::keyClicks(canvas_.get(), "\"");
        undo()->undo();
        QCOMPARE(sheet().regions().size(), 1u);
        QCOMPARE(source(sheet().regions()[0]), QStringLiteral("a := 1"));
    }

    void textWrapsAndCaretMovesByLine()
    {
        const int t = sheet().addTextRegion(72, 120, 120, "one two three four five six\nseven");
        canvas_->worksheetReset();
        const lemma::Region& r = *sheet().region(t);
        // "six" has wrapped below the first line, and "seven" starts a line of its own.
        QVERIFY(canvas_->sourcePosition(r, 24).y() > canvas_->sourcePosition(r, 0).y());
        QVERIFY(canvas_->sourcePosition(r, 28).y() > canvas_->sourcePosition(r, 24).y());
        QCOMPARE(canvas_->sourcePosition(r, 28).x(), 72.0);
        QVERIFY(canvas_->regionRect(r).height() > canvas_->sourcePosition(r, 28).y() - canvas_->sourcePosition(r, 0).y());
        QVERIFY(canvas_->regionRect(r).width() <= 120);

        // Click at the start of "seven", on the last line.
        click(regionPoint(r, 28));
        QTest::keyClicks(canvas_.get(), "_");
        QCOMPARE(source(*sheet().region(t)), QStringLiteral("one two three four five six\n_seven"));

        QTest::keyClick(canvas_.get(), Qt::Key_End);
        QTest::keyClicks(canvas_.get(), "!");
        QCOMPARE(source(*sheet().region(t)), QStringLiteral("one two three four five six\n_seven!"));

        // Up goes to the line above (the end of the wrapped text), not to the start of the region.
        QTest::keyClick(canvas_.get(), Qt::Key_Up);
        QTest::keyClick(canvas_.get(), Qt::Key_End);
        QTest::keyClicks(canvas_.get(), "?");
        QCOMPARE(source(*sheet().region(t)), QStringLiteral("one two three four five six?\n_seven!"));
        QTest::keyClick(canvas_.get(), Qt::Key_Home);
        QTest::keyClick(canvas_.get(), Qt::Key_Down);
        QTest::keyClick(canvas_.get(), Qt::Key_Down);
        QTest::keyClick(canvas_.get(), Qt::Key_Home);
        QTest::keyClicks(canvas_.get(), "#");
        QCOMPARE(source(*sheet().region(t)), QStringLiteral("one two three four five six?\n#_seven!"));
    }

    void draggingTheRightEdgeSetsTheWrapWidth()
    {
        const int t = sheet().addTextRegion(72, 120, 120, "one two three four five six");
        canvas_->worksheetReset();
        const QRectF before = canvas_->regionRect(*sheet().region(t));

        const QPointF edge(before.right() + 3, before.center().y());
        QTest::mouseMove(canvas_->viewport(), canvas_->viewportPosition(edge).toPoint());
        QCOMPARE(canvas_->viewport()->cursor().shape(), Qt::SizeHorCursor);
        drag(edge, {72 + 600 + 3, before.center().y()});

        const lemma::Region& r = *sheet().region(t);
        QCOMPARE(r.width, 600.0);
        QCOMPARE(r.x, 72.0); // resized, not moved
        QVERIFY(canvas_->regionRect(r).height() < before.height()); // now on one line
        QCOMPARE(source(r), QStringLiteral("one two three four five six"));

        // Never narrower than the minimum.
        drag({canvas_->regionRect(r).right() + 3, before.center().y()}, {0, before.center().y()});
        QCOMPARE(sheet().region(t)->width, WorksheetCanvas::kMinTextWidth);

        undo()->undo();
        QCOMPARE(sheet().region(t)->width, 600.0);
        undo()->undo();
        QCOMPARE(sheet().region(t)->width, 120.0);

        // Dragging inside the region moves it, like a math region.
        drag(regionPoint(*sheet().region(t), 2), regionPoint(*sheet().region(t), 2) + QPointF(0, 120));
        QCOMPARE(sheet().region(t)->y, 240.0);
        QCOMPARE(sheet().region(t)->width, 120.0);
    }

    void saveAndLoadTextRegions()
    {
        sheet().addTextRegion(72, 96, 300, "Cantilever check\nAssumes a uniform load.");
        sheet().addRegion(72, 168, "w := 2 kN/m");
        sheet().evaluate();

        QTemporaryDir dir;
        const QString path = dir.filePath("text.lemma");
        QString error;
        QVERIFY2(DocumentIO::save(sheet(), path, &error), qPrintable(error));

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(root.value("version").toInt(), 2); // version 1 would silently drop text regions
        QCOMPARE(root.value("regions").toArray()[0].toObject().value("type").toString(), QStringLiteral("text"));

        lemma::Worksheet loaded;
        QVERIFY2(DocumentIO::load(loaded, path, &error), qPrintable(error));
        QCOMPARE(loaded.regions().size(), 2u);
        const lemma::Region& text = loaded.regions()[0];
        QCOMPARE(text.kind, lemma::Region::Kind::Text);
        QCOMPARE(source(text), QStringLiteral("Cantilever check\nAssumes a uniform load."));
        QCOMPARE(text.x, 72.0);
        QCOMPARE(text.y, 96.0);
        QCOMPARE(text.width, 300.0);
        QCOMPARE(loaded.regions()[1].kind, lemma::Region::Kind::Math);
    }

    void loadsVersion1Files()
    {
        QTemporaryDir dir;
        QFile f(dir.filePath("v1.lemma"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(R"({"format": "lemma-worksheet", "version": 1,
                    "regions": [{"type": "math", "x": 72, "y": 96, "source": "2 m + 1 m ="}]})");
        f.close();

        lemma::Worksheet ws;
        QString error;
        QVERIFY2(DocumentIO::load(ws, f.fileName(), &error), qPrintable(error));
        QCOMPARE(ws.regions().size(), 1u);
        QCOMPARE(value(ws.regions()[0]), QStringLiteral("3 m"));
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
        sheet().addTextRegion(72, 2200, 300, "A note\non page 3");
        canvas_->worksheetReset();
        QCOMPARE(canvas_->pageCount(), 3);

        QTemporaryDir dir;
        const QString path = dir.filePath("out.pdf");
        QVERIFY(exportPdf(*canvas_, path, "test"));

        QFile pdf(path);
        QVERIFY(pdf.open(QIODevice::ReadOnly));
        const QByteArray bytes = pdf.readAll();
        QVERIFY(bytes.startsWith("%PDF"));
        QCOMPARE(bytes.count("/Type /Page\n") + bytes.count("/Type /Page\r") + bytes.count("/Type /Page "), 3);
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

        click({480, 192}); // beside the math, so it wraps at the right margin
        QTest::keyClicks(canvas_.get(), "\"Deflection under a uniform load, from the beam tables. "
                                        "Check it against L/240.");
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
    QUndoStack* undo() { return canvas_->undoStack(); }

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
