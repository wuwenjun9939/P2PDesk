#ifndef CLIENT_MAINWINDOW_H
#define CLIENT_MAINWINDOW_H

#include <QMainWindow>
#include <QTextEdit>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QSpinBox>
#include <QTimer>
#include <QWidget>

class NetworkClient;
class AudioPlayer;
class VideoRenderer;

class ClientWindow : public QMainWindow {
    Q_OBJECT

public:
    ClientWindow(QWidget *parent = nullptr);
    ~ClientWindow();

    // Called when frame is received
    void onFrameReceived(uint32_t frameNumber, uint32_t width, uint32_t height,
                         uint8_t codec, const uint8_t* data, size_t dataSize);
    // Called when audio is received
    void onAudioReceived(uint32_t sampleRate, uint32_t channels,
                         const int16_t* samples, uint32_t sampleCount);

private slots:
    void onConnectDisconnect();
    void updateStatus();

private:
    void setupUI();
    void appendLog(const QString& msg, const QColor& color = Qt::black);
    bool connectToServer();
    void disconnectFromServer();

    // UI Components
    QWidget *videoWidget_;         // Area to display video frames
    QLabel *statusLabel_;
    QLabel *fpsLabel_;
    QLabel *resolutionLabel_;
    QPushButton *connectButton_;
    QPushButton *exitButton_;
    QLineEdit *serverEdit_;
    QSpinBox *portSpin_;
    QTextEdit *logEdit_;
    QTimer *statusTimer_;

    // Client components
    NetworkClient *network_;
    AudioPlayer *audio_;
    VideoRenderer *renderer_;
    bool connected_;
    uint32_t fpsCounter_;
};

#endif // CLIENT_MAINWINDOW_H
