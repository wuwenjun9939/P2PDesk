#include "ClientComponents.h"
#include <QDateTime>
#include <QHostAddress>
#include <QImage>
#include <QCoreApplication>
#include <iostream>

// ============================================================
// Network Client Implementation
// ============================================================

NetworkClient::NetworkClient(QObject *parent)
    : QObject(parent)
    , udpPort_(DEFAULT_UDP_PORT)
    , tcpPort_(DEFAULT_TCP_PORT)
    , udpSocket_(nullptr)
    , tcpSocket_(nullptr)
    , connected_(false)
{
}

NetworkClient::~NetworkClient() {
    disconnectFromServer();
}

bool NetworkClient::connectToServer(const QString& host, uint16_t udpPort, uint16_t tcpPort) {
    host_ = host;
    udpPort_ = udpPort;
    tcpPort_ = tcpPort;

    emit logMessage(QString("[Network] Connecting to %1...").arg(host));
    emit logMessage(QString("[Network]   UDP (video/audio): port %1").arg(udpPort));
    emit logMessage(QString("[Network]   TCP (control):     port %1").arg(tcpPort));

    // Create UDP socket
    udpSocket_ = new QUdpSocket(this);
    connect(udpSocket_, &QUdpSocket::readyRead, this, &NetworkClient::onUdpReadyRead);
    if (!udpSocket_->bind(QHostAddress::Any, 0, QAbstractSocket::ShareAddress | QAbstractSocket::ReuseAddressHint)) {
        emit logMessage("[Network] Warning: Failed to bind UDP socket");
    }

    // Create TCP socket and connect
    tcpSocket_ = new QTcpSocket(this);
    connect(tcpSocket_, &QTcpSocket::connected, this, &NetworkClient::onTcpConnected);
    connect(tcpSocket_, &QTcpSocket::readyRead, this, &NetworkClient::onTcpReadyRead);
    connect(tcpSocket_, &QTcpSocket::disconnected, this, &NetworkClient::onTcpDisconnected);

    tcpSocket_->connectToHost(host, tcpPort);
    if (!tcpSocket_->waitForConnected(5000)) {
        emit logMessage(QString("[Network] Failed to connect: %1").arg(tcpSocket_->errorString()));
        delete udpSocket_;
        delete tcpSocket_;
        udpSocket_ = nullptr;
        tcpSocket_ = nullptr;
        return false;
    }

    connected_.store(true);
    emit logMessage("[Network] Connected (dual-channel)");
    emit connected();
    return true;
}

void NetworkClient::disconnectFromServer() {
    if (!connected_.load()) return;
    connected_.store(false);

    emit logMessage("[Network] Disconnecting...");

    if (udpSocket_) {
        udpSocket_->close();
        delete udpSocket_;
        udpSocket_ = nullptr;
    }

    if (tcpSocket_) {
        tcpSocket_->disconnectFromHost();
        tcpSocket_->waitForDisconnected(1000);
        delete tcpSocket_;
        tcpSocket_ = nullptr;
    }

    emit logMessage("[Network] Disconnected");
    emit disconnected();
}

void NetworkClient::sendControl(const ControlMessage& msg) {
    if (!tcpSocket_ || !connected_.load()) return;
    tcpSocket_->write(reinterpret_cast<const char*>(&msg), sizeof(msg));
}

void NetworkClient::sendPing() {
    ControlMessage msg;
    msg.type = PING;
    msg.payload = 0;
    sendControl(msg);
}

void NetworkClient::sendShutdown() {
    ControlMessage msg;
    msg.type = SHUTDOWN;
    msg.payload = 0;
    sendControl(msg);
}

void NetworkClient::onUdpReadyRead() {
    if (!udpSocket_ || !connected_.load()) return;

    while (udpSocket_->hasPendingDatagrams()) {
        qint64 size = udpSocket_->pendingDatagramSize();
        QByteArray buffer(size, 0);
        QHostAddress sender;
        quint16 senderPort;
        udpSocket_->readDatagram(buffer.data(), buffer.size(), &sender, &senderPort);

        if (buffer.size() < static_cast<int>(sizeof(FrameHeader))) continue;

        // Parse header
        uint32_t magic = reinterpret_cast<const uint32_t*>(buffer.constData())[0];

        if (magic == FRAME_MAGIC) {
            const FrameHeader* header = reinterpret_cast<const FrameHeader*>(buffer.constData());
            size_t dataSize = buffer.size() - sizeof(FrameHeader);

            if (frameCallback_) {
                frameCallback_(header->frame_number, header->width, header->height,
                              header->codec,
                              reinterpret_cast<const uint8_t*>(buffer.constData() + sizeof(FrameHeader)),
                              dataSize);
            }
        } else if (magic == AUDIO_MAGIC) {
            const AudioHeader* header = reinterpret_cast<const AudioHeader*>(buffer.constData());
            size_t sampleDataSize = buffer.size() - sizeof(AudioHeader);
            uint32_t sampleCount = header->sample_count;
            uint32_t channels = header->channels;

            if (audioCallback_ && sampleDataSize == sampleCount * channels * sizeof(int16_t)) {
                audioCallback_(header->sample_rate, channels,
                              reinterpret_cast<const int16_t*>(buffer.constData() + sizeof(AudioHeader)),
                              sampleCount);
            }
        }
    }
}

void NetworkClient::onTcpReadyRead() {
    // Handle TCP responses (PONG, etc.)
    QByteArray data = tcpSocket_->readAll();
    const char* ptr = data.constData();
    int remaining = data.size();

    while (remaining >= static_cast<int>(sizeof(ControlMessage))) {
        const ControlMessage* msg = reinterpret_cast<const ControlMessage*>(ptr);
        
        if (msg->type == PONG) {
            emit logMessage("[Network] Pong received");
        }

        ptr += sizeof(ControlMessage);
        remaining -= sizeof(ControlMessage);
    }
}

void NetworkClient::onTcpConnected() {
    emit logMessage("[Network] TCP connected");
}

void NetworkClient::onTcpDisconnected() {
    emit logMessage("[Network] TCP disconnected");
    connected_.store(false);
    emit disconnected();
}

// ============================================================
// Audio Player Implementation
// ============================================================

AudioPlayer::AudioPlayer(QObject *parent)
    : QObject(parent)
    , running_(false)
{
}

AudioPlayer::~AudioPlayer() {
    stop();
}

bool AudioPlayer::initialize() {
    emit logMessage("[Audio] Initializing audio player...");

    int ret = snd_pcm_open(&pcmHandle_, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (ret < 0) {
        emit logMessage(QString("[Audio] Warning: Could not open PCM device: %1").arg(snd_strerror(ret)));
        pcmHandle_ = nullptr;
        return true;  // Continue without audio
    }

    snd_pcm_hw_params_t *hwParams;
    snd_pcm_hw_params_alloca(&hwParams);

    ret = snd_pcm_hw_params_any(pcmHandle_, hwParams);
    ret = snd_pcm_hw_params_set_access(pcmHandle_, hwParams, SND_PCM_ACCESS_RW_INTERLEAVED);
    ret = snd_pcm_hw_params_set_format(pcmHandle_, hwParams, SND_PCM_FORMAT_S16_LE);
    unsigned int rate = 48000;
    ret = snd_pcm_hw_params_set_rate_near(pcmHandle_, hwParams, &rate, nullptr);
    ret = snd_pcm_hw_params_set_channels(pcmHandle_, hwParams, 2);
    ret = snd_pcm_hw_params(pcmHandle_, hwParams);

    if (ret < 0) {
        emit logMessage(QString("[Audio] Failed to set hardware parameters: %1").arg(snd_strerror(ret)));
        snd_pcm_close(pcmHandle_);
        pcmHandle_ = nullptr;
    } else {
        emit logMessage("[Audio] ALSA device configured");
    }

    return true;
}

void AudioPlayer::start() {
    if (running_.load()) return;

    emit logMessage("[Audio] Starting playback...");
    running_.store(true);

    playbackThread_ = new QThread();
    connect(playbackThread_, &QThread::started, this, &AudioPlayer::playbackLoop);
    playbackThread_->start();
}

void AudioPlayer::stop() {
    if (!running_.load()) return;

    emit logMessage("[Audio] Stopping playback...");
    running_.store(false);

    if (playbackThread_ && playbackThread_->isRunning()) {
        playbackThread_->quit();
        playbackThread_->wait();
        delete playbackThread_;
        playbackThread_ = nullptr;
    }

    if (pcmHandle_) {
        snd_pcm_drain(pcmHandle_);
        snd_pcm_close(pcmHandle_);
        pcmHandle_ = nullptr;
    }
}

void AudioPlayer::queueAudio(const int16_t* samples, uint32_t count, uint32_t channels) {
    QMutexLocker locker(&queueMutex_);
    audioQueue_.insert(audioQueue_.end(), samples, samples + count * channels);

    // Limit queue size to prevent memory overflow
    const size_t maxSamples = 48000 * 2 * 2;
    if (audioQueue_.size() > maxSamples) {
        audioQueue_.erase(audioQueue_.begin(), audioQueue_.begin() + (audioQueue_.size() - maxSamples));
    }
}

void AudioPlayer::playbackLoop() {
    emit logMessage("[Audio] Playback thread started");

    while (running_.load()) {
        QThread::msleep(10);
        QCoreApplication::processEvents();

        std::vector<int16_t> chunk;
        {
            QMutexLocker locker(&queueMutex_);
            if (audioQueue_.empty()) continue;

            const size_t chunkSize = 1024 * 2;
            if (audioQueue_.size() < chunkSize) continue;

            chunk.assign(audioQueue_.begin(), audioQueue_.begin() + chunkSize);
            audioQueue_.erase(audioQueue_.begin(), audioQueue_.begin() + chunkSize);
        }

        if (pcmHandle_ && !chunk.empty()) {
            int frames = snd_pcm_writei(pcmHandle_, chunk.data(), 1024);
            if (frames < 0) {
                snd_pcm_recover(pcmHandle_, frames, 0);
            }
        }
    }

    emit logMessage("[Audio] Playback thread stopped");
}

// ============================================================
// Video Renderer Implementation
// ============================================================

VideoRenderer::VideoRenderer(QObject *parent)
    : QObject(parent)
{
}

VideoRenderer::~VideoRenderer() {
}

bool VideoRenderer::initialize() {
    if (!decoder_.init()) {
        emit logMessage("[Video] Warning: H.265 decoder initialization failed");
        return false;
    }

    frameBuffer_.resize(1920 * 1080);
    initialized_ = true;

    emit logMessage("[Video] H.265 decoder ready");
    return true;
}

QImage VideoRenderer::processFrame(uint32_t frameNumber, uint32_t width, uint32_t height,
                                   uint8_t codec, const uint8_t* data, size_t dataSize) {
    Q_UNUSED(frameNumber);

    if (!initialized_) {
        return QImage();
    }

    // Ensure buffer is large enough
    size_t pixelCount = width * height;
    if (frameBuffer_.size() < pixelCount) {
        frameBuffer_.resize(pixelCount);
    }

    QImage image;

    switch (codec) {
        case 0: // Raw BGRA
            image = QImage(reinterpret_cast<const uchar*>(data), width, height, QImage::Format_ARGB32);
            break;

        case 3: // H.265
            if (decoder_.decode(data, dataSize, frameBuffer_.data(), width, height)) {
                image = QImage(reinterpret_cast<const uchar*>(frameBuffer_.data()),
                             width, height, QImage::Format_ARGB32);
            }
            break;

        default:
            emit logMessage(QString("[Video] Unknown codec: %1").arg(codec));
            break;
    }

    if (!image.isNull()) {
        currentImage_ = image.copy();
        emit frameReady(currentImage_);
    }

    return currentImage_;
}
