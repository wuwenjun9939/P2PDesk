/**
 * Server: GPU 1080p 160Hz Display Driver + Virtual Sound Card + Network Streaming
 * 
 * Features:
 * 1. Uses Vulkan API to drive display at 1080p 160Hz
 * 2. Registers a virtual ALSA sound card as "ears"
 * 3. Streams display frames and audio to clients over network
 * 
 * Compile:
 *   gcc -std=c++17 -Wall -Wextra -O2 server.cpp -o server \
 *       -lvulkan -lasound -lpthread -lX11 -lXrandr -lstdc++ -lm
 */

#define VK_USE_PLATFORM_XLIB_KHR
#include "common/H265Codec.h"
#include <vulkan/vulkan.h>
#include <alsa/asoundlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>

#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <thread>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <functional>
#include <algorithm>

// Network headers
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

// ============================================================
// Network Protocol
// ============================================================

// Dual-channel architecture:
//   UDP (port 9876)  - Video frames + Audio stream (unreliable, low latency)
//   TCP (port 9877)  - Control messages (reliable, ordered)

#pragma pack(push, 1)

// --- UDP: Video/Audio Stream ---
struct FrameHeader {
    uint32_t magic;         // 0x44535059 = "DSPY"
    uint32_t frame_number;
    uint16_t width;
    uint16_t height;
    uint8_t  codec;         // 0=raw, 1=RLE, 2=delta
    uint8_t  fragment_id;   // 0 = single, 1+ = multi-fragment
    uint16_t fragment_total;
    uint32_t fragment_size; // bytes of pixel data in this fragment
    uint32_t pixel_offset;  // starting pixel index for this fragment
};

struct AudioHeader {
    uint32_t magic;         // 0x41554449 = "AUDI"
    uint32_t timestamp_us;  // microseconds
    uint16_t sample_rate;
    uint8_t  channels;
    uint8_t  sample_count;  // max 255 samples per packet
    // Followed by sample_count * channels * sizeof(int16_t)
};

// --- TCP: Control Messages ---
enum ControlType : uint32_t {
    PING = 1,
    PONG = 2,
    STATUS = 3,
    SHUTDOWN = 4,
    MOUSE_MOVE = 10,
    MOUSE_CLICK = 11,
    MOUSE_SCROLL = 12,
    KEY_PRESS = 13,
    KEY_RELEASE = 14,
    CONTROL_ENABLE = 15,
    CONTROL_DISABLE = 16,
};

struct ControlMessage {
    uint32_t type;
    union {
        uint32_t payload;
        struct { int32_t x; int32_t y; } mouse_pos;
        struct { uint8_t button; int32_t x; int32_t y; } mouse_click;
        struct { int32_t delta; int32_t x; int32_t y; } mouse_scroll;
        struct { uint32_t keycode; uint32_t state; } key;
    };
};
#pragma pack(pop)

constexpr uint32_t FRAME_MAGIC = 0x44535059;
constexpr uint32_t AUDIO_MAGIC = 0x41554449;

// UDP MTU-safe fragment size (1400 bytes payload leaves room for IP+UDP headers)
constexpr size_t MAX_UDP_PAYLOAD = 1400;

// ============================================================
// Video Codec: H.265
// ============================================================

class VideoCodec {
public:
    VideoCodec() {}
    
    bool init(int width, int height, int fps) {
        return encoder_.init(width, height, fps, false);
    }

    std::vector<uint8_t> encode(const uint32_t* pixels, uint32_t width, uint32_t height) {
        return encoder_.encode(pixels, width, height, nullptr);
    }

private:
    H265Encoder encoder_;
};

// ============================================================
// Network Server (UDP Video/Audio + TCP Control)
// ============================================================

class NetworkServer {
public:
    NetworkServer(uint16_t udp_port = 9876, uint16_t tcp_port = 9877) 
        : udp_port_(udp_port), tcp_port_(tcp_port), 
          udp_fd_(-1), tcp_fd_(-1), running_(false) {}

    ~NetworkServer() {
        stop();
    }

    bool start() {
        std::cout << "[Network] Starting dual-channel server..." << std::endl;
        std::cout << "[Network]   UDP (video/audio): port " << udp_port_ << std::endl;
        std::cout << "[Network]   TCP (control):     port " << tcp_port_ << std::endl;

        // Create UDP socket
        udp_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_fd_ < 0) {
            std::cout << "[Network] Failed to create UDP socket: " << strerror(errno) << std::endl;
            return false;
        }

        struct sockaddr_in udp_addr{};
        udp_addr.sin_family = AF_INET;
        udp_addr.sin_addr.s_addr = INADDR_ANY;
        udp_addr.sin_port = htons(udp_port_);

        if (bind(udp_fd_, (struct sockaddr*)&udp_addr, sizeof(udp_addr)) < 0) {
            std::cout << "[Network] Failed to bind UDP: " << strerror(errno) << std::endl;
            close(udp_fd_);
            return false;
        }

        // Create TCP socket for control
        tcp_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (tcp_fd_ < 0) {
            std::cout << "[Network] Failed to create TCP socket: " << strerror(errno) << std::endl;
            close(udp_fd_);
            return false;
        }

        int opt = 1;
        setsockopt(tcp_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in tcp_addr{};
        tcp_addr.sin_family = AF_INET;
        tcp_addr.sin_addr.s_addr = INADDR_ANY;
        tcp_addr.sin_port = htons(tcp_port_);

        if (bind(tcp_fd_, (struct sockaddr*)&tcp_addr, sizeof(tcp_addr)) < 0) {
            std::cout << "[Network] Failed to bind TCP: " << strerror(errno) << std::endl;
            close(udp_fd_);
            close(tcp_fd_);
            return false;
        }

        if (listen(tcp_fd_, 5) < 0) {
            std::cout << "[Network] Failed to listen TCP: " << strerror(errno) << std::endl;
            close(udp_fd_);
            close(tcp_fd_);
            return false;
        }

        running_.store(true);
        tcp_thread_ = std::thread(&NetworkServer::tcpAcceptLoop, this);

        std::cout << "[Network] Server ready" << std::endl;
        return true;
    }

    void stop() {
        if (!running_.load()) return;
        running_.store(false);

        if (udp_fd_ >= 0) {
            close(udp_fd_);
            udp_fd_ = -1;
        }

        if (tcp_fd_ >= 0) {
            close(tcp_fd_);
            tcp_fd_ = -1;
        }

        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (int fd : tcp_clients_) {
            close(fd);
        }
        tcp_clients_.clear();
        client_addrs_.clear();

        if (tcp_thread_.joinable()) {
            tcp_thread_.join();
        }
    }

    void setControlCallback(std::function<void(const ControlMessage&)> cb) {
        control_callback_ = cb;
    }

    // UDP: Send video fragment to client
    void sendVideo(const uint8_t* data, size_t size, struct sockaddr_in& client_addr) {
        if (udp_fd_ < 0) return;

        // Fragment into MTU-safe chunks
        size_t offset = 0;
        while (offset < size) {
            size_t chunk = std::min(size - offset, MAX_UDP_PAYLOAD);
            sendto(udp_fd_, data + offset, chunk, 0, 
                   reinterpret_cast<struct sockaddr*>(&client_addr), sizeof(client_addr));
            offset += chunk;
        }
    }

    // UDP: Send audio to client
    void sendAudio(const uint8_t* data, size_t size, struct sockaddr_in& client_addr) {
        if (udp_fd_ < 0) return;
        sendto(udp_fd_, data, size, 0,
               reinterpret_cast<struct sockaddr*>(&client_addr), sizeof(client_addr));
    }

    int getClientCount() const {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        return tcp_clients_.size();
    }

    struct sockaddr_in getClientAddr(int index) const {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        if (index < static_cast<int>(client_addrs_.size())) {
            return client_addrs_[index];
        }
        return {};
    }

private:
    void tcpAcceptLoop() {
        while (running_.load()) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(tcp_fd_, &fds);

            struct timeval tv{1, 0};
            int ret = select(tcp_fd_ + 1, &fds, nullptr, nullptr, &tv);
            if (ret <= 0) continue;

            struct sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(tcp_fd_, (struct sockaddr*)&client_addr, &client_len);
            if (client_fd < 0) continue;

            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
            std::cout << "[Network] Control client connected: " << ip << std::endl;

            // Register UDP address for this client
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                tcp_clients_.push_back(client_fd);
                client_addrs_.push_back(client_addr);
            }

            std::thread(&NetworkServer::tcpClientHandler, this, client_fd).detach();
        }
    }

    void tcpClientHandler(int client_fd) {
        char buffer[1024];
        while (running_.load()) {
            int bytes = recv(client_fd, buffer, sizeof(buffer), 0);
            if (bytes <= 0) {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                for (size_t i = 0; i < tcp_clients_.size(); i++) {
                    if (tcp_clients_[i] == client_fd) {
                        tcp_clients_.erase(tcp_clients_.begin() + i);
                        client_addrs_.erase(client_addrs_.begin() + i);
                        break;
                    }
                }
                close(client_fd);
                std::cout << "[Network] Control client disconnected" << std::endl;
                return;
            }

            size_t offset = 0;
            while (offset + sizeof(ControlMessage) <= static_cast<size_t>(bytes)) {
                ControlMessage* msg = reinterpret_cast<ControlMessage*>(buffer + offset);
                offset += sizeof(ControlMessage);

                switch (msg->type) {
                    case PING: {
                        ControlMessage pong{PONG, 0};
                        send(client_fd, &pong, sizeof(pong), MSG_NOSIGNAL);
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
                        if (control_callback_) {
                            control_callback_(*msg);
                        }
                        break;
                    default:
                        break;
                }
            }
        }
    }

    uint16_t udp_port_;
    uint16_t tcp_port_;
    int udp_fd_;
    int tcp_fd_;
    std::atomic<bool> running_;
    std::thread tcp_thread_;
    std::vector<int> tcp_clients_;
    std::vector<struct sockaddr_in> client_addrs_;
    mutable std::mutex clients_mutex_;
    std::function<void(const ControlMessage&)> control_callback_;
};

// ============================================================
// Virtual Sound Card (ALSA Loopback)
// ============================================================

class VirtualSoundCard {
public:
    VirtualSoundCard() : running_(false), pcm_handle_(nullptr) {}

    ~VirtualSoundCard() {
        stop();
    }

    bool initialize() {
        std::cout << "[Audio] Initializing virtual sound card..." << std::endl;

        int ret = system("modprobe snd-aloop 2>/dev/null");
        if (ret != 0) {
            std::cout << "[Audio] Note: snd-aloop module may need sudo to load" << std::endl;
        }

        sample_rate_ = 48000;
        channels_ = 2;
        buffer_size_ = 1024;
        period_size_ = 256;

        std::cout << "[Audio] Configuration:" << std::endl;
        std::cout << "  - Sample Rate: " << sample_rate_ << " Hz" << std::endl;
        std::cout << "  - Channels: " << channels_ << " (Stereo)" << std::endl;
        std::cout << "  - Buffer Size: " << buffer_size_ << " frames" << std::endl;
        std::cout << "  - Period Size: " << period_size_ << " frames" << std::endl;

        ret = snd_pcm_open(&pcm_handle_, "default", SND_PCM_STREAM_PLAYBACK, 0);
        if (ret < 0) {
            std::cout << "[Audio] Warning: Could not open default PCM device: " 
                      << snd_strerror(ret) << std::endl;
            std::cout << "[Audio] Will use virtual buffer mode" << std::endl;
            pcm_handle_ = nullptr;
        } else {
            snd_pcm_hw_params_t *hw_params;
            snd_pcm_hw_params_alloca(&hw_params);

            ret = snd_pcm_hw_params_any(pcm_handle_, hw_params);
            if (ret < 0) {
                std::cout << "[Audio] Failed to get hardware parameters" << std::endl;
                snd_pcm_close(pcm_handle_);
                pcm_handle_ = nullptr;
            } else {
                ret = snd_pcm_hw_params_set_access(pcm_handle_, hw_params, 
                                                   SND_PCM_ACCESS_RW_INTERLEAVED);
                ret = snd_pcm_hw_params_set_format(pcm_handle_, hw_params, 
                                                   SND_PCM_FORMAT_S16_LE);
                unsigned int rate = sample_rate_;
                ret = snd_pcm_hw_params_set_rate_near(pcm_handle_, hw_params, &rate, nullptr);
                ret = snd_pcm_hw_params_set_channels(pcm_handle_, hw_params, channels_);
                ret = snd_pcm_hw_params(pcm_handle_, hw_params);
                if (ret < 0) {
                    std::cout << "[Audio] Failed to apply hardware parameters: " 
                              << snd_strerror(ret) << std::endl;
                    snd_pcm_close(pcm_handle_);
                    pcm_handle_ = nullptr;
                } else {
                    std::cout << "[Audio] ALSA device configured successfully" << std::endl;
                }
            }
        }

        registerVirtualDevice();
        return true;
    }

    void start(NetworkServer* server) {
        if (running_.load()) return;
        server_ = server;

        std::cout << "[Audio] Starting virtual sound card audio generation..." << std::endl;
        running_.store(true);
        audio_thread_ = std::thread(&VirtualSoundCard::audioLoop, this);
    }

    void stop() {
        if (!running_.load()) return;

        std::cout << "[Audio] Stopping virtual sound card..." << std::endl;
        running_.store(false);
        if (audio_thread_.joinable()) {
            audio_thread_.join();
        }

        if (pcm_handle_) {
            snd_pcm_drain(pcm_handle_);
            snd_pcm_close(pcm_handle_);
            pcm_handle_ = nullptr;
        }
    }

    bool isRunning() const { return running_.load(); }

private:
    void registerVirtualDevice() {
        std::cout << "[Audio] Registering virtual sound card device..." << std::endl;

        const char* asound_conf_path = "/tmp/virtual_soundcard.conf";
        FILE* f = fopen(asound_conf_path, "w");
        if (f) {
            fprintf(f, "# Virtual Sound Card Configuration\n");
            fprintf(f, "pcm.virtual_ears {\n");
            fprintf(f, "    type plug\n");
            fprintf(f, "    slave {\n");
            fprintf(f, "        pcm {\n");
            fprintf(f, "            type dmix\n");
            fprintf(f, "            slave.pcm \"default\"\n");
            fprintf(f, "        }\n");
            fprintf(f, "    }\n");
            fprintf(f, "}\n");
            fprintf(f, "\n");
            fprintf(f, "ctl.virtual_ears {\n");
            fprintf(f, "    type hw\n");
            fprintf(f, "    card 0\n");
            fprintf(f, "}\n");
            fclose(f);
            std::cout << "[Audio] Virtual device config written to: " << asound_conf_path << std::endl;
        }
    }

    void audioLoop() {
        double phase = 0.0;
        const double phase_increment = 2.0 * M_PI * 440.0 / sample_rate_;

        std::vector<int16_t> buffer(buffer_size_ * channels_);

        std::cout << "[Audio] Generating 440Hz test tone (A4)..." << std::endl;

        while (running_.load()) {
            auto start_time = std::chrono::steady_clock::now();

            for (size_t i = 0; i < buffer_size_; ++i) {
                int16_t sample = static_cast<int16_t>(32767.0 * sin(phase));
                phase += phase_increment;
                if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;

                buffer[i * channels_] = sample;
                buffer[i * channels_ + 1] = sample;
            }

            if (pcm_handle_) {
                int frames = snd_pcm_writei(pcm_handle_, buffer.data(), buffer_size_);
                if (frames < 0) {
                    frames = snd_pcm_recover(pcm_handle_, frames, 0);
                }
            }

            // Stream audio to clients via UDP
            if (server_) {
                AudioHeader audio_hdr;
                audio_hdr.magic = AUDIO_MAGIC;
                audio_hdr.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                audio_hdr.sample_rate = static_cast<uint16_t>(sample_rate_);
                audio_hdr.channels = static_cast<uint8_t>(channels_);
                audio_hdr.sample_count = static_cast<uint8_t>(buffer_size_);

                int client_count = server_->getClientCount();
                for (int i = 0; i < client_count; i++) {
                    auto client_addr = server_->getClientAddr(i);
                    if (client_addr.sin_family == AF_INET) {
                        server_->sendVideo(reinterpret_cast<uint8_t*>(&audio_hdr), sizeof(audio_hdr), client_addr);
                        server_->sendVideo(reinterpret_cast<uint8_t*>(buffer.data()), 
                                          buffer_size_ * channels_ * sizeof(int16_t), client_addr);
                    }
                }
            }

            auto end_time = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
            auto expected_duration = std::chrono::microseconds(
                static_cast<int64_t>(buffer_size_ * 1000000.0 / sample_rate_));
            
            if (duration < expected_duration) {
                std::this_thread::sleep_for(expected_duration - duration);
            }
        }

        std::cout << "[Audio] Audio thread stopped" << std::endl;
    }

    NetworkServer* server_;
    std::atomic<bool> running_;
    std::thread audio_thread_;
    snd_pcm_t* pcm_handle_;

    unsigned int sample_rate_;
    unsigned int channels_;
    snd_pcm_uframes_t buffer_size_;
    snd_pcm_uframes_t period_size_;
};

// ============================================================
// GPU Display Driver (Vulkan + X11)
// ============================================================

class DisplayDriver {
public:
    DisplayDriver() 
        : running_(false),
          instance_(VK_NULL_HANDLE),
          physical_device_(VK_NULL_HANDLE),
          device_(VK_NULL_HANDLE),
          surface_(VK_NULL_HANDLE),
          swapchain_(VK_NULL_HANDLE),
          display_(nullptr),
          window_(0) {
    }

    ~DisplayDriver() {
        cleanup();
    }

    bool initialize(int width = 1920, int height = 1080, int target_fps = 160) {
        width_ = width;
        height_ = height;
        target_fps_ = target_fps;

        std::cout << "[Display] Initializing GPU display driver..." << std::endl;
        std::cout << "[Display] Target: " << width << "x" << height << " @" << target_fps << "Hz" << std::endl;

        if (!createWindow()) {
            std::cout << "[Display] Failed to create X11 window" << std::endl;
            return false;
        }

        if (!initVulkan()) {
            std::cout << "[Display] Failed to initialize Vulkan" << std::endl;
            return false;
        }

        // Create readback resources
        if (!createReadbackResources()) {
            std::cout << "[Display] Failed to create readback resources" << std::endl;
            return false;
        }

        std::cout << "[Display] GPU display driver initialized successfully" << std::endl;
        return true;
    }

    void handleControl(const ControlMessage& msg) {
        if (!display_) return;

        switch (msg.type) {
            case MOUSE_MOVE:
                XWarpPointer(display_, None, window_, 0, 0, 0, 0, 
                            msg.mouse_pos.x, msg.mouse_pos.y);
                XFlush(display_);
                break;

            case MOUSE_CLICK: {
                int x = msg.mouse_click.x;
                int y = msg.mouse_click.y;
                uint8_t button = msg.mouse_click.button;

                // Move to position first
                XWarpPointer(display_, None, window_, 0, 0, 0, 0, x, y);

                // X11 button mapping: 1=left, 2=middle, 3=right
                int x11_button = (button == 0) ? Button1 : (button == 1) ? Button3 : Button2;

                if (msg.mouse_click.button & 0x80) { // Release
                    XTestFakeButtonEvent(display_, x11_button, False, CurrentTime);
                } else { // Press
                    XTestFakeButtonEvent(display_, x11_button, True, CurrentTime);
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
                // Convert keycode to X11 KeySym
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

    void start(NetworkServer* server) {
        if (running_.load()) return;
        server_ = server;

        std::cout << "[Display] Starting render loop at " << target_fps_ << " FPS..." << std::endl;
        running_.store(true);
        render_thread_ = std::thread(&DisplayDriver::renderLoop, this);
    }

    void stop() {
        if (!running_.load()) return;

        std::cout << "[Display] Stopping render loop..." << std::endl;
        running_.store(false);
        if (render_thread_.joinable()) {
            render_thread_.join();
        }
    }

    bool isRunning() const { return running_.load(); }

    void cleanup() {
        stop();

        // Cleanup readback resources
        if (staging_buffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, staging_buffer_, nullptr);
        }
        if (staging_memory_ != VK_NULL_HANDLE) {
            vkFreeMemory(device_, staging_memory_, nullptr);
        }

        if (swapchain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }

        if (device_ != VK_NULL_HANDLE) {
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }

        if (surface_ != VK_NULL_HANDLE) {
            if (instance_ != VK_NULL_HANDLE) {
                vkDestroySurfaceKHR(instance_, surface_, nullptr);
            }
            surface_ = VK_NULL_HANDLE;
        }

        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }

        if (window_) {
            XDestroyWindow(display_, window_);
            window_ = 0;
        }

        if (display_) {
            XCloseDisplay(display_);
            display_ = nullptr;
        }
    }

private:
    bool createWindow() {
        std::cout << "[Display] Creating X11 window..." << std::endl;

        display_ = XOpenDisplay(nullptr);
        if (!display_) {
            std::cout << "[Display] Failed to open X display" << std::endl;
            return false;
        }

        int screen = DefaultScreen(display_);
        Window root = RootWindow(display_, screen);

        window_ = XCreateSimpleWindow(display_, root, 0, 0, width_, height_, 2,
                                      WhitePixel(display_, screen),
                                      BlackPixel(display_, screen));

        XSelectInput(display_, window_, ExposureMask | KeyPressMask | StructureNotifyMask);
        XStoreName(display_, window_, "GPU Display Server - 1080p 160Hz");

        XSizeHints* size_hints = XAllocSizeHints();
        size_hints->flags = PMinSize | PMaxSize;
        size_hints->min_width = width_;
        size_hints->max_width = width_;
        size_hints->min_height = height_;
        size_hints->max_height = height_;
        XSetWMNormalHints(display_, window_, size_hints);
        XFree(size_hints);

        XMapWindow(display_, window_);
        XFlush(display_);

        std::cout << "[Display] X11 window created: " << width_ << "x" << height_ << std::endl;
        return true;
    }

    bool initVulkan() {
        std::cout << "[Display] Initializing Vulkan..." << std::endl;

        if (!createInstance()) return false;
        if (!createSurface()) return false;
        if (!selectPhysicalDevice()) return false;
        if (!createLogicalDevice()) return false;
        if (!createSwapchain()) return false;

        std::cout << "[Display] Vulkan initialized successfully" << std::endl;
        return true;
    }

    bool createInstance() {
        VkApplicationInfo app_info{};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = "GPU Display Server";
        app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        app_info.pEngineName = "Custom Engine";
        app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        app_info.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &app_info;

        uint32_t ext_count = 0;
        const char** extensions = getRequiredExtensions(ext_count);
        create_info.enabledExtensionCount = ext_count;
        create_info.ppEnabledExtensionNames = extensions;

        VkResult result = vkCreateInstance(&create_info, nullptr, &instance_);
        if (result != VK_SUCCESS) {
            std::cout << "[Display] Failed to create Vulkan instance: " << result << std::endl;
            return false;
        }

        std::cout << "[Display] Vulkan instance created" << std::endl;
        return true;
    }

    const char** getRequiredExtensions(uint32_t& count) {
        static const char* extensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_XLIB_SURFACE_EXTENSION_NAME
        };
        count = 2;
        return extensions;
    }

    bool createSurface() {
        VkXlibSurfaceCreateInfoKHR surface_info{};
        surface_info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
        surface_info.dpy = display_;
        surface_info.window = window_;

        VkResult result = vkCreateXlibSurfaceKHR(instance_, &surface_info, nullptr, &surface_);
        if (result != VK_SUCCESS) {
            std::cout << "[Display] Failed to create Vulkan surface: " << result << std::endl;
            return false;
        }

        std::cout << "[Display] Vulkan surface created (X11)" << std::endl;
        return true;
    }

    bool selectPhysicalDevice() {
        uint32_t device_count = 0;
        vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);

        if (device_count == 0) {
            std::cout << "[Display] No Vulkan-capable GPUs found" << std::endl;
            return false;
        }

        std::vector<VkPhysicalDevice> devices(device_count);
        vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());

        for (const auto& dev : devices) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(dev, &props);

            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                physical_device_ = dev;
                std::cout << "[Display] Selected GPU: " << props.deviceName << std::endl;
                return true;
            }
        }

        physical_device_ = devices[0];
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical_device_, &props);
        std::cout << "[Display] Selected GPU (fallback): " << props.deviceName << std::endl;
        return true;
    }

    bool createLogicalDevice() {
        uint32_t queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_family_count, nullptr);

        std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_family_count, 
                                                  queue_families.data());

        graphics_queue_family_ = UINT32_MAX;
        for (uint32_t i = 0; i < queue_family_count; ++i) {
            if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                graphics_queue_family_ = i;
                break;
            }
        }

        if (graphics_queue_family_ == UINT32_MAX) {
            std::cout << "[Display] No graphics queue family found" << std::endl;
            return false;
        }

        float queue_priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = graphics_queue_family_;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &queue_priority;

        uint32_t ext_count;
        const char** extensions = getDeviceExtensions(ext_count);

        VkDeviceCreateInfo device_info{};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = ext_count;
        device_info.ppEnabledExtensionNames = extensions;

        VkResult result = vkCreateDevice(physical_device_, &device_info, nullptr, &device_);
        if (result != VK_SUCCESS) {
            std::cout << "[Display] Failed to create logical device: " << result << std::endl;
            return false;
        }

        vkGetDeviceQueue(device_, graphics_queue_family_, 0, &graphics_queue_);

        std::cout << "[Display] Logical device created (Graphics Queue Family: " 
                  << graphics_queue_family_ << ")" << std::endl;
        return true;
    }

    const char** getDeviceExtensions(uint32_t& count) {
        static const char* extensions[] = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME
        };
        count = 1;
        return extensions;
    }

    bool createSwapchain() {
        VkSurfaceCapabilitiesKHR capabilities;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &capabilities);

        uint32_t format_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, nullptr);

        std::vector<VkSurfaceFormatKHR> formats(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, 
                                              formats.data());

        VkSurfaceFormatKHR surface_format = formats[0];
        for (const auto& fmt : formats) {
            if (fmt.format == VK_FORMAT_B8G8R8A8_SRGB && 
                fmt.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                surface_format = fmt;
                break;
            }
        }

        VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
        
        uint32_t present_mode_count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, 
                                                   &present_mode_count, nullptr);
        std::vector<VkPresentModeKHR> present_modes(present_mode_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, 
                                                   &present_mode_count, present_modes.data());

        for (const auto& mode : present_modes) {
            if (mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR) {
                present_mode = mode;
                break;
            }
        }

        uint32_t image_count = capabilities.minImageCount + 1;
        if (image_count > capabilities.maxImageCount && capabilities.maxImageCount > 0) {
            image_count = capabilities.maxImageCount;
        }

        VkExtent2D extent = capabilities.currentExtent;
        if (extent.width == UINT32_MAX) {
            extent.width = width_;
            extent.height = height_;
        }

        VkSwapchainCreateInfoKHR swapchain_info{};
        swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        swapchain_info.surface = surface_;
        swapchain_info.minImageCount = image_count;
        swapchain_info.imageFormat = surface_format.format;
        swapchain_info.imageColorSpace = surface_format.colorSpace;
        swapchain_info.imageExtent = extent;
        swapchain_info.imageArrayLayers = 1;
        swapchain_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchain_info.preTransform = capabilities.currentTransform;
        swapchain_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        swapchain_info.presentMode = present_mode;
        swapchain_info.clipped = VK_TRUE;
        swapchain_info.oldSwapchain = VK_NULL_HANDLE;

        VkResult result = vkCreateSwapchainKHR(device_, &swapchain_info, nullptr, &swapchain_);
        if (result != VK_SUCCESS) {
            std::cout << "[Display] Failed to create swapchain: " << result << std::endl;
            return false;
        }

        uint32_t swapchain_image_count = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &swapchain_image_count, nullptr);
        swapchain_images_.resize(swapchain_image_count);
        vkGetSwapchainImagesKHR(device_, swapchain_, &swapchain_image_count, 
                                swapchain_images_.data());

        swapchain_image_format_ = surface_format.format;
        swapchain_extent_ = extent;

        std::cout << "[Display] Swapchain created: " << image_count << " images, "
                  << extent.width << "x" << extent.height << std::endl;
        std::cout << "[Display] Present mode: " << (present_mode == VK_PRESENT_MODE_FIFO_KHR ? 
                                       "FIFO (VSync)" : "FIFO_RELAXED") << std::endl;

        return true;
    }

    bool createReadbackResources() {
        // Create staging buffer for readback (host visible)
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = width_ * height_ * sizeof(uint32_t);
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkResult result = vkCreateBuffer(device_, &buffer_info, nullptr, &staging_buffer_);
        if (result != VK_SUCCESS) {
            std::cout << "[Display] Failed to create staging buffer: " << result << std::endl;
            return false;
        }

        // Allocate host-visible memory
        VkMemoryRequirements mem_reqs;
        vkGetBufferMemoryRequirements(device_, staging_buffer_, &mem_reqs);

        VkMemoryAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = mem_reqs.size;

        // Find host-visible memory type
        VkPhysicalDeviceMemoryProperties mem_props;
        vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_props);

        for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
            if ((mem_reqs.memoryTypeBits & (1 << i)) &&
                (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                alloc_info.memoryTypeIndex = i;
                break;
            }
        }

        result = vkAllocateMemory(device_, &alloc_info, nullptr, &staging_memory_);
        if (result != VK_SUCCESS) {
            std::cout << "[Display] Failed to allocate staging memory: " << result << std::endl;
            return false;
        }

        vkBindBufferMemory(device_, staging_buffer_, staging_memory_, 0);

        std::cout << "[Display] Readback resources created" << std::endl;
        return true;
    }

    void renderLoop() {
        std::cout << "[Display] Render thread started" << std::endl;

        uint32_t frame_count = 0;
        auto fps_start = std::chrono::steady_clock::now();
        uint32_t fps_frames = 0;

        while (running_.load()) {
            auto frame_start = std::chrono::steady_clock::now();

            processX11Events();

            if (!running_.load()) break;

            uint32_t image_index;
            VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                                     VK_NULL_HANDLE, VK_NULL_HANDLE, &image_index);
            if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
                std::cout << "[Display] Failed to acquire swapchain image: " << result << std::endl;
                break;
            }

            // Render and read back pixels
            renderAndReadback(image_index, frame_count);

            VkPresentInfoKHR present_info{};
            present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present_info.swapchainCount = 1;
            present_info.pSwapchains = &swapchain_;
            present_info.pImageIndices = &image_index;

            result = vkQueuePresentKHR(graphics_queue_, &present_info);
            if (result != VK_SUCCESS) {
                std::cout << "[Display] Failed to present image: " << result << std::endl;
                break;
            }

            frame_count++;
            fps_frames++;

            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - fps_start).count();
            if (elapsed >= 1) {
                double actual_fps = static_cast<double>(fps_frames) / elapsed;
                int clients = server_ ? server_->getClientCount() : 0;
                std::cout << "[Display] FPS: " << actual_fps 
                          << " | Clients: " << clients
                          << " | Frame: " << frame_count << std::endl;
                fps_start = now;
                fps_frames = 0;
            }

            auto frame_end = std::chrono::steady_clock::now();
            auto frame_duration = std::chrono::duration_cast<std::chrono::microseconds>(
                frame_end - frame_start);
            auto target_duration = std::chrono::microseconds(1000000 / target_fps_);

            if (frame_duration < target_duration) {
                std::this_thread::sleep_for(target_duration - frame_duration);
            }
        }

        std::cout << "[Display] Render thread stopped. Total frames: " << frame_count << std::endl;
    }

    void renderAndReadback(uint32_t image_index, uint32_t frame_count) {
        float t = static_cast<float>(frame_count) / 100.0f;
        float r = (sin(t) * 0.5f + 0.5f);
        float g = (sin(t + 2.094f) * 0.5f + 0.5f);
        float b = (sin(t + 4.189f) * 0.5f + 0.5f);

        VkCommandPool command_pool;
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = graphics_queue_family_;
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

        vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool);

        VkCommandBuffer command_buffer;
        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = command_pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;

        vkAllocateCommandBuffers(device_, &alloc_info, &command_buffer);

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

        vkBeginCommandBuffer(command_buffer, &begin_info);

        // Transition to transfer source
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = swapchain_images_[image_index];
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        vkCmdPipelineBarrier(command_buffer,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Clear with color
        VkClearColorValue clear_color;
        clear_color.float32[0] = r;
        clear_color.float32[1] = g;
        clear_color.float32[2] = b;
        clear_color.float32[3] = 1.0f;

        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = 0;
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = 1;

        vkCmdClearColorImage(command_buffer,
                            swapchain_images_[image_index],
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            &clear_color,
                            1, &range);

        // Copy to staging buffer for readback
        VkBufferImageCopy copy_region{};
        copy_region.bufferOffset = 0;
        copy_region.bufferRowLength = width_;
        copy_region.bufferImageHeight = height_;
        copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy_region.imageSubresource.mipLevel = 0;
        copy_region.imageSubresource.baseArrayLayer = 0;
        copy_region.imageSubresource.layerCount = 1;
        copy_region.imageOffset = {0, 0, 0};
        copy_region.imageExtent = {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_), 1};

        vkCmdCopyImageToBuffer(command_buffer,
                              swapchain_images_[image_index],
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              staging_buffer_,
                              1, &copy_region);

        // Transition back to present
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

        vkCmdPipelineBarrier(command_buffer,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &barrier);

        vkEndCommandBuffer(command_buffer);

        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer;

        vkQueueSubmit(graphics_queue_, 1, &submit_info, VK_NULL_HANDLE);
        vkQueueWaitIdle(graphics_queue_);

        // Map staging buffer, encode, and stream to clients
        if (server_) {
            uint32_t* pixels = nullptr;
            vkMapMemory(device_, staging_memory_, 0, width_ * height_ * sizeof(uint32_t), 0, 
                       reinterpret_cast<void**>(&pixels));
            
            // Encode with codec
            auto encoded = codec_.encode(pixels, width_, height_, frame_count);
            
            // Send header + encoded data via UDP
            FrameHeader header;
            header.magic = FRAME_MAGIC;
            header.frame_number = frame_count;
            header.width = static_cast<uint16_t>(width_);
            header.height = static_cast<uint16_t>(height_);
            header.codec = (frame_count % 30 == 0) ? 1 : 2; // keyframe=RLE, else=delta
            header.fragment_id = 0;
            header.fragment_total = 1;
            header.fragment_size = encoded.size();
            header.pixel_offset = 0;
            
            // Send to all connected clients
            int client_count = server_->getClientCount();
            for (int i = 0; i < client_count; i++) {
                auto client_addr = server_->getClientAddr(i);
                if (client_addr.sin_family == AF_INET) {
                    // Send header
                    server_->sendVideo(reinterpret_cast<uint8_t*>(&header), sizeof(header), client_addr);
                    // Send encoded data
                    server_->sendVideo(encoded.data(), encoded.size(), client_addr);
                }
            }
            
            vkUnmapMemory(device_, staging_memory_);
        }

        vkFreeCommandBuffers(device_, command_pool, 1, &command_buffer);
        vkDestroyCommandPool(device_, command_pool, nullptr);
    }

    void processX11Events() {
        XEvent event;
        while (XPending(display_)) {
            XNextEvent(display_, &event);
            if (event.type == KeyPress) {
                running_.store(false);
            } else if (event.type == DestroyNotify) {
                running_.store(false);
            }
        }
    }

    NetworkServer* server_;
    VideoCodec codec_;
    std::atomic<bool> running_;
    std::thread render_thread_;

    int width_;
    int height_;
    int target_fps_;

    VkInstance instance_;
    VkPhysicalDevice physical_device_;
    VkDevice device_;
    VkSurfaceKHR surface_;
    VkSwapchainKHR swapchain_;
    VkQueue graphics_queue_;
    uint32_t graphics_queue_family_;

    Display* display_;
    Window window_;

    std::vector<VkImage> swapchain_images_;
    VkFormat swapchain_image_format_;
    VkExtent2D swapchain_extent_;

    // Readback resources
    VkBuffer staging_buffer_;
    VkDeviceMemory staging_memory_;
};

// ============================================================
// Main Application (Server)
// ============================================================

int main() {
    std::cout << "============================================" << std::endl;
    std::cout << "  GPU Display Server + Virtual Sound Card" << std::endl;
    std::cout << "  Target: 1080p @ 160Hz" << std::endl;
    std::cout << "  Video/Audio: UDP :9876" << std::endl;
    std::cout << "  Control:     TCP :9877" << std::endl;
    std::cout << "  Codec:       RLE + Delta Frame" << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout << std::endl;

    // Start dual-channel network server
    NetworkServer network(9876, 9877);
    if (!network.start()) {
        std::cout << "[Error] Failed to start network server" << std::endl;
        return 1;
    }
    std::cout << std::endl;

    // Initialize virtual sound card
    VirtualSoundCard soundCard;
    if (!soundCard.initialize()) {
        std::cout << "[Error] Failed to initialize virtual sound card" << std::endl;
        return 1;
    }
    std::cout << std::endl;

    // Initialize display driver
    DisplayDriver display;
    if (!display.initialize(1920, 1080, 160)) {
        std::cout << "[Error] Failed to initialize display driver" << std::endl;
        return 1;
    }

    // Wire up control callback to display driver
    network.setControlCallback([&](const ControlMessage& msg) {
        display.handleControl(msg);
    });

    std::cout << std::endl;

    // Start both systems with network server reference
    std::cout << "Starting systems..." << std::endl;
    soundCard.start(&network);
    display.start(&network);

    std::cout << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout << "  Server Running" << std::endl;
    std::cout << "  Display: 1080p @ 160Hz (Vulkan)" << std::endl;
    std::cout << "  Audio: 48kHz Stereo (ALSA)" << std::endl;
    std::cout << "  Network: TCP :9876" << std::endl;
    std::cout << "  Control: Mouse/Keyboard enabled" << std::endl;
    std::cout << "  Press any key in the window or close it to exit" << std::endl;
    std::cout << "  Press Ctrl+C to force quit" << std::endl;
    std::cout << "============================================" << std::endl;

    std::cin.get();

    std::cout << std::endl;
    std::cout << "Shutting down..." << std::endl;

    display.stop();
    soundCard.stop();
    network.stop();

    std::cout << "Cleanup complete. Goodbye!" << std::endl;
    return 0;
}
