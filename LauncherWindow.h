#ifndef LAUNCHER_WINDOW_H
#define LAUNCHER_WINDOW_H

#include <QWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QLabel>

class LauncherWindow : public QWidget {
    Q_OBJECT

public:
    LauncherWindow(QWidget *parent = nullptr) : QWidget(parent) {
        setWindowTitle("投屏系统启动器");
        setFixedSize(300, 200);

        auto *layout = new QVBoxLayout(this);
        auto *title = new QLabel("请选择启动模式", this);
        title->setAlignment(Qt::AlignCenter);
        
        QPushButton *serverBtn = new QPushButton("启动服务端 (Server)", this);
        QPushButton *clientBtn = new QPushButton("启动客户端 (Client)", this);

        layout->addWidget(title);
        layout->addWidget(serverBtn);
        layout->addWidget(clientBtn);

        connect(serverBtn, &QPushButton::clicked, this, &LauncherWindow::launchServer);
        connect(clientBtn, &QPushButton::clicked, this, &LauncherWindow::launchClient);
    }

signals:
    void modeSelected(int mode); // 1: Server, 2: Client

private slots:
    void launchServer() { emit modeSelected(1); close(); }
    void launchClient() { emit modeSelected(2); close(); }
};

#endif // LAUNCHER_WINDOW_H
