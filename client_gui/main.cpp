#include <QApplication>
#include <QNetworkProxy>
#include "MainWindow.h"

int main(int argc, char *argv[]) {
    // Disable system proxy for all network operations
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);

    QApplication app(argc, argv);
    app.setApplicationName("ScreenCast Client");
    app.setStyle("Fusion");

    ClientWindow window;
    window.show();

    return app.exec();
}
