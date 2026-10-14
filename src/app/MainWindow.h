#pragma once

#include <QMainWindow>

class WorksheetCanvas;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    bool openFile(const QString& path);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void newFile();
    void open();
    bool save();
    bool saveAs();
    void exportPdf();
    void showQuickReference();
    void showAbout();

    bool saveTo(const QString& path);
    bool maybeSave();
    void setCurrentPath(const QString& path);

    WorksheetCanvas* canvas_;
    QString currentPath_;
};
