#define VK_USE_PLATFORM_XLIB_KHR
#include "ServerComponents.h"
#include <QDateTime>
#include <QHostAddress>
#include <QDataStream>
#include <QNetworkProxy>
#include <iostream>

// ============================================================
// Network Server Implementation
// ============================================================

NetworkServer::NetworkServer(QObject *parent)
    : QObject(parent)
    , udpPort_(DEFAULT_UDP_PORT)
    , tcpPort_(DEFAULT_TCP_PORT)
    , udpSocket_(nullptr)
    , tcpServer_(nullptr)
    , running_(false)
{
}

NetworkServer::~NetworkServer() {
    stop();
}

bool NetworkServer::start(uint16_t udpPort, uint16_t tcpPort) {
    udpPort_ = udpPort;
    tcpPort_ = tcpPort;

    // Disable system proxy for this socket
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);

    // Create UDP socket using raw socket descriptor to avoid proxy issues
    udpSocket_ = new QUdpSocket(this);
    udpSocket_->setProxy(QNetworkProxy::NoProxy);
    
    // Bind with DefaultBindingMode (no proxy)
    if (!udpSocket_->bind(QHostAddress::AnyIPv4, udpPort_, QAbstractSocket::DontShareAddress)) {
        emit logMessage(QString("[Network] Failed to bind UDP port %1: %2").arg(udpPort_).arg(udpSocket_->errorString()));
        emit logMessage("[Network] Trying alternative binding method...");
        
        // Try again with different settings
        udpSocket_->close();
        delete udpSocket_;
        udpSocket_ = new QUdpSocket(this);
        udpSocket_->setProxy(QNetworkProxy::NoProxy);
        
        if (!udpSocket_->bind(QHostAddress::Any, udpPort_)) {
            emit logMessage(QString("[Network] Failed to bind UDP port %1 (second attempt): %2").arg(udpPort_).arg(udpSocket_->errorString()));
            delete udpSocket_;
            udpSocket_ = nullptr;
            return false;
        }
    }
    emit logMessage(QString("[Network] UDP listening on port %1").arg(udpPort_));

    // Create TCP server
    tcpServer_ = new QTcpServer(this);
    connect(tcpServer_, &QTcpServer::newConnection, this, &NetworkServer::onNewTcpConnection);
    if (!tcpServer_->listen(QHostAddress::Any, tcpPort_)) {
        emit logMessage(QString("[Network] Failed to bind TCP port %1").arg(tcpPort_));
        delete tcpServer_;
        tcpServer_ = nullptr;
        delete udpSocket_;
        udpSocket_ = nullptr;
        return false;
    }
    emit logMessage(QString("[Network] TCP listening on port %1").arg(tcpPort_));

    running_.store(true);
    emit logMessage("[Network] Server ready");
    return true;
}

void NetworkServer::stop() {
    if (!running_.load()) return;
    running_.store(false);

    if (tcpServer_) {
        tcpServer_->close();
    }

    QMutexLocker locker(&mutex_);
    for (QTcpSocket* client : tcpClients_) {
        client->disconnectFromHost();
    }
    tcpClients_.clear();
    clientAddresses_.clear();

    emit logMessage("[Network] Server stopped");
}

void NetworkServer::sendVideoFrame(const uint8_t* data, size_t size) {
    if (!udpSocket_ || !running_.load()) return;

    // Create frame header
    FrameHeader header;
    header.magic = FRAME_MAGIC;
    header.frame_number = 0;  // Will be set by caller
    header.width = 0;  // Will be set by caller
    header.height = 0;  // Will be set by caller
    header.codec = 3;  // H.265
    header.fragment_id = 0;
    header.fragment_total = 1;
    header.fragment_size = size;
    header.pixel_offset = 0;

    // Send header
    QByteArray headerData(reinterpret_cast<const char*>(&header), sizeof(header));

    QMutexLocker locker(&mutex_);
    for (int i = 0; i < clientAddresses_.size(); ++i) {
        udpSocket_->writeDatagram(headerData, clientAddresses_[i], udpPort_);
        if (size > 0 && data) {
            QByteArray frameData(reinterpret_cast<const char*>(data), size);
            udpSocket_->writeDatagram(frameData, clientAddresses_[i], udpPort_);
        }
    }
}

void NetworkServer::sendAudio(const uint8_t* data, size_t size) {
    if (!udpSocket_ || !running_.load() || !data || size == 0) return;

    QMutexLocker locker(&mutex_);
    QByteArray audioData(reinterpret_cast<const char*>(data), size);
    for (const QHostAddress& addr : clientAddresses_) {
        udpSocket_->writeDatagram(audioData, addr, udpPort_);
    }
}

int NetworkServer::getClientCount() const {
    QMutexLocker locker(const_cast<QMutex*>(&mutex_));
    return tcpClients_.size();
}

void NetworkServer::setControlCallback(std::function<void(const ControlMessage&)> cb) {
    controlCallback_ = cb;
}

void NetworkServer::onNewTcpConnection() {
    QTcpSocket* client = tcpServer_->nextPendingConnection();
    if (!client) return;

    QHostAddress addr = client->peerAddress();
    emit logMessage(QString("[Network] Control client connected: %1").arg(addr.toString()));

    connect(client, &QTcpSocket::readyRead, this, &NetworkServer::onTcpReadyRead);
    connect(client, &QTcpSocket::disconnected, this, &NetworkServer::onTcpDisconnected);

    QMutexLocker locker(&mutex_);
    tcpClients_.append(client);
    clientAddresses_.append(addr);

    emit clientConnected();
}

void NetworkServer::onTcpReadyRead() {
    QTcpSocket* client = qobject_cast<QTcpSocket*>(sender());
    if (!client) return;

    QByteArray data = client->readAll();
    const char* ptr = data.constData();
    int remaining = data.size();

    while (remaining >= static_cast<int>(sizeof(ControlMessage))) {
        const ControlMessage* msg = reinterpret_cast<const ControlMessage*>(ptr);

        switch (msg->type) {
            case PING: {
                ControlMessage pong;
                pong.type = PONG;
                pong.payload = 0;
                client->write(reinterpret_cast<const char*>(&pong), sizeof(pong));
                break;
            }
            case SHUTDOWN:
                running_.store(false);
                break;
            case MOUSE_MOVE:
            case MOUSE_CLICK:
            case MOUSE_SCROLL:
            case KEY_PRESS:
            case KEY_RELEASE:
            case CONTROL_ENABLE:
            case CONTROL_DISABLE:
                if (controlCallback_) {
                    controlCallback_(*msg);
                }
                break;
            default:
                break;
        }

        ptr += sizeof(ControlMessage);
        remaining -= sizeof(ControlMessage);
    }
}

void NetworkServer::onTcpDisconnected() {
    QTcpSocket* client = qobject_cast<QTcpSocket*>(sender());
    if (!client) return;

    emit logMessage("[Network] Control client disconnected");

    QMutexLocker locker(&mutex_);
    int index = tcpClients_.indexOf(client);
    if (index >= 0) {
        tcpClients_.removeAt(index);
        clientAddresses_.removeAt(index);
    }
    client->deleteLater();

    emit clientDisconnected();
}

// ============================================================
// Virtual Sound Card Implementation
// ============================================================

VirtualSoundCard::VirtualSoundCard(QObject *parent)
    : QObject(parent)
    , running_(false)
{
}

VirtualSoundCard::~VirtualSoundCard() {
    stop();
}

bool VirtualSoundCard::initialize() {
    emit logMessage("[Audio] Initializing virtual sound card...");

    // Try to load snd-aloop module
    system("modprobe snd-aloop 2>/dev/null");

    sampleRate_ = 48000;
    channels_ = 2;
    bufferSize_ = 1024;
    periodSize_ = 256;

    emit logMessage(QString("[Audio] Configuration: %1 Hz, %2 channels, buffer %3")
        .arg(sampleRate_).arg(channels_).arg(bufferSize_));

    int ret = snd_pcm_open(&pcmHandle_, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (ret < 0) {
        emit logMessage(QString("[Audio] Warning: Could not open PCM device: %1").arg(snd_strerror(ret)));
        emit logMessage("[Audio] Will use virtual buffer mode");
        pcmHandle_ = nullptr;
    } else {
        snd_pcm_hw_params_t *hwParams;
        snd_pcm_hw_params_alloca(&hwParams);

        ret = snd_pcm_hw_params_any(pcmHandle_, hwParams);
        ret = snd_pcm_hw_params_set_access(pcmHandle_, hwParams, SND_PCM_ACCESS_RW_INTERLEAVED);
        ret = snd_pcm_hw_params_set_format(pcmHandle_, hwParams, SND_PCM_FORMAT_S16_LE);
        unsigned int rate = sampleRate_;
        ret = snd_pcm_hw_params_set_rate_near(pcmHandle_, hwParams, &rate, nullptr);
        ret = snd_pcm_hw_params_set_channels(pcmHandle_, hwParams, channels_);
        ret = snd_pcm_hw_params(pcmHandle_, hwParams);

        if (ret < 0) {
            emit logMessage(QString("[Audio] Failed to apply hardware parameters: %1").arg(snd_strerror(ret)));
            snd_pcm_close(pcmHandle_);
            pcmHandle_ = nullptr;
        } else {
            emit logMessage("[Audio] ALSA device configured successfully");
        }
    }

    return true;
}

void VirtualSoundCard::start(NetworkServer* server) {
    if (running_.load()) return;
    server_ = server;

    emit logMessage("[Audio] Starting audio generation...");
    running_.store(true);

    audioThread_ = new QThread();
    connect(audioThread_, &QThread::started, this, &VirtualSoundCard::audioLoop);
    audioThread_->start();
}

void VirtualSoundCard::stop() {
    if (!running_.load()) return;

    emit logMessage("[Audio] Stopping...");
    running_.store(false);

    if (audioThread_ && audioThread_->isRunning()) {
        audioThread_->quit();
        audioThread_->wait();
        delete audioThread_;
        audioThread_ = nullptr;
    }

    if (pcmHandle_) {
        snd_pcm_drain(pcmHandle_);
        snd_pcm_close(pcmHandle_);
        pcmHandle_ = nullptr;
    }
}

void VirtualSoundCard::audioLoop() {
    double phase = 0.0;
    const double phaseIncrement = 2.0 * M_PI * 440.0 / sampleRate_;

    std::vector<int16_t> buffer(bufferSize_ * channels_);

    emit logMessage("[Audio] Generating 440Hz test tone (A4)...");

    while (running_.load()) {
        auto startTime = std::chrono::steady_clock::now();

        for (size_t i = 0; i < bufferSize_; ++i) {
            int16_t sample = static_cast<int16_t>(32767.0 * sin(phase));
            phase += phaseIncrement;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;

            buffer[i * channels_] = sample;
            buffer[i * channels_ + 1] = sample;
        }

        if (pcmHandle_) {
            int frames = snd_pcm_writei(pcmHandle_, buffer.data(), bufferSize_);
            if (frames < 0) {
                frames = snd_pcm_recover(pcmHandle_, frames, 0);
            }
        }

        // Stream audio to clients
        if (server_ && server_->isRunning()) {
            AudioHeader audioHdr;
            audioHdr.magic = AUDIO_MAGIC;
            audioHdr.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            audioHdr.sample_rate = static_cast<uint16_t>(sampleRate_);
            audioHdr.channels = static_cast<uint8_t>(channels_);
            audioHdr.sample_count = static_cast<uint8_t>(bufferSize_);

            server_->sendAudio(reinterpret_cast<uint8_t*>(&audioHdr), sizeof(audioHdr));
            server_->sendAudio(reinterpret_cast<uint8_t*>(buffer.data()),
                              bufferSize_ * channels_ * sizeof(int16_t));
        }

        auto endTime = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
        auto expectedDuration = std::chrono::microseconds(
            static_cast<int64_t>(bufferSize_ * 1000000.0 / sampleRate_));

        if (duration < expectedDuration) {
            QThread::usleep((expectedDuration - duration).count());
        }

        // Process Qt events
        QCoreApplication::processEvents();
    }

    emit logMessage("[Audio] Audio thread stopped");
}

// ============================================================
// Display Driver Implementation
// ============================================================

DisplayDriver::DisplayDriver(QObject *parent)
    : QObject(parent)
    , running_(false)
{
}

DisplayDriver::~DisplayDriver() {
    stop();
}

bool DisplayDriver::initialize(int width, int height, int targetFps) {
    width_ = width;
    height_ = height;
    targetFps_ = targetFps;

    emit logMessage(QString("[Display] Initializing: %1x%2 @%3Hz").arg(width).arg(height).arg(targetFps));

    if (!createWindow()) {
        emit logMessage("[Display] Failed to create X11 window");
        return false;
    }

    if (!initVulkan()) {
        emit logMessage("[Display] Failed to initialize Vulkan");
        return false;
    }

    if (!createReadbackResources()) {
        emit logMessage("[Display] Failed to create readback resources");
        return false;
    }

    // Initialize H.265 encoder
    if (!encoder_.init(width, height, targetFps, true)) {
        emit logMessage("[Display] Warning: H.265 encoder initialization failed, using fallback");
    } else {
        emit logMessage("[Display] H.265 encoder ready");
    }

    emit logMessage("[Display] Ready");
    return true;
}

void DisplayDriver::start(NetworkServer* server) {
    if (running_.load()) return;
    server_ = server;

    emit logMessage(QString("[Display] Starting render loop at %1 FPS...").arg(targetFps_));
    running_.store(true);

    renderThread_ = new QThread();
    connect(renderThread_, &QThread::started, this, &DisplayDriver::renderLoop);
    renderThread_->start();
}

void DisplayDriver::stop() {
    if (!running_.load()) return;

    emit logMessage("[Display] Stopping...");
    running_.store(false);

    if (renderThread_ && renderThread_->isRunning()) {
        renderThread_->quit();
        renderThread_->wait();
        delete renderThread_;
        renderThread_ = nullptr;
    }
}

void DisplayDriver::handleControl(const ControlMessage& msg) {
    if (!display_) return;

    switch (msg.type) {
        case MOUSE_MOVE:
            XWarpPointer(display_, 0, window_, 0, 0, 0, 0,
                        msg.mouse_pos.x, msg.mouse_pos.y);
            XFlush(display_);
            break;

        case MOUSE_CLICK: {
            int x = msg.mouse_click.x;
            int y = msg.mouse_click.y;
            uint8_t button = msg.mouse_click.button;

            XWarpPointer(display_, 0, window_, 0, 0, 0, 0, x, y);

            int x11Button = (button == 0) ? Button1 : (button == 1) ? Button3 : Button2;

            if (msg.mouse_click.button & 0x80) {
                XTestFakeButtonEvent(display_, x11Button, False, CurrentTime);
            } else {
                XTestFakeButtonEvent(display_, x11Button, True, CurrentTime);
            }
            XFlush(display_);
            break;
        }

        case MOUSE_SCROLL:
            if (msg.mouse_scroll.delta > 0) {
                XTestFakeButtonEvent(display_, 4, True, CurrentTime);
                XTestFakeButtonEvent(display_, 4, False, CurrentTime);
            } else {
                XTestFakeButtonEvent(display_, 5, True, CurrentTime);
                XTestFakeButtonEvent(display_, 5, False, CurrentTime);
            }
            XFlush(display_);
            break;

        case KEY_PRESS:
        case KEY_RELEASE: {
            KeySym keysym = msg.key.keycode;
            KeyCode keycode = XKeysymToKeycode(display_, keysym);
            bool press = (msg.type == KEY_PRESS);

            XTestFakeKeyEvent(display_, keycode, press, CurrentTime);
            XFlush(display_);
            break;
        }

        default:
            break;
    }
}

bool DisplayDriver::createWindow() {
    display_ = XOpenDisplay(nullptr);
    if (!display_) {
        emit logMessage("[Display] Failed to open X display");
        return false;
    }

    int screen = DefaultScreen(display_);
    Window root = RootWindow(display_, screen);

    window_ = XCreateSimpleWindow(display_, root, 0, 0, width_, height_, 2,
                                  WhitePixel(display_, screen),
                                  BlackPixel(display_, screen));

    XSelectInput(display_, window_, ExposureMask | KeyPressMask | StructureNotifyMask);
    XStoreName(display_, window_, "GPU Display Server - 1080p");

    XSizeHints* sizeHints = XAllocSizeHints();
    sizeHints->flags = PMinSize | PMaxSize;
    sizeHints->min_width = width_;
    sizeHints->max_width = width_;
    sizeHints->min_height = height_;
    sizeHints->max_height = height_;
    XSetWMNormalHints(display_, window_, sizeHints);
    XFree(sizeHints);

    XMapWindow(display_, window_);
    XFlush(display_);

    emit logMessage(QString("[Display] X11 window created: %1x%2").arg(width_).arg(height_));
    return true;
}

bool DisplayDriver::initVulkan() {
    if (!createInstance()) return false;
    if (!createSurface()) return false;
    if (!selectPhysicalDevice()) return false;
    if (!createLogicalDevice()) return false;
    if (!createSwapchain()) return false;

    emit logMessage("[Display] Vulkan initialized");
    return true;
}

bool DisplayDriver::createInstance() {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "GPU Display Server";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "Custom Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    uint32_t extCount = 0;
    const char** extensions = getRequiredExtensions(extCount);
    createInfo.enabledExtensionCount = extCount;
    createInfo.ppEnabledExtensionNames = extensions;

    VkResult result = vkCreateInstance(&createInfo, nullptr, &instance_);
    if (result != VK_SUCCESS) {
        emit logMessage(QString("[Display] Failed to create Vulkan instance: %1").arg(result));
        return false;
    }

    emit logMessage("[Display] Vulkan instance created");
    return true;
}

const char** DisplayDriver::getRequiredExtensions(uint32_t& count) {
    static const char* extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_XLIB_SURFACE_EXTENSION_NAME
    };
    count = 2;
    return extensions;
}

bool DisplayDriver::createSurface() {
    VkXlibSurfaceCreateInfoKHR surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.dpy = display_;
    surfaceInfo.window = window_;

    VkResult result = vkCreateXlibSurfaceKHR(instance_, &surfaceInfo, nullptr, &surface_);
    if (result != VK_SUCCESS) {
        emit logMessage(QString("[Display] Failed to create Vulkan surface: %1").arg(result));
        return false;
    }

    emit logMessage("[Display] Vulkan surface created");
    return true;
}

bool DisplayDriver::selectPhysicalDevice() {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);

    if (deviceCount == 0) {
        emit logMessage("[Display] No Vulkan-capable GPUs found");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

    for (const auto& dev : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);

        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            physicalDevice_ = dev;
            emit logMessage(QString("[Display] Selected GPU: %1").arg(props.deviceName));
            return true;
        }
    }

    physicalDevice_ = devices[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);
    emit logMessage(QString("[Display] Selected GPU (fallback): %1").arg(props.deviceName));
    return true;
}

bool DisplayDriver::createLogicalDevice() {
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, queueFamilies.data());

    graphicsQueueFamily_ = UINT32_MAX;
    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphicsQueueFamily_ = i;
            break;
        }
    }

    if (graphicsQueueFamily_ == UINT32_MAX) {
        return false;
    }

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = graphicsQueueFamily_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    uint32_t extCount;
    const char** extensions = getDeviceExtensions(extCount);

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = extCount;
    deviceInfo.ppEnabledExtensionNames = extensions;

    VkResult result = vkCreateDevice(physicalDevice_, &deviceInfo, nullptr, &device_);
    if (result != VK_SUCCESS) {
        emit logMessage(QString("[Display] Failed to create logical device: %1").arg(result));
        return false;
    }

    vkGetDeviceQueue(device_, graphicsQueueFamily_, 0, &graphicsQueue_);
    emit logMessage("[Display] Logical device created");
    return true;
}

const char** DisplayDriver::getDeviceExtensions(uint32_t& count) {
    static const char* extensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };
    count = 1;
    return extensions;
}

bool DisplayDriver::createSwapchain() {
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &capabilities);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr);

    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, formats.data());

    VkSurfaceFormatKHR surfaceFormat = formats[0];
    for (const auto& fmt : formats) {
        if (fmt.format == VK_FORMAT_B8G8R8A8_SRGB &&
            fmt.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surfaceFormat = fmt;
            break;
        }
    }

    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &presentModeCount, presentModes.data());

    for (const auto& mode : presentModes) {
        if (mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR) {
            presentMode = mode;
            break;
        }
    }

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (imageCount > capabilities.maxImageCount && capabilities.maxImageCount > 0) {
        imageCount = capabilities.maxImageCount;
    }

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = width_;
        extent.height = height_;
    }

    VkSwapchainCreateInfoKHR swapchainInfo{};
    swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainInfo.surface = surface_;
    swapchainInfo.minImageCount = imageCount;
    swapchainInfo.imageFormat = surfaceFormat.format;
    swapchainInfo.imageColorSpace = surfaceFormat.colorSpace;
    swapchainInfo.imageExtent = extent;
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainInfo.preTransform = capabilities.currentTransform;
    swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainInfo.presentMode = presentMode;
    swapchainInfo.clipped = VK_TRUE;
    swapchainInfo.oldSwapchain = VK_NULL_HANDLE;

    VkResult result = vkCreateSwapchainKHR(device_, &swapchainInfo, nullptr, &swapchain_);
    if (result != VK_SUCCESS) {
        emit logMessage(QString("[Display] Failed to create swapchain: %1").arg(result));
        return false;
    }

    uint32_t swapchainImageCount = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &swapchainImageCount, nullptr);
    swapchainImages_.resize(swapchainImageCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &swapchainImageCount, swapchainImages_.data());

    swapchainImageFormat_ = surfaceFormat.format;
    swapchainExtent_ = extent;

    emit logMessage(QString("[Display] Swapchain created: %1 images").arg(imageCount));
    return true;
}

bool DisplayDriver::createReadbackResources() {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = width_ * height_ * sizeof(uint32_t);
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateBuffer(device_, &bufferInfo, nullptr, &stagingBuffer_);
    if (result != VK_SUCCESS) {
        emit logMessage(QString("[Display] Failed to create staging buffer: %1").arg(result));
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device_, stagingBuffer_, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReqs.memoryTypeBits & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            allocInfo.memoryTypeIndex = i;
            break;
        }
    }

    result = vkAllocateMemory(device_, &allocInfo, nullptr, &stagingMemory_);
    if (result != VK_SUCCESS) {
        emit logMessage(QString("[Display] Failed to allocate staging memory: %1").arg(result));
        return false;
    }

    vkBindBufferMemory(device_, stagingBuffer_, stagingMemory_, 0);
    emit logMessage("[Display] Readback resources created");
    return true;
}

void DisplayDriver::renderLoop() {
    emit logMessage("[Display] Render thread started");

    uint32_t frameCount = 0;
    auto fpsStart = std::chrono::steady_clock::now();
    uint32_t fpsFrames = 0;

    while (running_.load()) {
        auto frameStart = std::chrono::steady_clock::now();

        processX11Events();
        if (!running_.load()) break;

        uint32_t imageIndex;
        VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                                 VK_NULL_HANDLE, VK_NULL_HANDLE, &imageIndex);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            emit logMessage(QString("[Display] Failed to acquire swapchain image: %1").arg(result));
            break;
        }

        renderAndReadback(imageIndex, frameCount);

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain_;
        presentInfo.pImageIndices = &imageIndex;

        result = vkQueuePresentKHR(graphicsQueue_, &presentInfo);
        if (result != VK_SUCCESS) {
            emit logMessage(QString("[Display] Failed to present: %1").arg(result));
            break;
        }

        frameCount++;
        fpsFrames++;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - fpsStart).count();
        if (elapsed >= 1) {
            double actualFps = static_cast<double>(fpsFrames) / elapsed;
            int clients = server_ ? server_->getClientCount() : 0;
            emit fpsUpdate(actualFps, frameCount);
            fpsStart = now;
            fpsFrames = 0;
        }

        auto frameEnd = std::chrono::steady_clock::now();
        auto frameDuration = std::chrono::duration_cast<std::chrono::microseconds>(frameEnd - frameStart);
        auto targetDuration = std::chrono::microseconds(1000000 / targetFps_);

        if (frameDuration < targetDuration) {
            QThread::usleep((targetDuration - frameDuration).count());
        }

        QCoreApplication::processEvents();
    }

    emit logMessage(QString("[Display] Render thread stopped. Total frames: %1").arg(frameCount));
}

void DisplayDriver::renderAndReadback(uint32_t imageIndex, uint32_t frameCount) {
    // Generate colorful background
    float t = static_cast<float>(frameCount) / 100.0f;
    float r = (sin(t) * 0.5f + 0.5f);
    float g = (sin(t + 2.094f) * 0.5f + 0.5f);
    float b = (sin(t + 4.189f) * 0.5f + 0.5f);

    VkCommandPool commandPool;
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = graphicsQueueFamily_;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

    vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool);

    VkCommandBuffer commandBuffer;
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    vkAllocateCommandBuffers(device_, &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = swapchainImages_[imageIndex];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    vkCmdPipelineBarrier(commandBuffer,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkClearColorValue clearColor;
    clearColor.float32[0] = r;
    clearColor.float32[1] = g;
    clearColor.float32[2] = b;
    clearColor.float32[3] = 1.0f;

    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;

    vkCmdClearColorImage(commandBuffer,
                        swapchainImages_[imageIndex],
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        &clearColor,
                        1, &range);

    VkBufferImageCopy copyRegion{};
    copyRegion.bufferOffset = 0;
    copyRegion.bufferRowLength = width_;
    copyRegion.bufferImageHeight = height_;
    copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.imageSubresource.mipLevel = 0;
    copyRegion.imageSubresource.baseArrayLayer = 0;
    copyRegion.imageSubresource.layerCount = 1;
    copyRegion.imageOffset = {0, 0, 0};
    copyRegion.imageExtent = {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_), 1};

    vkCmdCopyImageToBuffer(commandBuffer,
                          swapchainImages_[imageIndex],
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          stagingBuffer_,
                          1, &copyRegion);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

    vkCmdPipelineBarrier(commandBuffer,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(graphicsQueue_, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue_);

    // Map and encode
    if (server_ && server_->isRunning() && encoder_.isInitialized()) {
        uint32_t* pixels = nullptr;
        vkMapMemory(device_, stagingMemory_, 0, width_ * height_ * sizeof(uint32_t), 0,
                   reinterpret_cast<void**>(&pixels));

        bool isKeyframe = false;
        auto encoded = encoder_.encode(pixels, width_, height_, &isKeyframe);

        if (!encoded.empty()) {
            // Send encoded frame
            // Note: For simplicity, we send header + data together
            // In production, you'd want proper fragmentation for large frames
            server_->sendVideoFrame(encoded.data(), encoded.size());
        }

        vkUnmapMemory(device_, stagingMemory_);
    }

    vkFreeCommandBuffers(device_, commandPool, 1, &commandBuffer);
    vkDestroyCommandPool(device_, commandPool, nullptr);
}

void DisplayDriver::processX11Events() {
    XEvent event;
    while (XPending(display_)) {
        XNextEvent(display_, &event);
        if (event.type == 2) { // KeyPress
            running_.store(false);
        } else if (event.type == 17) { // DestroyNotify
            running_.store(false);
        }
    }
}
