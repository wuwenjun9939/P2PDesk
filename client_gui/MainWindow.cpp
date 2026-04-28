#include "MainWindow.h"
#include "ClientComponents.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QFormLayout>
#include <QMessageBox>
#include <QDateTime>
#include <QApplication>
#include <QPaintEvent>
#include <QPainter>
#include <QImage>
#include <QScrollBar>

#include "../common/protocol.h"

// Video display widget
class VideoDisplay : public QWidget {
    Q_OBJECT
public:
    VideoDisplay(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumSize(640, 360);
        setStyleSheet("background-color: black;");
    }

public slots:
    void updateFrame(const QImage& image) {
        currentImage_ = image;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        
        if (!currentImage_.isNull()) {
            // Scale image to fit widget while keeping aspect ratio
            QImage scaled = currentImage_.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            
            // Center the image
            int x = (width() - scaled.width()) / 2;
            int y = (height() - scaled.height()) / 2;
            painter.drawImage(x, y, scaled);
        } else {
            painter.setPen(Qt::white);
            painter.drawText(rect(), Qt::AlignCenter, "No Signal\n\nConnect to a server to start receiving video");
        }
    }

private:
    QImage currentImage_;
};

ClientWindow::ClientWindow(QWidget *parent)
    : QMainWindow(parent)
    , videoWidget_(new VideoDisplay())
    , statusLabel_(new QLabel("Status: Disconnected"))
    , fpsLabel_(new QLabel("FPS: --"))
    , resolutionLabel_(new QLabel("Resolution: --"))
    , connectButton_(new QPushButton("Connect"))
    , exitButton_(new QPushButton("Exit"))
    , serverEdit_(new QLineEdit("127.0.0.1"))
    , portSpin_(new QSpinBox())
    , logEdit_(new QTextEdit())
    , statusTimer_(new QTimer(this))
    , network_(nullptr)
    , audio_(nullptr)
    , renderer_(nullptr)
    , connected_(false)
    , fpsCounter_(0)
{
    setWindowTitle("ScreenCast Client - H.265");
    resize(1000, 700);

    // Initialize components
    network_ = new NetworkClient(this);
    audio_ = new AudioPlayer(this);
    renderer_ = new VideoRenderer(this);

    // Connect signals
    connect(network_, &NetworkClient::logMessage, this, [this](const QString& msg) {
        appendLog(msg, Qt::black);
    });
    connect(audio_, &AudioPlayer::logMessage, this, [this](const QString& msg) {
        appendLog(msg, Qt::black);
    });
    connect(renderer_, &VideoRenderer::logMessage, this, [this](const QString& msg) {
        appendLog(msg, Qt::black);
    });
    connect(renderer_, &VideoRenderer::frameReady, this, [this](const QImage& img) {
        static_cast<VideoDisplay*>(videoWidget_)->updateFrame(img);
    });
    connect(network_, &NetworkClient::connected, this, [this]() {
        appendLog("Connected to server", Qt::green);
    });
    connect(network_, &NetworkClient::disconnected, this, [this]() {
        appendLog("Disconnected from server", Qt::red);
    });

    setupUI();

    connect(connectButton_, &QPushButton::clicked, this, &ClientWindow::onConnectDisconnect);
    connect(exitButton_, &QPushButton::clicked, this, &QMainWindow::close);
    connect(statusTimer_, &QTimer::timeout, this, &ClientWindow::updateStatus);
    statusTimer_->start(1000);

    appendLog("Client GUI ready", Qt::blue);
    appendLog("Codec: H.265 (HEVC) decoding", Qt::magenta);
}

ClientWindow::~ClientWindow() {
    if (connected_) {
        disconnectFromServer();
    }
}

void ClientWindow::setupUI() {
    QWidget *central = new QWidget(this);
    setCentralWidget(central);
    auto *mainLayout = new QVBoxLayout(central);

    // Top bar with video display and controls side by side
    auto *topLayout = new QHBoxLayout();

    // Video display (left side, takes more space)
    auto *videoGroup = new QGroupBox("Remote Display");
    auto *videoLayout = new QVBoxLayout(videoGroup);
    videoLayout->addWidget(videoWidget_);
    topLayout->addWidget(videoGroup, 3);

    // Controls (right side)
    auto *controlPanel = new QWidget();
    controlPanel->setMaximumWidth(300);
    auto *controlLayout = new QVBoxLayout(controlPanel);

    // Status
    auto *statusGroup = new QGroupBox("Status");
    auto *statusLayout = new QVBoxLayout(statusGroup);
    statusLabel_->setStyleSheet("font-weight: bold; color: red;");
    statusLayout->addWidget(statusLabel_);
    statusLayout->addWidget(fpsLabel_);
    statusLayout->addWidget(resolutionLabel_);
    controlLayout->addWidget(statusGroup);

    // Connection settings
    auto *connGroup = new QGroupBox("Connection");
    auto *formLayout = new QFormLayout(connGroup);

    serverEdit_->setToolTip("Server IP address");
    formLayout->addRow("Server:", serverEdit_);

    portSpin_->setRange(1024, 65535);
    portSpin_->setValue(DEFAULT_UDP_PORT);
    formLayout->addRow("Port:", portSpin_);

    controlLayout->addWidget(connGroup);

    // Buttons
    connectButton_->setMinimumHeight(40);
    connectButton_->setStyleSheet("font-size: 14px; font-weight: bold;");
    exitButton_->setMinimumHeight(40);
    controlLayout->addWidget(connectButton_);
    controlLayout->addWidget(exitButton_);
    controlLayout->addStretch();

    topLayout->addWidget(controlPanel, 1);
    mainLayout->addLayout(topLayout);

    // Log
    auto *logGroup = new QGroupBox("Log");
    auto *logLayout = new QVBoxLayout(logGroup);
    logEdit_->setReadOnly(true);
    logEdit_->setMaximumHeight(150);
    logLayout->addWidget(logEdit_);
    mainLayout->addWidget(logGroup);
}

void ClientWindow::appendLog(const QString& msg, const QColor& color) {
    QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    logEdit_->append(QString("<font color='%1'>[%2] %3</font>")
        .arg(color.name()).arg(timestamp).arg(msg));
    
    // Auto scroll to bottom
    logEdit_->verticalScrollBar()->setValue(logEdit_->verticalScrollBar()->maximum());
}

void ClientWindow::onConnectDisconnect() {
    if (connected_) {
        disconnectFromServer();
    } else {
        if (connectToServer()) {
            connected_ = true;
            connectButton_->setText("Disconnect");
            statusLabel_->setText("Status: Connected");
            statusLabel_->setStyleSheet("font-weight: bold; color: green;");
        }
    }
}

void ClientWindow::updateStatus() {
    fpsLabel_->setText(QString("FPS: %1").arg(fpsCounter_));
    fpsCounter_ = 0;
}

void ClientWindow::onFrameReceived(uint32_t frameNumber, uint32_t width, uint32_t height,
                                    uint8_t codec, const uint8_t* data, size_t dataSize) {
    if (renderer_) {
        renderer_->processFrame(frameNumber, width, height, codec, data, dataSize);
    }
    fpsCounter_++;
    resolutionLabel_->setText(QString("Resolution: %1x%2").arg(width).arg(height));
}

void ClientWindow::onAudioReceived(uint32_t sampleRate, uint32_t channels,
                                    const int16_t* samples, uint32_t sampleCount) {
    if (audio_) {
        audio_->queueAudio(samples, sampleCount, channels);
    }
}

bool ClientWindow::connectToServer() {
    QString serverIP = serverEdit_->text();
    uint16_t port = static_cast<uint16_t>(portSpin_->value());
    uint16_t tcpPort = port + 1;

    appendLog(QString("Connecting to %1:%2...").arg(serverIP).arg(port), Qt::darkYellow);

    // Initialize audio
    if (!audio_->initialize()) {
        appendLog("Warning: Audio initialization failed", Qt::darkYellow);
    }

    // Initialize video renderer
    if (!renderer_->initialize()) {
        appendLog("Warning: Video renderer initialization failed", Qt::darkYellow);
    }

    // Connect to server
    if (!network_->connectToServer(serverIP, port, tcpPort)) {
        appendLog("Failed to connect to server", Qt::red);
        return false;
    }

    // Set callbacks
    network_->setFrameCallback([this](uint32_t frameNum, uint32_t width, uint32_t height,
                                       uint8_t codec, const uint8_t* data, size_t dataSize) {
        onFrameReceived(frameNum, width, height, codec, data, dataSize);
    });

    network_->setAudioCallback([this](uint32_t sampleRate, uint32_t channels,
                                       const int16_t* samples, uint32_t sampleCount) {
        onAudioReceived(sampleRate, channels, samples, sampleCount);
    });

    // Start audio playback
    audio_->start();

    // Start ping timer
    QTimer* pingTimer = new QTimer(this);
    connect(pingTimer, &QTimer::timeout, [this]() {
        if (network_ && network_->isConnected()) {
            network_->sendPing();
        }
    });
    pingTimer->start(5000);

    appendLog("Connected successfully", Qt::green);
    return true;
}

void ClientWindow::disconnectFromServer() {
    appendLog("Disconnecting...", Qt::darkYellow);

    if (network_) {
        network_->sendShutdown();
        network_->disconnectFromServer();
    }

    if (audio_) {
        audio_->stop();
    }

    connected_ = false;
    connectButton_->setText("Connect");
    statusLabel_->setText("Status: Disconnected");
    statusLabel_->setStyleSheet("font-weight: bold; color: red;");
    fpsLabel_->setText("FPS: --");
    resolutionLabel_->setText("Resolution: --");
}

#include "MainWindow.moc"
