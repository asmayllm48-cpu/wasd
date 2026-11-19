// H.264/H.265 decoding with FFmpeg. Hardware path: D3D11VA on the renderer's
// device (frames stay on the GPU). Software fallback outputs YUV420P.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct ID3D11Device;

namespace wasd::video {

struct FrameDeleter {
    void operator()(AVFrame* f) const;
};
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

struct DecodedFrame {
    FramePtr frame;
    int64_t rxUs = 0;    // time the last byte of the packet was received
    int64_t decUs = 0;   // time the decoder returned the frame
    uint64_t pts = 0;
    bool hardware = false;
};

// Lock shared by the decoder (D3D11VA) and the renderer for the immediate context.
std::recursive_mutex& d3dContextLock();

class VideoDecoder {
public:
    using FrameFn = std::function<void(DecodedFrame&&)>;
    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // codecId: proto::kCodecH264 / kCodecH265. device may be null (software only).
    bool open(uint32_t codecId, ID3D11Device* device, bool preferHardware, std::string& err);
    void close();
    // Feeds one complete access unit (config already merged in). Returns false on fatal error.
    bool decode(const uint8_t* data, std::size_t size, uint64_t pts, bool keyFrame, int64_t rxUs, const FrameFn& onFrame);
    bool hardware() const { return hw_; }
    std::string description() const;

private:
    AVCodecContext* ctx_ = nullptr;
    AVPacket* pkt_ = nullptr;
    bool hw_ = false;
    uint32_t codecId_ = 0;
};

}  // namespace wasd::video
