#include "MainWindow.h"
#include "ServerComponents.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QFormLayout>
#include <QMessageBox>
#include <QDateTime>
#include <QApplication>
#include <QScreen>
#include <QScrollBar>

#include "../common/protocol.h"

ServerWindow::ServerWindow(QWidget *parent)
    : QMainWindow(parent)
    , statusLabel_(new QLabel("Status: Stopped"))
    , fpsLabel_(new QLabel("FPS: --"))
    , clientLabel_(new QLabel("Clients: 0"))
    , startButton_(new QPushButton("Start"))
    , exitButton_(new QPushButton("Exit"))
    , ipEdit_(new QLineEdit("0.0.0.0"))
    , portSpin_(new QSpinBox())
    , widthSpin_(new QSpinBox())
    , heightSpin_(new QSpinBox())
    , fpsSpin_(new QSpinBox())
    , logEdit_(new QTextEdit())
    , statusTimer_(new QTimer(this))
    , network_(nullptr)
    , soundCard_(nullptr)
    , display_(nullptr)
    , running_(false)
{
    setWindowTitle("ScreenCast Server - H.265");
    resize(600, 700);

    setupUI();

    // Initialize components
    network_ = new NetworkServer(this);
    soundCard_ = new VirtualSoundCard(this);
    display_ = new DisplayDriver(this);

    // Connect signals
    connect(network_, &NetworkServer::logMessage, this, [this](const QString& msg) {
        appendLog(msg, Qt::black);
    });
    connect(soundCard_, &VirtualSoundCard::logMessage, this, [this](const QString& msg) {
        appendLog(msg, Qt::black);
    });
    connect(display_, &DisplayDriver::logMessage, this, [this](const QString& msg) {
        appendLog(msg, Qt::black);
    });
    connect(display_, &DisplayDriver::fpsUpdate, this, [this](double fps, int frameCount) {
        fpsLabel_->setText(QString("FPS: %1 | Frame: %2").arg(fps, 0, 'f', 1).arg(frameCount));
    });
    connect(network_, &NetworkServer::clientConnected, this, [this]() {
        appendLog("Client connected", Qt::green);
    });
    connect(network_, &NetworkServer::clientDisconnected, this, [this]() {
        appendLog("Client disconnected", Qt::darkYellow);
    });

    // Wire control callback
    network_->setControlCallback([this](const ControlMessage& msg) {
        display_->handleControl(msg);
    });

    connect(startButton_, &QPushButton::clicked, this, &ServerWindow::onStartStop);
    connect(exitButton_, &QPushButton::clicked, this, &QMainWindow::close);
    connect(statusTimer_, &QTimer::timeout, this, &ServerWindow::updateStatus);
    statusTimer_->start(1000);

    appendLog("Server GUI ready", Qt::blue);
    appendLog("Codec: H.265 (HEVC) with hardware acceleration", Qt::magenta);
}

ServerWindow::~ServerWindow() {
    if (running_) {
        stopServer();
    }
}

void ServerWindow::setupUI() {
    QWidget *central = new QWidget(this);
    setCentralWidget(central);
    auto *mainLayout = new QVBoxLayout(central);

    // Status bar area
    auto *statusGroup = new QGroupBox("Status");
    auto *statusLayout = new QHBoxLayout(statusGroup);
    statusLabel_->setStyleSheet("font-weight: bold; color: red;");
    statusLayout->addWidget(statusLabel_);
    statusLayout->addWidget(fpsLabel_);
    statusLayout->addWidget(clientLabel_);
    statusLayout->addStretch();
    mainLayout->addWidget(statusGroup);

    // Settings
    auto *settingsGroup = new QGroupBox("Settings");
    auto *formLayout = new QFormLayout(settingsGroup);

    ipEdit_->setToolTip("Bind IP address (0.0.0.0 for all interfaces)");
    formLayout->addRow("Bind IP:", ipEdit_);

    portSpin_->setRange(1024, 65535);
    portSpin_->setValue(DEFAULT_UDP_PORT);
    formLayout->addRow("UDP Port:", portSpin_);

    widthSpin_->setRange(640, 3840);
    widthSpin_->setValue(1920);
    widthSpin_->setSingleStep(100);
    formLayout->addRow("Width:", widthSpin_);

    heightSpin_->setRange(480, 2160);
    heightSpin_->setValue(1080);
    heightSpin_->setSingleStep(100);
    formLayout->addRow("Height:", heightSpin_);

    fpsSpin_->setRange(1, 160);
    fpsSpin_->setValue(60);
    formLayout->addRow("Target FPS:", fpsSpin_);

    mainLayout->addWidget(settingsGroup);

    // Log
    auto *logGroup = new QGroupBox("Log");
    auto *logLayout = new QVBoxLayout(logGroup);
    logEdit_->setReadOnly(true);
    logEdit_->setMaximumHeight(300);
    logLayout->addWidget(logEdit_);
    mainLayout->addWidget(logGroup);

    // Buttons
    auto *buttonLayout = new QHBoxLayout();
    startButton_->setMinimumHeight(40);
    startButton_->setStyleSheet("font-size: 16px; font-weight: bold;");
    exitButton_->setMinimumHeight(40);
    buttonLayout->addWidget(startButton_);
    buttonLayout->addWidget(exitButton_);
    mainLayout->addLayout(buttonLayout);
}

void ServerWindow::appendLog(const QString& msg, const QColor& color) {
    QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    logEdit_->append(QString("<font color='%1'>[%2] %3</font>")
        .arg(color.name()).arg(timestamp).arg(msg));
    
    // Auto scroll to bottom
    logEdit_->verticalScrollBar()->setValue(logEdit_->verticalScrollBar()->maximum());
}

void ServerWindow::onStartStop() {
    if (running_) {
        stopServer();
    } else {
        if (startServer()) {
            running_ = true;
            startButton_->setText("Stop");
            statusLabel_->setText("Status: Running");
            statusLabel_->setStyleSheet("font-weight: bold; color: green;");
            appendLog("Server started", Qt::green);
        }
    }
}

void ServerWindow::updateStatus() {
    if (network_) {
        int clients = network_->getClientCount();
        clientLabel_->setText(QString("Clients: %1").arg(clients));
    }
}

bool ServerWindow::startServer() {
    appendLog("Initializing server components...", Qt::darkYellow);

    // Get settings
    uint16_t udpPort = static_cast<uint16_t>(portSpin_->value());
    uint16_t tcpPort = udpPort + 1;
    int width = widthSpin_->value();
    int height = heightSpin_->value();
    int fps = fpsSpin_->value();

    // Start network
    if (!network_->start(udpPort, tcpPort)) {
        appendLog("Failed to start network server", Qt::red);
        return false;
    }

    // Initialize display
    if (!display_->initialize(width, height, fps)) {
        appendLog("Failed to initialize display", Qt::red);
        network_->stop();
        return false;
    }

    // Initialize audio
    if (!soundCard_->initialize()) {
        appendLog("Warning: Audio initialization failed", Qt::darkYellow);
    }

    // Start components
    soundCard_->start(network_);
    display_->start(network_);

    appendLog(QString("Server running on UDP:%1 TCP:%2")
        .arg(udpPort).arg(tcpPort), Qt::green);

    return true;
}

void ServerWindow::stopServer() {
    appendLog("Stopping server...", Qt::darkYellow);

    display_->stop();
    soundCard_->stop();
    network_->stop();

    running_ = false;
    startButton_->setText("Start");
    statusLabel_->setText("Status: Stopped");
    statusLabel_->setStyleSheet("font-weight: bold; color: red;");
    fpsLabel_->setText("FPS: --");
    clientLabel_->setText("Clients: 0");
    appendLog("Server stopped", Qt::red);
}
