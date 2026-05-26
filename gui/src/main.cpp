#include "MainWindow.hpp"

#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName("OVL Extractor");
    app.setOrganizationName("OVLExtractor-3");

    ovlgui::MainWindow w;
    w.show();

    if (argc >= 2) {
        w.openOvl(QString::fromLocal8Bit(argv[1]));
    }

    return app.exec();
}
