#ifndef SERVER_COMPONENTS_H
#define SERVER_COMPONENTS_H

#include <QObject>
#include <QThread>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QTimer>
#include <QMutex>
#include <QImage>
#include <QCoreApplication>
#include <QScrollBar>
#include <vulkan/vulkan.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#undef None
#undef KeyPress
#undef KeyRelease
#undef ButtonPress
#undef ButtonRelease
#undef MotionNotify
#undef DestroyNotify
#include <alsa/asoundlib.h>
#include <functional>
#include <vector>
#include <atomic>

#include "../common/protocol.h"
#include "../common/H265Codec.h"

// ============================================================
// Network Server (UDP Video/Audio + TCP Control)
// ============================================================

class NetworkServer : public QObject {
    Q_OBJECT

public:
    explicit NetworkServer(QObject *parent = nullptr);
    ~NetworkServer();

    bool start(uint16_t udpPort = DEFAULT_UDP_PORT, uint16_t tcpPort = DEFAULT_TCP_PORT);
    void stop();
    bool isRunning() const { return running_.load(); }

    // Send video frame to all clients
    void sendVideoFrame(const uint8_t* data, size_t size);
    // Send audio to all clients
    void sendAudio(const uint8_t* data, size_t size);

    int getClientCount() const;

    // Set control message handler
    void setControlCallback(std::function<void(const ControlMessage&)> cb);

signals:
    void clientConnected();
    void clientDisconnected();
    void logMessage(const QString& msg);

private slots:
    void onNewTcpConnection();
    void onTcpReadyRead();
    void onTcpDisconnected();

private:
    void sendToAllClients(const uint8_t* data, size_t size);

    uint16_t udpPort_;
    uint16_t tcpPort_;

    QUdpSocket* udpSocket_;
    QTcpServer* tcpServer_;
    QList<QTcpSocket*> tcpClients_;
    QList<QHostAddress> clientAddresses_;

    std::atomic<bool> running_;
    mutable QMutex mutex_;

    std::function<void(const ControlMessage&)> controlCallback_;
};

// ============================================================
// Virtual Sound Card (ALSA)
// ============================================================

class VirtualSoundCard : public QObject {
    Q_OBJECT

public:
    explicit VirtualSoundCard(QObject *parent = nullptr);
    ~VirtualSoundCard();

    bool initialize();
    void start(NetworkServer* server);
    void stop();
    bool isRunning() const { return running_.load(); }

signals:
    void logMessage(const QString& msg);

private:
    void audioLoop();

    NetworkServer* server_ = nullptr;
    std::atomic<bool> running_;
    QThread* audioThread_ = nullptr;
    snd_pcm_t* pcmHandle_ = nullptr;

    unsigned int sampleRate_ = 48000;
    unsigned int channels_ = 2;
    snd_pcm_uframes_t bufferSize_ = 1024;
    snd_pcm_uframes_t periodSize_ = 256;
};

// ============================================================
// Display Driver (Vulkan + X11)
// ============================================================

class DisplayDriver : public QObject {
    Q_OBJECT

public:
    explicit DisplayDriver(QObject *parent = nullptr);
    ~DisplayDriver();

    bool initialize(int width = 1920, int height = 1080, int targetFps = 60);
    void start(NetworkServer* server);
    void stop();
    bool isRunning() const { return running_.load(); }

    void handleControl(const ControlMessage& msg);

signals:
    void logMessage(const QString& msg);
    void fpsUpdate(double fps, int frameCount);

private:
    void renderLoop();
    bool createWindow();
    bool initVulkan();
    bool createInstance();
    bool createSurface();
    bool selectPhysicalDevice();
    bool createLogicalDevice();
    bool createSwapchain();
    bool createReadbackResources();
    void renderAndReadback(uint32_t imageIndex, uint32_t frameCount);
    void processX11Events();

    const char** getRequiredExtensions(uint32_t& count);
    const char** getDeviceExtensions(uint32_t& count);

    NetworkServer* server_ = nullptr;
    H265Encoder encoder_;

    std::atomic<bool> running_;
    QThread* renderThread_ = nullptr;

    int width_ = 1920;
    int height_ = 1080;
    int targetFps_ = 60;

    // Vulkan
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily_ = UINT32_MAX;

    Display* display_ = nullptr;
    Window window_ = 0;

    std::vector<VkImage> swapchainImages_;
    VkFormat swapchainImageFormat_;
    VkExtent2D swapchainExtent_;

    VkBuffer stagingBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory_ = VK_NULL_HANDLE;
};

#endif // SERVER_COMPONENTS_H
