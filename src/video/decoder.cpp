#include "video/decoder.h"

#include <d3d11.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
}

#include "core/log.h"
#include "core/protocol.h"
#include "core/stats.h"

namespace wasd::video {

void FrameDeleter::operator()(AVFrame* f) const { av_frame_free(&f); }

std::recursive_mutex& d3dContextLock() {
    static std::recursive_mutex m;
    return m;
}

static void lockFn(void*) { d3dContextLock().lock(); }
static void unlockFn(void*) { d3dContextLock().unlock(); }

static AVPixelFormat pickFormat(AVCodecContext* ctx, const AVPixelFormat* fmts) {
    const bool wantHw = ctx->hw_device_ctx != nullptr;
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (wantHw && *p == AV_PIX_FMT_D3D11) return *p;
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_YUV420P || *p == AV_PIX_FMT_YUVJ420P) return *p;
    return fmts[0];
}

VideoDecoder::~VideoDecoder() { close(); }

void VideoDecoder::close() {
    if (ctx_) avcodec_free_context(&ctx_);
    if (pkt_) av_packet_free(&pkt_);
    hw_ = false;
}

bool VideoDecoder::open(uint32_t codecId, ID3D11Device* device, bool preferHardware, std::string& err) {
    close();
    codecId_ = codecId;
    const AVCodecID id = codecId == proto::kCodecH265 ? AV_CODEC_ID_HEVC
                         : codecId == proto::kCodecAv1 ? AV_CODEC_ID_AV1
                                                       : AV_CODEC_ID_H264;
    const AVCodec* codec = avcodec_find_decoder(id);
    if (!codec) {
        err = "FFmpeg decoder not available for this codec";
        return false;
    }
    ctx_ = avcodec_alloc_context3(codec);
    pkt_ = av_packet_alloc();
    if (!ctx_ || !pkt_) {
        err = "out of memory";
        return false;
    }
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
    ctx_->thread_count = 1;  // frame threading adds a frame of latency per thread
    ctx_->get_format = pickFormat;

    if (preferHardware && device) {
        AVBufferRef* hwdev = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (hwdev) {
            auto* dc = reinterpret_cast<AVHWDeviceContext*>(hwdev->data);
            auto* d3d = static_cast<AVD3D11VADeviceContext*>(dc->hwctx);
            device->AddRef();
            d3d->device = device;
            d3d->lock = lockFn;
            d3d->unlock = unlockFn;
            d3d->lock_ctx = nullptr;
            if (av_hwdevice_ctx_init(hwdev) == 0) {
                ctx_->hw_device_ctx = av_buffer_ref(hwdev);
                ctx_->extra_hw_frames = 4;  // renderer may hold frames briefly
                hw_ = true;
            } else {
                log::warn("D3D11VA init failed, using software decoding");
            }
            av_buffer_unref(&hwdev);
        }
    }
    if (avcodec_open2(ctx_, codec, nullptr) < 0) {
        err = "avcodec_open2 failed";
        close();
        return false;
    }
    log::info("decoder opened: " + description());
    return true;
}

std::string VideoDecoder::description() const {
    std::string c = codecId_ == proto::kCodecH265 ? "H.265" : codecId_ == proto::kCodecAv1 ? "AV1" : "H.264";
    return c + (hw_ ? " / D3D11VA" : " / software");
}

bool VideoDecoder::decode(const uint8_t* data, std::size_t size, uint64_t pts, bool keyFrame, int64_t rxUs,
                          const FrameFn& onFrame) {
    if (!ctx_) return false;
    pkt_->data = const_cast<uint8_t*>(data);
    pkt_->size = static_cast<int>(size);
    pkt_->pts = static_cast<int64_t>(pts);
    pkt_->flags = keyFrame ? AV_PKT_FLAG_KEY : 0;
    int r = avcodec_send_packet(ctx_, pkt_);
    av_packet_unref(pkt_);
    if (r < 0 && r != AVERROR(EAGAIN)) {
        char msg[128];
        av_strerror(r, msg, sizeof msg);
        log::warn(std::string("decode error: ") + msg);
        return r != AVERROR(ENOMEM);
    }
    for (;;) {
        AVFrame* f = av_frame_alloc();
        r = avcodec_receive_frame(ctx_, f);
        if (r < 0) {
            av_frame_free(&f);
            break;
        }
        DecodedFrame df;
        df.hardware = f->format == AV_PIX_FMT_D3D11;
        df.pts = static_cast<uint64_t>(f->pts);
        df.rxUs = rxUs;
        df.decUs = nowMicros();
        df.frame.reset(f);
        onFrame(std::move(df));
    }
    return true;
}

}  // namespace wasd::video
