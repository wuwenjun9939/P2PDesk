#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <cstdint>

#pragma pack(push, 1)

// UDP Video/Audio stream port
constexpr uint16_t DEFAULT_UDP_PORT = 9876;
// TCP Control port
constexpr uint16_t DEFAULT_TCP_PORT = 9877;

// UDP MTU-safe fragment size
constexpr size_t MAX_UDP_PAYLOAD = 1400;

// Magic numbers
constexpr uint32_t FRAME_MAGIC = 0x44535059; // "DSPY"
constexpr uint32_t AUDIO_MAGIC = 0x41554449; // "AUDI"

// Frame header for UDP video streaming
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

// Audio header for UDP audio streaming
struct AudioHeader {
    uint32_t magic;         // 0x41554449 = "AUDI"
    uint32_t timestamp_us;  // microseconds
    uint16_t sample_rate;
    uint8_t  channels;
    uint8_t  sample_count;  // max 255 samples per packet
    // Followed by sample_count * channels * sizeof(int16_t)
};

// Control message types (TCP)
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

// Control message for TCP communication
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

#endif // PROTOCOL_H
