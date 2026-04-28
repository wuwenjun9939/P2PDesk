#include "H265Codec.h"
#include <iostream>
#include <cstring>

// ============================================================
// H.265 Encoder Implementation
// ============================================================

H265Encoder::H265Encoder() {
    // avcodec_register_all() is deprecated in FFmpeg >= 5.0
    // Codecs are now registered automatically
}

H265Encoder::~H265Encoder() {
    if (encoder_) {
        avcodec_free_context(&encoder_);
    }
    if (frame_) {
        av_frame_free(&frame_);
    }
    if (packet_) {
        av_packet_free(&packet_);
    }
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
    }
    if (hwDeviceCtx_) {
        av_buffer_unref(&hwDeviceCtx_);
    }
}

bool H265Encoder::findHardwareEncoder() {
    // Try NVENC first (NVIDIA)
    const char* nvencName = "h264_nvenc";  // Note: for HEVC use "hevc_nvenc"
    const char* encoders[] = {"hevc_nvenc", "hevc_vaapi", "hevc_qsv", "hevc_amf"};
    
    for (const char* name : encoders) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name);
        if (codec) {
            hwEncoderName_ = name;
            std::cout << "[H265] Found hardware encoder: " << name << std::endl;
            return true;
        }
    }
    
    std::cout << "[H265] No hardware encoder found, using software encoding" << std::endl;
    return false;
}

bool H265Encoder::init(int width, int height, int fps, bool useHardware) {
    width_ = width;
    height_ = height;
    fps_ = fps;
    
    const AVCodec* codec = nullptr;
    
    if (useHardware) {
        findHardwareEncoder();
        if (!hwEncoderName_.empty()) {
            codec = avcodec_find_encoder_by_name(hwEncoderName_.c_str());
            if (codec) {
                useHardware_ = true;
                
                // Create hardware device context for VAAPI
                if (hwEncoderName_.find("vaapi") != std::string::npos) {
                    if (av_hwdevice_ctx_create(&hwDeviceCtx_, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) < 0) {
                        std::cout << "[H265] Failed to create VAAPI device context, falling back to software" << std::endl;
                        useHardware_ = false;
                        hwDeviceCtx_ = nullptr;
                        codec = avcodec_find_encoder_by_name("libx265");
                    }
                }
            }
        }
    }
    
    if (!codec) {
        // Try libx265 (software)
        codec = avcodec_find_encoder_by_name("libx265");
    }
    
    if (!codec) {
        // Try native HEVC encoder
        codec = avcodec_find_encoder(AV_CODEC_ID_HEVC);
    }
    
    if (!codec) {
        std::cerr << "[H265] No HEVC encoder found" << std::endl;
        return false;
    }
    
    encoder_ = avcodec_alloc_context3(codec);
    if (!encoder_) {
        std::cerr << "[H265] Failed to allocate codec context" << std::endl;
        return false;
    }
    
    encoder_->width = width;
    encoder_->height = height;
    encoder_->time_base = {1, (int)fps};
    encoder_->framerate = {fps, 1};
    encoder_->pix_fmt = useHardware_ && hwEncoderName_.find("vaapi") != std::string::npos 
                        ? AV_PIX_FMT_VAAPI : AV_PIX_FMT_BGR24;
    encoder_->gop_size = fps;  // Keyframe every second
    encoder_->max_b_frames = 0;  // Low latency: no B-frames
    encoder_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    
    if (useHardware_) {
        // Hardware encoding settings
        encoder_->bit_rate = 4000000;  // 4 Mbps
        av_opt_set(encoder_->priv_data, "preset", "ll", 0);  // low latency
        av_opt_set(encoder_->priv_data, "tune", "zerolatency", 0);
        if (hwDeviceCtx_) {
            encoder_->hw_device_ctx = av_buffer_ref(hwDeviceCtx_);
        }
    } else {
        // Software encoding settings (optimized for speed)
        encoder_->bit_rate = 3000000;  // 3 Mbps
        av_opt_set(encoder_->priv_data, "preset", "ultrafast", 0);
        av_opt_set(encoder_->priv_data, "tune", "zerolatency", 0);
        av_opt_set_int(encoder_->priv_data, "x265-params", 0, 0);
    }
    
    // Additional low latency settings
    encoder_->refs = 1;
    encoder_->thread_count = 0;  // Auto
    encoder_->thread_type = FF_THREAD_SLICE;
    
    if (avcodec_open2(encoder_, codec, nullptr) < 0) {
        std::cerr << "[H265] Failed to open codec" << std::endl;
        avcodec_free_context(&encoder_);
        return false;
    }
    
    // Allocate frame
    frame_ = av_frame_alloc();
    frame_->format = encoder_->pix_fmt;
    frame_->width = width;
    frame_->height = height;
    
    if (av_frame_get_buffer(frame_, 0) < 0) {
        std::cerr << "[H265] Failed to allocate frame buffer" << std::endl;
        return false;
    }
    
    // Allocate packet
    packet_ = av_packet_alloc();
    
    // Create swscale context for BGRA -> BGR24 conversion
    swsCtx_ = sws_getContext(
        width, height, AV_PIX_FMT_BGRA,
        width, height, AV_PIX_FMT_BGR24,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
    );
    
    std::cout << "[H265] Encoder initialized: " << width << "x" << height 
              << " @" << fps << "fps, " << (useHardware_ ? "Hardware" : "Software") << std::endl;
    
    return true;
}

std::vector<uint8_t> H265Encoder::encode(const uint32_t* bgraPixels, int width, int height, bool* isKeyframe) {
    // Convert BGRA to BGR24
    std::vector<uint8_t> bgrBuffer(width * height * 3);
    
    const uint8_t* srcSlice[1] = { reinterpret_cast<const uint8_t*>(bgraPixels) };
    int srcStride[1] = { width * 4 };
    uint8_t* dstSlice[1] = { bgrBuffer.data() };
    int dstStride[1] = { width * 3 };
    
    sws_scale(swsCtx_, srcSlice, srcStride, 0, height, dstSlice, dstStride);
    
    return encodeBGR24(bgrBuffer.data(), width, height, isKeyframe);
}

std::vector<uint8_t> H265Encoder::encodeBGR24(const uint8_t* bgrPixels, int width, int height, bool* isKeyframe) {
    std::vector<uint8_t> output;
    
    if (!encoder_ || !frame_) {
        return output;
    }
    
    // Copy pixel data to frame
    if (av_frame_make_writable(frame_) < 0) {
        return output;
    }
    
    const uint8_t* srcSlice[1] = { bgrPixels };
    int srcStride[1] = { width * 3 };
    
    // If frame format is BGR24, copy directly
    if (frame_->format == AV_PIX_FMT_BGR24) {
        av_image_copy_plane(frame_->data[0], frame_->linesize[0], 
                           bgrPixels, width * 3, 
                           width * 3, height);
    }
    
    frame_->pts = pts_++;
    frame_->width = width;
    frame_->height = height;
    
    // Send frame to encoder
    int ret = avcodec_send_frame(encoder_, frame_);
    if (ret < 0) {
        std::cerr << "[H265] Error sending frame to encoder" << std::endl;
        return output;
    }
    
    // Receive encoded packets
    while (ret >= 0) {
        ret = avcodec_receive_packet(encoder_, packet_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            std::cerr << "[H265] Error receiving packet" << std::endl;
            break;
        }
        
        // Check if keyframe
        if (isKeyframe) {
            *isKeyframe = (packet_->flags & AV_PKT_FLAG_KEY) != 0;
        }
        
        // Append to output
        output.insert(output.end(), packet_->data, packet_->data + packet_->size);
        
        av_packet_unref(packet_);
    }
    
    return output;
}

// ============================================================
// H.265 Decoder Implementation
// ============================================================

H265Decoder::H265Decoder() {
    // avcodec_register_all() is deprecated in FFmpeg >= 5.0
    // Codecs are now registered automatically
}

H265Decoder::~H265Decoder() {
    if (decoder_) {
        avcodec_free_context(&decoder_);
    }
    if (frame_) {
        av_frame_free(&frame_);
    }
    if (swFrame_) {
        av_frame_free(&swFrame_);
    }
    if (packet_) {
        av_packet_free(&packet_);
    }
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
    }
}

bool H265Decoder::init() {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!codec) {
        std::cerr << "[H265] HEVC decoder not found" << std::endl;
        return false;
    }
    
    decoder_ = avcodec_alloc_context3(codec);
    if (!decoder_) {
        std::cerr << "[H265] Failed to allocate decoder context" << std::endl;
        return false;
    }
    
    // Low latency settings
    decoder_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    decoder_->flags2 |= AV_CODEC_FLAG2_FAST;
    decoder_->thread_count = 0;  // Auto
    
    if (avcodec_open2(decoder_, codec, nullptr) < 0) {
        std::cerr << "[H265] Failed to open decoder" << std::endl;
        avcodec_free_context(&decoder_);
        return false;
    }
    
    frame_ = av_frame_alloc();
    swFrame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    
    initialized_ = true;
    std::cout << "[H265] Decoder initialized" << std::endl;
    return true;
}

bool H265Decoder::decode(const uint8_t* data, size_t dataSize, uint32_t* outputBgra, int width, int height) {
    if (!decoder_ || !initialized_) {
        return false;
    }
    
    // Create packet from data
    packet_->data = const_cast<uint8_t*>(data);
    packet_->size = dataSize;
    
    // Send packet to decoder
    int ret = avcodec_send_packet(decoder_, packet_);
    if (ret < 0) {
        std::cerr << "[H265] Error sending packet to decoder" << std::endl;
        return false;
    }
    
    // Receive decoded frame
    ret = avcodec_receive_frame(decoder_, frame_);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return false;
    } else if (ret < 0) {
        std::cerr << "[H265] Error receiving frame" << std::endl;
        return false;
    }
    
    // Convert to BGRA
    if (!swsCtx_ || frame_->width != width || frame_->height != height) {
        sws_freeContext(swsCtx_);
        swsCtx_ = sws_getContext(
            frame_->width, frame_->height, (AVPixelFormat)frame_->format,
            width, height, AV_PIX_FMT_BGRA,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
        );
    }
    
    if (!swsCtx_) {
        return false;
    }
    
    uint8_t* dstSlice[1] = { reinterpret_cast<uint8_t*>(outputBgra) };
    int dstStride[1] = { width * 4 };
    
    sws_scale(swsCtx_, frame_->data, frame_->linesize, 0, height, dstSlice, dstStride);
    
    return true;
}

bool H265Decoder::decodeToBGR24(const uint8_t* data, size_t dataSize, uint8_t* outputBgr, int width, int height) {
    if (!decoder_ || !initialized_) {
        return false;
    }
    
    packet_->data = const_cast<uint8_t*>(data);
    packet_->size = dataSize;
    
    int ret = avcodec_send_packet(decoder_, packet_);
    if (ret < 0) {
        return false;
    }
    
    ret = avcodec_receive_frame(decoder_, frame_);
    if (ret < 0) {
        return false;
    }
    
    // Convert to BGR24
    if (!swsCtx_ || frame_->width != width || frame_->height != height) {
        sws_freeContext(swsCtx_);
        swsCtx_ = sws_getContext(
            frame_->width, frame_->height, (AVPixelFormat)frame_->format,
            width, height, AV_PIX_FMT_BGR24,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
        );
    }
    
    if (!swsCtx_) {
        return false;
    }
    
    uint8_t* dstSlice[1] = { outputBgr };
    int dstStride[1] = { width * 3 };
    
    sws_scale(swsCtx_, frame_->data, frame_->linesize, 0, height, dstSlice, dstStride);
    
    return true;
}
