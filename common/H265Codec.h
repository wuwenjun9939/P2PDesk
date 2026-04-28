#ifndef H265_CODEC_H
#define H265_CODEC_H

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

#include <vector>
#include <string>
#include <cstdint>

// H.265 Encoder using FFmpeg
class H265Encoder {
public:
    H265Encoder();
    ~H265Encoder();

    // Initialize encoder with parameters
    bool init(int width, int height, int fps = 60, bool useHardware = true);
    
    // Encode raw BGRA pixels to H.265 NAL units
    // Returns encoded data, sets isKeyframe if the frame is a keyframe
    std::vector<uint8_t> encode(const uint32_t* bgraPixels, int width, int height, bool* isKeyframe = nullptr);
    
    // Encode raw BGR24 pixels (more efficient, no conversion needed)
    std::vector<uint8_t> encodeBGR24(const uint8_t* bgrPixels, int width, int height, bool* isKeyframe = nullptr);

    bool isInitialized() const { return encoder_ != nullptr; }
    int getWidth() const { return width_; }
    int getHeight() const { return height_; }

private:
    bool findHardwareEncoder();
    
    AVCodecContext* encoder_ = nullptr;
    AVFrame* frame_ = nullptr;
    SwsContext* swsCtx_ = nullptr;
    AVPacket* packet_ = nullptr;
    
    int width_ = 0;
    int height_ = 0;
    int fps_ = 60;
    int64_t pts_ = 0;
    bool useHardware_ = false;
    std::string hwEncoderName_;
    
    // Hardware encoding
    AVBufferRef* hwDeviceCtx_ = nullptr;
};

// H.265 Decoder using FFmpeg
class H265Decoder {
public:
    H265Decoder();
    ~H265Decoder();

    // Initialize decoder
    bool init();
    
    // Decode H.265 NAL units to raw BGRA pixels
    // Returns true if a frame was decoded successfully
    bool decode(const uint8_t* data, size_t dataSize, uint32_t* outputBgra, int width, int height);
    
    // Decode to BGR24 (more efficient)
    bool decodeToBGR24(const uint8_t* data, size_t dataSize, uint8_t* outputBgr, int width, int height);

    bool isInitialized() const { return decoder_ != nullptr; }

private:
    AVCodecContext* decoder_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* swFrame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* swsCtx_ = nullptr;
    
    bool initialized_ = false;
};

#endif // H265_CODEC_H
