#ifndef SERVER_MAINWINDOW_H
#define SERVER_MAINWINDOW_H

#include <QMainWindow>
#include <QTextEdit>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QSpinBox>
#include <QTimer>

class NetworkServer;
class VirtualSoundCard;
class DisplayDriver;

class NetworkServer;
class VirtualSoundCard;
class DisplayDriver;

class ServerWindow : public QMainWindow {
    Q_OBJECT

public:
    ServerWindow(QWidget *parent = nullptr);
    ~ServerWindow();

private slots:
    void onStartStop();
    void updateStatus();

private:
    void setupUI();
    void appendLog(const QString& msg, const QColor& color = Qt::black);
    bool startServer();
    void stopServer();

    // UI Components
    QLabel *statusLabel_;
    QLabel *fpsLabel_;
    QLabel *clientLabel_;
    QPushButton *startButton_;
    QPushButton *exitButton_;
    QLineEdit *ipEdit_;
    QSpinBox *portSpin_;
    QSpinBox *widthSpin_;
    QSpinBox *heightSpin_;
    QSpinBox *fpsSpin_;
    QTextEdit *logEdit_;
    QTimer *statusTimer_;

    // Server components
    NetworkServer *network_;
    VirtualSoundCard *soundCard_;
    DisplayDriver *display_;
    bool running_;
};

#endif // SERVER_MAINWINDOW_H
