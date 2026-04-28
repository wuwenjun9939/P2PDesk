#ifndef CLIENT_COMPONENTS_H
#define CLIENT_COMPONENTS_H

#include <QObject>
#include <QThread>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QTimer>
#include <QMutex>
#include <QImage>
#include <QCoreApplication>
#include <alsa/asoundlib.h>
#include <functional>
#include <vector>
#include <atomic>

#include "../common/protocol.h"
#include "../common/H265Codec.h"

// ============================================================
// Network Client (UDP Video/Audio + TCP Control)
// ============================================================

class NetworkClient : public QObject {
    Q_OBJECT

public:
    explicit NetworkClient(QObject *parent = nullptr);
    ~NetworkClient();

    bool connectToServer(const QString& host, uint16_t udpPort = DEFAULT_UDP_PORT, 
                        uint16_t tcpPort = DEFAULT_TCP_PORT);
    void disconnectFromServer();
    bool isConnected() const { return connected_.load(); }

    // Send control message
    void sendControl(const ControlMessage& msg);
    void sendPing();
    void sendShutdown();

    // Set callbacks
    void setFrameCallback(std::function<void(uint32_t, uint32_t, uint32_t, uint8_t, const uint8_t*, size_t)> cb) {
        frameCallback_ = cb;
    }
    void setAudioCallback(std::function<void(uint32_t, uint32_t, const int16_t*, uint32_t)> cb) {
        audioCallback_ = cb;
    }

signals:
    void connected();
    void disconnected();
    void logMessage(const QString& msg);

private slots:
    void onUdpReadyRead();
    void onTcpReadyRead();
    void onTcpDisconnected();
    void onTcpConnected();

private:
    QString host_;
    uint16_t udpPort_;
    uint16_t tcpPort_;

    QUdpSocket* udpSocket_;
    QTcpSocket* tcpSocket_;

    std::atomic<bool> connected_;
    mutable QMutex mutex_;

    std::function<void(uint32_t, uint32_t, uint32_t, uint8_t, const uint8_t*, size_t)> frameCallback_;
    std::function<void(uint32_t, uint32_t, const int16_t*, uint32_t)> audioCallback_;

    // Reassembly buffer for video frames
    QByteArray udpBuffer_;
};

// ============================================================
// Audio Player (ALSA)
// ============================================================

class AudioPlayer : public QObject {
    Q_OBJECT

public:
    explicit AudioPlayer(QObject *parent = nullptr);
    ~AudioPlayer();

    bool initialize();
    void start();
    void stop();

    // Queue audio samples for playback
    void queueAudio(const int16_t* samples, uint32_t count, uint32_t channels);

signals:
    void logMessage(const QString& msg);

private:
    void playbackLoop();

    snd_pcm_t* pcmHandle_ = nullptr;
    std::atomic<bool> running_;
    QThread* playbackThread_ = nullptr;

    QMutex queueMutex_;
    std::vector<int16_t> audioQueue_;
};

// ============================================================
// Video Renderer (Qt-based)
// ============================================================

class VideoRenderer : public QObject {
    Q_OBJECT

public:
    explicit VideoRenderer(QObject *parent = nullptr);
    ~VideoRenderer();

    bool initialize();
    
    // Process incoming frame data
    QImage processFrame(uint32_t frameNumber, uint32_t width, uint32_t height,
                       uint8_t codec, const uint8_t* data, size_t dataSize);

    // Get current image
    QImage currentImage() const { return currentImage_; }

signals:
    void logMessage(const QString& msg);
    void frameReady(const QImage& image);

private:
    H265Decoder decoder_;
    QImage currentImage_;
    std::vector<uint32_t> frameBuffer_;
    bool initialized_ = false;
};

#endif // CLIENT_COMPONENTS_H
