#include <QApplication>
#include <QNetworkProxy>
#include "LauncherWindow.h"
#include "server_gui/MainWindow.h"
#include "client_gui/MainWindow.h"

int main(int argc, char *argv[]) {
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);

    QApplication app(argc, argv);
    app.setApplicationName("ScreenCast System");
    app.setStyle("Fusion");

    LauncherWindow launcher;
    
    // Use a pointer to store the main window so we can delete it later
    QMainWindow* mainWindow = nullptr;

    QObject::connect(&launcher, &LauncherWindow::modeSelected, [&](int mode) {
        if (mode == 1) {
            mainWindow = new ServerWindow();
        } else {
            mainWindow = new ClientWindow();
        }
        mainWindow->show();
    });

    launcher.show();

    return app.exec();
}