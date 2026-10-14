#include "MainWindow.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("Lemma");
    QApplication::setApplicationVersion(LEMMA_VERSION);

    MainWindow window;
    const QStringList args = QApplication::arguments();
    if (args.size() > 1) {
        window.openFile(args.at(1));
    }
    window.show();
    return app.exec();
}
