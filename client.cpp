/**
 * Client: Remote Display Receiver + Audio Playback
 * 
 * Architecture:
 *   UDP (port 9876)  - Receives video frames + audio stream
 *   TCP (port 9877)  - Sends control messages (mouse/keyboard)
 * 
 * Codec:
 *   Keyframe (every 30 frames): RLE compression
 *   Delta frame: Only changed pixels
 * 
 * Compile:
 *   gcc -std=c++17 -Wall -Wextra -O2 client.cpp -o client \
 *       -lasound -lpthread -lX11 -lstdc++ -lm
 */

#include "common/H265Codec.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <alsa/asoundlib.h>

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
#include <condition_variable>
#include <algorithm>

// Network headers
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

// ============================================================
// Network Protocol (must match server)
// ============================================================

#pragma pack(push, 1)
struct FrameHeader {
    uint32_t magic;
    uint32_t frame_number;
    uint16_t width;
    uint16_t height;
    uint8_t  codec;         // 0=raw, 1=RLE, 2=delta
    uint8_t  fragment_id;
    uint16_t fragment_total;
    uint32_t fragment_size;
    uint32_t pixel_offset;
};

struct AudioHeader {
    uint32_t magic;
    uint32_t timestamp_us;
    uint16_t sample_rate;
    uint8_t  channels;
    uint8_t  sample_count;
};

enum ControlType : uint32_t {
    PING = 1, PONG = 2, STATUS = 3, SHUTDOWN = 4,
    MOUSE_MOVE = 10, MOUSE_CLICK = 11, MOUSE_SCROLL = 12,
    KEY_PRESS = 13, KEY_RELEASE = 14,
    CONTROL_ENABLE = 15, CONTROL_DISABLE = 16,
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
constexpr size_t MAX_UDP_PAYLOAD = 1400;

// ============================================================
// Video Codec Decoder
// ============================================================

class VideoDecoder {
public:
    VideoDecoder() {
        decoder_.init();
    }

    void decode(const uint8_t* data, size_t data_size, uint32_t width, uint32_t height, 
                uint8_t codec, uint32_t*& output) {
        if (codec == 3) { // H.265
            if (current_frame_.size() < width * height) {
                current_frame_.resize(width * height);
            }
            if (decoder_.decode(data, data_size, current_frame_.data(), width, height)) {
                output = current_frame_.data();
            }
        }
    }

private:
    H265Decoder decoder_;
    std::vector<uint32_t> current_frame_;
};

// ============================================================
// Network Client (UDP Video/Audio + TCP Control)
// ============================================================

class NetworkClient {
public:
    NetworkClient(const std::string& host, uint16_t udp_port, uint16_t tcp_port) 
        : host_(host), udp_port_(udp_port), tcp_port_(tcp_port),
          udp_fd_(-1), tcp_fd_(-1), running_(false), connected_(false) {}

    ~NetworkClient() {
        disconnect();
    }

    bool connect() {
        std::cout << "[Network] Connecting to " << host_ << "..." << std::endl;
        std::cout << "[Network]   UDP (video/audio): port " << udp_port_ << std::endl;
        std::cout << "[Network]   TCP (control):     port " << tcp_port_ << std::endl;

        // Create UDP socket
        udp_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_fd_ < 0) {
            std::cout << "[Network] Failed to create UDP socket: " << strerror(errno) << std::endl;
            return false;
        }

        // Create TCP socket and connect
        tcp_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (tcp_fd_ < 0) {
            std::cout << "[Network] Failed to create TCP socket: " << strerror(errno) << std::endl;
            close(udp_fd_);
            return false;
        }

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(tcp_port_);
        inet_pton(AF_INET, host_.c_str(), &addr.sin_addr);

        if (::connect(tcp_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cout << "[Network] Failed to connect TCP: " << strerror(errno) << std::endl;
            close(udp_fd_);
            close(tcp_fd_);
            return false;
        }

        // Bind UDP to same local port (for NAT traversal)
        struct sockaddr_in udp_addr{};
        udp_addr.sin_family = AF_INET;
        udp_addr.sin_addr.s_addr = INADDR_ANY;
        udp_addr.sin_port = htons(0); // Let OS choose

        if (bind(udp_fd_, (struct sockaddr*)&udp_addr, sizeof(udp_addr)) < 0) {
            std::cout << "[Network] Warning: Failed to bind UDP: " << strerror(errno) << std::endl;
        }

        running_.store(true);
        connected_.store(true);
        udp_recv_thread_ = std::thread(&NetworkClient::udpReceiveLoop, this);
        std::cout << "[Network] Connected (dual-channel)" << std::endl;
        return true;
    }

    void disconnect() {
        if (!running_.load()) return;
        running_.store(false);

        if (udp_fd_ >= 0) {
            close(udp_fd_);
            udp_fd_ = -1;
        }
        if (tcp_fd_ >= 0) {
            shutdown(tcp_fd_, SHUT_RDWR);
            close(tcp_fd_);
            tcp_fd_ = -1;
        }

        if (udp_recv_thread_.joinable()) {
            udp_recv_thread_.join();
        }

        connected_.store(false);
    }

    bool isConnected() const { return connected_.load(); }

    void setFrameCallback(std::function<void(uint32_t, uint32_t, uint32_t, uint8_t, const uint8_t*, size_t)> cb) {
        frame_callback_ = cb;
    }

    void setAudioCallback(std::function<void(uint32_t, uint32_t, const int16_t*, uint32_t)> cb) {
        audio_callback_ = cb;
    }

    void sendPing() {
        ControlMessage msg{PING, 0};
        send(tcp_fd_, &msg, sizeof(msg), MSG_NOSIGNAL);
    }

    void sendShutdown() {
        ControlMessage msg{SHUTDOWN, 0};
        send(tcp_fd_, &msg, sizeof(msg), MSG_NOSIGNAL);
    }

    void sendControl(const ControlMessage& msg) {
        send(tcp_fd_, &msg, sizeof(msg), MSG_NOSIGNAL);
    }

private:
    void udpReceiveLoop() {
        std::cout << "[Network] UDP receive thread started" << std::endl;

        uint8_t buffer[65536]; // Max UDP packet size
        VideoDecoder decoder;

        while (running_.load()) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(udp_fd_, &fds);

            struct timeval tv{0, 100000}; // 100ms timeout
            int ret = select(udp_fd_ + 1, &fds, nullptr, nullptr, &tv);
            if (ret <= 0) continue;

            int bytes = recv(udp_fd_, buffer, sizeof(buffer), 0);
            if (bytes <= 0) continue;

            // Parse header
            if (bytes < static_cast<int>(sizeof(FrameHeader))) continue;

            uint32_t magic = reinterpret_cast<uint32_t*>(buffer)[0];

            if (magic == FRAME_MAGIC) {
                FrameHeader* header = reinterpret_cast<FrameHeader*>(buffer);
                uint32_t width = header->width;
                uint32_t height = header->height;
                size_t data_size = bytes - sizeof(FrameHeader);

                if (frame_callback_) {
                    frame_callback_(header->frame_number, width, height, header->codec,
                                   buffer + sizeof(FrameHeader), data_size);
                }

            } else if (magic == AUDIO_MAGIC) {
                AudioHeader* header = reinterpret_cast<AudioHeader*>(buffer);
                size_t sample_data_size = bytes - sizeof(AudioHeader);
                uint32_t sample_count = header->sample_count;
                uint32_t channels = header->channels;

                if (audio_callback_ && sample_data_size == sample_count * channels * sizeof(int16_t)) {
                    audio_callback_(header->sample_rate, channels,
                                   reinterpret_cast<int16_t*>(buffer + sizeof(AudioHeader)),
                                   sample_count);
                }
            }
        }

        std::cout << "[Network] UDP receive thread stopped" << std::endl;
    }

    std::string host_;
    uint16_t udp_port_;
    uint16_t tcp_port_;
    int udp_fd_;
    int tcp_fd_;
    std::atomic<bool> running_;
    std::atomic<bool> connected_;
    std::thread udp_recv_thread_;

    std::function<void(uint32_t, uint32_t, uint32_t, uint8_t, const uint8_t*, size_t)> frame_callback_;
    std::function<void(uint32_t, uint32_t, const int16_t*, uint32_t)> audio_callback_;
};

// ============================================================
// Display Renderer (X11)
// ============================================================

class DisplayRenderer {
public:
    DisplayRenderer() : running_(false), frame_ready_(false), control_enabled_(true),
                       display_(nullptr), window_(0), gc_(nullptr), 
                       image_(nullptr), network_client_(nullptr) {
        pixel_buffer_.resize(1920 * 1080);
    }

    ~DisplayRenderer() {
        cleanup();
    }

    void setNetworkClient(NetworkClient* client) {
        network_client_ = client;
    }

    bool initialize(int width, int height) {
        width_ = width;
        height_ = height;

        std::cout << "[Display] Initializing X11 renderer..." << std::endl;

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

        XSelectInput(display_, window_, ExposureMask | KeyPressMask | KeyReleaseMask | 
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask | 
                     StructureNotifyMask | FocusChangeMask);
        XStoreName(display_, window_, "Display Client - Remote Stream (Control Enabled)");

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

        gc_ = XCreateGC(display_, window_, 0, nullptr);

        int depth = DefaultDepth(display_, screen);
        Visual* visual = DefaultVisual(display_, screen);
        image_ = XCreateImage(display_, visual, depth, ZPixmap, 0,
                             nullptr, width_, height_, 32, 0);

        std::cout << "[Display] X11 renderer initialized: " << width_ << "x" << height_ << std::endl;
        return true;
    }

    void start() {
        if (running_.load()) return;

        std::cout << "[Display] Starting render loop..." << std::endl;
        running_.store(true);
        render_thread_ = std::thread(&DisplayRenderer::renderLoop, this);
    }

    void stop() {
        if (!running_.load()) return;

        std::cout << "[Display] Stopping render loop..." << std::endl;
        running_.store(false);
        if (render_thread_.joinable()) {
            render_thread_.join();
        }
    }

    void updateFrame(uint32_t /*frame_number*/, uint32_t width, uint32_t height, 
                     uint8_t codec, const uint8_t* data, size_t data_size) {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        
        decoder_.decode(data, data_size, width, height, codec, frame_pixels_);
        
        if (frame_pixels_) {
            std::memcpy(pixel_buffer_.data(), frame_pixels_, width_ * height_ * sizeof(uint32_t));
            frame_ready_.store(true);
        }
    }

    void cleanup() {
        stop();

        if (image_) {
            XDestroyImage(image_);
            image_ = nullptr;
        }

        if (gc_) {
            XFreeGC(display_, gc_);
            gc_ = nullptr;
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
    void renderLoop() {
        std::cout << "[Display] Render thread started" << std::endl;

        uint32_t frame_count = 0;
        auto fps_start = std::chrono::steady_clock::now();
        uint32_t fps_frames = 0;

        while (running_.load()) {
            // Process X11 events (including input capture)
            XEvent event;
            while (XPending(display_)) {
                XNextEvent(display_, &event);
                
                if (event.type == DestroyNotify) {
                    running_.store(false);
                    break;
                }

                if (!network_client_ || !network_client_->isConnected()) continue;

                switch (event.type) {
                    case MotionNotify:
                        if (control_enabled_) {
                            ControlMessage msg;
                            msg.type = MOUSE_MOVE;
                            msg.mouse_pos.x = event.xmotion.x;
                            msg.mouse_pos.y = event.xmotion.y;
                            network_client_->sendControl(msg);
                        }
                        break;

                    case ButtonPress:
                        if (control_enabled_) {
                            ControlMessage msg;
                            msg.type = MOUSE_CLICK;
                            msg.mouse_click.button = event.xbutton.button - 1;
                            msg.mouse_click.x = event.xbutton.x;
                            msg.mouse_click.y = event.xbutton.y;
                            network_client_->sendControl(msg);
                        }
                        break;

                    case ButtonRelease:
                        if (control_enabled_) {
                            ControlMessage msg;
                            msg.type = MOUSE_CLICK;
                            msg.mouse_click.button = (event.xbutton.button - 1) | 0x80;
                            msg.mouse_click.x = event.xbutton.x;
                            msg.mouse_click.y = event.xbutton.y;
                            network_client_->sendControl(msg);
                        }
                        break;

                    case KeyPress: {
                        if (control_enabled_) {
                            KeySym keysym = XLookupKeysym(reinterpret_cast<XKeyEvent*>(&event), 0);
                            
                            if (keysym == XK_Escape) {
                                control_enabled_ = !control_enabled_;
                                std::cout << "[Display] Control " << (control_enabled_ ? "enabled" : "disabled") << std::endl;
                                const char* title = control_enabled_ ? 
                                    "Display Client - Remote Stream (Control Enabled)" :
                                    "Display Client - Remote Stream (Control Disabled)";
                                XStoreName(display_, window_, title);
                                XFlush(display_);
                                break;
                            }

                            ControlMessage msg;
                            msg.type = KEY_PRESS;
                            msg.key.keycode = static_cast<uint32_t>(keysym);
                            network_client_->sendControl(msg);
                        } else if (event.xkey.keycode == 0xff1b) {
                            control_enabled_ = true;
                            std::cout << "[Display] Control enabled" << std::endl;
                            XStoreName(display_, window_, "Display Client - Remote Stream (Control Enabled)");
                            XFlush(display_);
                        }
                        break;
                    }

                    case KeyRelease: {
                        if (control_enabled_) {
                            KeySym keysym = XLookupKeysym(reinterpret_cast<XKeyEvent*>(&event), 0);
                            ControlMessage msg;
                            msg.type = KEY_RELEASE;
                            msg.key.keycode = static_cast<uint32_t>(keysym);
                            network_client_->sendControl(msg);
                        }
                        break;
                    }

                    default:
                        break;
                }
            }

            if (!running_.load()) break;

            // Render if new frame available
            if (frame_ready_.load()) {
                frame_ready_.store(false);

                std::lock_guard<std::mutex> lock(buffer_mutex_);
                
                image_->data = reinterpret_cast<char*>(pixel_buffer_.data());
                XPutImage(display_, window_, gc_, image_, 0, 0, 0, 0, width_, height_);
                XFlush(display_);

                frame_count++;
                fps_frames++;
            }

            // FPS counter
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - fps_start).count();
            if (elapsed >= 1) {
                double actual_fps = static_cast<double>(fps_frames) / elapsed;
                std::cout << "[Display] FPS: " << actual_fps 
                          << " | Frame: " << frame_count << std::endl;
                fps_start = now;
                fps_frames = 0;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        std::cout << "[Display] Render thread stopped. Total frames: " << frame_count << std::endl;
    }

    std::atomic<bool> running_;
    std::atomic<bool> frame_ready_{false};
    std::atomic<bool> control_enabled_{true};
    std::thread render_thread_;
    std::mutex buffer_mutex_;

    Display* display_;
    Window window_;
    GC gc_;
    XImage* image_;

    int width_;
    int height_;
    std::vector<uint32_t> pixel_buffer_;
    uint32_t* frame_pixels_ = nullptr;
    VideoDecoder decoder_;

    NetworkClient* network_client_;
};

// ============================================================
// Audio Player (ALSA)
// ============================================================

class AudioPlayer {
public:
    AudioPlayer() : pcm_handle_(nullptr), running_(false) {}

    ~AudioPlayer() {
        stop();
    }

    bool initialize() {
        std::cout << "[Audio] Initializing audio player..." << std::endl;

        int ret = snd_pcm_open(&pcm_handle_, "default", SND_PCM_STREAM_PLAYBACK, 0);
        if (ret < 0) {
            std::cout << "[Audio] Warning: Could not open PCM device: " 
                      << snd_strerror(ret) << std::endl;
            pcm_handle_ = nullptr;
            return true;
        }

        snd_pcm_hw_params_t *hw_params;
        snd_pcm_hw_params_alloca(&hw_params);

        ret = snd_pcm_hw_params_any(pcm_handle_, hw_params);
        ret = snd_pcm_hw_params_set_access(pcm_handle_, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
        ret = snd_pcm_hw_params_set_format(pcm_handle_, hw_params, SND_PCM_FORMAT_S16_LE);
        unsigned int rate = 48000;
        ret = snd_pcm_hw_params_set_rate_near(pcm_handle_, hw_params, &rate, nullptr);
        ret = snd_pcm_hw_params_set_channels(pcm_handle_, hw_params, 2);
        ret = snd_pcm_hw_params(pcm_handle_, hw_params);
        if (ret < 0) {
            std::cout << "[Audio] Failed to set hardware parameters: " << snd_strerror(ret) << std::endl;
            snd_pcm_close(pcm_handle_);
            pcm_handle_ = nullptr;
        } else {
            std::cout << "[Audio] ALSA device configured" << std::endl;
        }

        return true;
    }

    void start() {
        if (running_.load()) return;

        std::cout << "[Audio] Starting audio playback thread..." << std::endl;
        running_.store(true);
        playback_thread_ = std::thread(&AudioPlayer::playbackLoop, this);
    }

    void stop() {
        if (!running_.load()) return;

        std::cout << "[Audio] Stopping audio playback..." << std::endl;
        running_.store(false);

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            condition_.notify_all();
        }

        if (playback_thread_.joinable()) {
            playback_thread_.join();
        }

        if (pcm_handle_) {
            snd_pcm_drain(pcm_handle_);
            snd_pcm_close(pcm_handle_);
            pcm_handle_ = nullptr;
        }
    }

    void queueAudio(const int16_t* samples, uint32_t count, uint32_t channels) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        audio_queue_.insert(audio_queue_.end(), samples, samples + count * channels);

        const size_t max_samples = 48000 * 2 * 2;
        if (audio_queue_.size() > max_samples) {
            audio_queue_.erase(audio_queue_.begin(), audio_queue_.begin() + (audio_queue_.size() - max_samples));
        }

        condition_.notify_one();
    }

private:
    void playbackLoop() {
        std::cout << "[Audio] Playback thread started" << std::endl;

        while (running_.load()) {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            condition_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                return !audio_queue_.empty() || !running_.load();
            });

            if (!running_.load()) break;
            if (audio_queue_.empty()) continue;

            const size_t chunk_size = 1024 * 2;
            if (audio_queue_.size() < chunk_size) continue;

            std::vector<int16_t> chunk(audio_queue_.begin(), audio_queue_.begin() + chunk_size);
            audio_queue_.erase(audio_queue_.begin(), audio_queue_.begin() + chunk_size);

            lock.unlock();

            if (pcm_handle_) {
                int frames = snd_pcm_writei(pcm_handle_, chunk.data(), 1024);
                if (frames < 0) {
                    snd_pcm_recover(pcm_handle_, frames, 0);
                }
            }
        }

        std::cout << "[Audio] Playback thread stopped" << std::endl;
    }

    snd_pcm_t* pcm_handle_;
    std::atomic<bool> running_;
    std::thread playback_thread_;

    std::mutex queue_mutex_;
    std::condition_variable condition_;
    std::vector<int16_t> audio_queue_;
};

// ============================================================
// Main Application (Client)
// ============================================================

int main(int argc, char* argv[]) {
    std::string server_host = "127.0.0.1";
    uint16_t udp_port = 9876;
    uint16_t tcp_port = 9877;

    if (argc > 1) {
        server_host = argv[1];
    }
    if (argc > 2) {
        udp_port = static_cast<uint16_t>(std::stoi(argv[2]));
        tcp_port = udp_port + 1;
    }

    std::cout << "============================================" << std::endl;
    std::cout << "  Display Client - Remote Stream Receiver" << std::endl;
    std::cout << "  Server: " << server_host << std::endl;
    std::cout << "  Video/Audio: UDP :" << udp_port << std::endl;
    std::cout << "  Control:     TCP :" << tcp_port << std::endl;
    std::cout << "  Codec:       RLE + Delta Frame" << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout << std::endl;

    // Initialize components
    AudioPlayer audio;
    if (!audio.initialize()) {
        std::cout << "[Error] Failed to initialize audio" << std::endl;
        return 1;
    }

    DisplayRenderer display;
    if (!display.initialize(1920, 1080)) {
        std::cout << "[Error] Failed to initialize display" << std::endl;
        return 1;
    }

    // Create network client (dual-channel)
    NetworkClient client(server_host, udp_port, tcp_port);

    // Wire display to network client
    display.setNetworkClient(&client);

    // Set callbacks
    client.setFrameCallback([&](uint32_t /*frame_num*/, uint32_t width, uint32_t height, 
                                 uint8_t codec, const uint8_t* data, size_t data_size) {
        display.updateFrame(0, width, height, codec, data, data_size);
    });

    client.setAudioCallback([&](uint32_t /*sample_rate*/, uint32_t channels, 
                                const int16_t* samples, uint32_t sample_count) {
        audio.queueAudio(samples, sample_count, channels);
    });

    // Connect to server
    if (!client.connect()) {
        std::cout << "[Error] Failed to connect to server" << std::endl;
        std::cout << "Make sure the server is running on " << server_host << std::endl;
        return 1;
    }

    std::cout << std::endl;

    // Start local playback
    audio.start();
    display.start();

    std::cout << "============================================" << std::endl;
    std::cout << "  Client Running" << std::endl;
    std::cout << "  Receiving stream from server..." << std::endl;
    std::cout << "  Control: Mouse/Keyboard enabled" << std::endl;
    std::cout << "  ESC: Toggle control on/off" << std::endl;
    std::cout << "  Press any key in the window or close it to exit" << std::endl;
    std::cout << "  Press Ctrl+C to force quit" << std::endl;
    std::cout << "============================================" << std::endl;

    // Periodically send ping
    auto ping_start = std::chrono::steady_clock::now();
    while (client.isConnected()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - ping_start).count();
        if (elapsed >= 5) {
            client.sendPing();
            ping_start = now;
        }
    }

    std::cout << std::endl;
    std::cout << "Disconnecting..." << std::endl;

    display.stop();
    audio.stop();
    client.disconnect();

    std::cout << "Cleanup complete. Goodbye!" << std::endl;
    return 0;
}
