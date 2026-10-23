// =============================================================================
//  decoder.cpp - 视频解码实现（FFmpeg libavcodec，优先 D3D11VA 硬解）
//
//  两条路径
//   * 硬解：自己建 D3D11 设备（BGRA_SUPPORT | VIDEO_SUPPORT），塞进
//     AVHWDeviceContext 交给 FFmpeg；get_format 只接受 AV_PIX_FMT_D3D11，
//     解出来的 AVFrame 里 data[0] 是 ID3D11Texture2D*、data[1] 是数组层下标，
//     渲染器可以零拷贝直接采样。
//   * 软解：preferHardware 为假、或硬解初始化/打开失败时回退，输出 CPU 的
//     NV12 / YUV420P。
//
//  低延迟：thread_count = 0（自动）、AV_CODEC_FLAG_LOW_DELAY、无重排等待，
//  receive() 用 1ms 粒度的超时轮询而不是阻塞。
//
//  设备所有权（关键，容易漏引用计数）
//    D3D11CreateDevice 出来时引用计数 = 1（我们的）；
//    再 AddRef 一次（= 2）后把其中一个交给 FFmpeg，它释放 AVHWDeviceContext 时
//    会 Release 掉一个；剩下的那个由 d3d11Device_ 持有，shutdown() 里 Release。
// =============================================================================

#include "decoder.h"

#include "../common/clock.h"

#include <d3d10.h>   // ID3D10Multithread：D3D11 设备也用它打开多线程保护
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixfmt.h>

#include <climits>
#include <cstring>
#include <string>

namespace ms::client {

namespace {

// AVERROR 码 -> 可读字符串，用于填 err
std::string av_err_text(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(code, buf, sizeof(buf)) < 0) {
        return "错误码 " + std::to_string(code);
    }
    return std::string(buf);
}

// 微秒时基：pkt 的 pts/dts 与 pkt_timebase 都用它
constexpr AVRational kUsTimeBase{1, 1000000};

} // namespace

VideoDecoder::VideoDecoder() = default;

VideoDecoder::~VideoDecoder() {
    shutdown();
}

// -----------------------------------------------------------------------------
// 初始化
// -----------------------------------------------------------------------------
bool VideoDecoder::init(proto::VideoCodec codec, int width, int height,
                        bool preferHardware, std::string& err) {
    shutdown();
    err.clear();

    const bool hevc = (codec == proto::VideoCodec::HEVC);
    const char* wantName = hevc ? "hevc" : "h264";
    const AVCodecID wantId = hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;

    // 先按名字找（能拿到具体实现），失败再按 ID 兜底
    codec_ = avcodec_find_decoder_by_name(wantName);
    if (!codec_) {
        codec_ = avcodec_find_decoder(wantId);
    }
    if (!codec_) {
        err = std::string("找不到解码器: ") + wantName;
        return false;
    }
    decoderName_ = codec_->name ? codec_->name : wantName;
    width_  = width  > 0 ? width  : 0;
    height_ = height > 0 ? height : 0;

    // 1) 优先硬解
    std::string hwErr;
    if (preferHardware && open_hardware(hwErr)) {
        hardware_ = true;
        return true;
    }

    // 2) 软解（也是硬解的回退路径）
    std::string swErr;
    if (!open_software(swErr)) {
        err = swErr;
        if (preferHardware && !hwErr.empty()) {
            err += "（硬解尝试: " + hwErr + "）";
        }
        return false;
    }

    hardware_ = false;
    if (preferHardware) {
        // 返回 true 但 err 带上说明：调用方可以把它显示到日志/悬浮条
        err = "硬解不可用，已回退软解: " + hwErr;
    }
    return true;
}

// -----------------------------------------------------------------------------
// 硬解：自建 D3D11 设备 + AVHWDeviceContext + D3D11VA 解码器
// -----------------------------------------------------------------------------
bool VideoDecoder::open_hardware(std::string& why) {
    why.clear();

    // --- 1) 自建 D3D11 设备（渲染器稍后通过 d3d11_device() 复用它）---
    ID3D11Device* dev = nullptr;
    static const D3D_FEATURE_LEVEL kLevels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    const UINT devFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, devFlags,
                                   kLevels, ARRAYSIZE(kLevels), D3D11_SDK_VERSION,
                                   &dev, nullptr, nullptr);
    if (hr == E_INVALIDARG) {
        // 老 SDK / 老系统不认 11_1，去掉最高等级重试
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, devFlags,
                               kLevels + 1, ARRAYSIZE(kLevels) - 1, D3D11_SDK_VERSION,
                               &dev, nullptr, nullptr);
    }
    if (FAILED(hr) || !dev) {
        why = "D3D11CreateDevice 失败";
        if (dev) { dev->Release(); }
        return false;
    }

    // 引用计数：D3D11CreateDevice 出来的那一份（R1）自己拿；
    // 交给 FFmpeg 的那一份单独 AddRef（R2），它在释放 AVHWDeviceContext 时会 Release。
    // 多线程保护：这个设备/立刻上下文会被解码线程和渲染线程同时使用
    // （AVD3D11VADeviceContext 的 device_context 就是它），必须打开。
    // FFmpeg 自己 D3D11CreateDevice 时会做同样的事，用户自建设备时得自己来。
    {
        ID3D10Multithread* mt = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(&mt))) && mt) {
            mt->SetMultithreadProtected(TRUE);
            mt->Release();
        }
    }

    // --- 2) 建 AVHWDeviceContext 并把我们的设备挂上去 ---
    AVBufferRef* hwRef = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!hwRef) {
        why = "av_hwdevice_ctx_alloc 失败";
        dev->Release();                // R1
        return false;
    }
    AVHWDeviceContext* hwc = reinterpret_cast<AVHWDeviceContext*>(hwRef->data);
    AVD3D11VADeviceContext* d3dctx = reinterpret_cast<AVD3D11VADeviceContext*>(hwc->hwctx);
    dev->AddRef();                     // R2：给 FFmpeg 的一份
    d3dctx->device = dev;              // device_context / video_device 由 init 从它派生
    d3d11Device_ = dev;                // 自己持有 R1，shutdown() 里 Release
    if (av_hwdevice_ctx_init(hwRef) < 0) {
        why = "av_hwdevice_ctx_init 失败";
        av_buffer_unref(&hwRef);       // 释放 R2（free 回调会 Release device）
        shutdown();                    // 释放 R1
        return false;
    }
    hwDeviceCtx_ = hwRef;              // 我们持有这个 AVBufferRef
    hwPixFmt_ = AV_PIX_FMT_D3D11;

    // --- 3) 解码器上下文 ---
    ctx_ = avcodec_alloc_context3(codec_);
    if (!ctx_) {
        why = "avcodec_alloc_context3 失败";
        shutdown();
        return false;
    }
    ctx_->opaque = this;
    ctx_->get_format = &VideoDecoder::get_hw_format;
    ctx_->pix_fmt = hwPixFmt_;
    ctx_->hw_device_ctx = av_buffer_ref(hwDeviceCtx_);
    if (!ctx_->hw_device_ctx) {
        why = "av_buffer_ref(hw_device_ctx) 失败";
        shutdown();
        return false;
    }
    ctx_->thread_count = 1;                       // 硬解只需单线程提交，多线程只加排队延迟
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;       // 不为了吞吐攒帧
    // SHOW_ALL：即使还没有参考帧/首帧不完整也把解出来的东西吐出来，
    // 避免解码器"憋着不出帧"造成延迟堆积（Moonlight 的同款设置）。
    ctx_->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
    // OUTPUT_CORRUPT：位流有小问题时也允许把解出来的帧交出来，不要整帧黑掉；
    // 真正的结构性错误由下面的 err_recognition 直接报错，交给"等 IDR"状态机处理。
    // （这两条是 Moonlight ffmpeg.cpp 的低延迟组合，实测能去掉周期性顿一下）
    ctx_->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    ctx_->err_recognition = AV_EF_EXPLODE;
    ctx_->extra_hw_frames = 8;                    // 硬解表面池留余量，避免取不到 surface
    ctx_->width  = width_;
    ctx_->height = height_;
    ctx_->pkt_timebase = kUsTimeBase;             // 我们的 pts/dts 单位是微秒
    ctx_->time_base    = kUsTimeBase;

    const int openRet = avcodec_open2(ctx_, codec_, nullptr);
    if (openRet < 0) {
        why = "硬解 avcodec_open2 失败: " + av_err_text(openRet);
        shutdown();
        return false;
    }

    frame_ = av_frame_alloc();
    pkt_   = av_packet_alloc();
    if (!frame_ || !pkt_) {
        why = "av_frame_alloc/av_packet_alloc 失败";
        shutdown();
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// 软解
// -----------------------------------------------------------------------------
bool VideoDecoder::open_software(std::string& why) {
    why.clear();
    shutdown();                       // 清掉硬解可能留下的半成品

    ctx_ = avcodec_alloc_context3(codec_);
    if (!ctx_) {
        why = "avcodec_alloc_context3 失败";
        return false;
    }
    ctx_->thread_count = 0;                  // 自动
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx_->flags2 |= AV_CODEC_FLAG2_SHOW_ALL; // 有帧就吐，别憋着
    ctx_->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    ctx_->err_recognition = AV_EF_EXPLODE;   // 真出错就报错，交给"等 IDR"状态机
    ctx_->width  = width_;
    ctx_->height = height_;
    ctx_->pkt_timebase = kUsTimeBase;
    ctx_->time_base    = kUsTimeBase;
    // 软解不设 pix_fmt / get_format / hw_device_ctx，让解码器自己选输出格式
    // （h264/hevc 软解通常是 NV12，也可能是 YUV420P，渲染器两条都支持）

    const int openRet = avcodec_open2(ctx_, codec_, nullptr);
    if (openRet < 0) {
        why = "软解 avcodec_open2 失败: " + av_err_text(openRet);
        shutdown();
        return false;
    }

    frame_ = av_frame_alloc();
    pkt_   = av_packet_alloc();
    if (!frame_ || !pkt_) {
        why = "av_frame_alloc/av_packet_alloc 失败";
        shutdown();
        return false;
    }
    hwPixFmt_ = AV_PIX_FMT_NONE;
    return true;
}

// get_format 回调：只在候选里挑我们要的硬件格式，挑不到就失败（触发软解重试）
AVPixelFormat VideoDecoder::get_hw_format(AVCodecContext* ctx, const AVPixelFormat* fmts) {
    const VideoDecoder* self = static_cast<const VideoDecoder*>(ctx->opaque);
    const AVPixelFormat want = (self && self->hwPixFmt_ != AV_PIX_FMT_NONE)
                                   ? self->hwPixFmt_
                                   : AV_PIX_FMT_D3D11;
    for (const AVPixelFormat* p = fmts; p && *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == want) {
            return want;
        }
    }
    return AV_PIX_FMT_NONE;
}

// -----------------------------------------------------------------------------
// 送包 / 取帧
// -----------------------------------------------------------------------------
int VideoDecoder::send(const u8* data, size_t len, i64 ptsUs) {
    if (!ctx_ || !pkt_ || !data || len == 0) {
        return -1;
    }
    if (len > static_cast<size_t>(INT_MAX)) {
        return -1;
    }

    // 自己拷一份到 pkt_ 的缓冲区：调用方的 buffer 生命周期不归我们管
    const int allocRet = av_new_packet(pkt_, static_cast<int>(len));
    if (allocRet < 0) {
        return allocRet;
    }
    std::memcpy(pkt_->data, data, len);
    pkt_->pts = ptsUs;      // 时基 {1,1000000}
    pkt_->dts = ptsUs;      // 无 B 帧，两者相同
    pkt_->flags = 0;

    const int ret = avcodec_send_packet(ctx_, pkt_);
    av_packet_unref(pkt_);  // 送完就还，下一帧复用同一块 AVPacket

    if (ret == AVERROR(EAGAIN)) {
        return 0;           // 解码器缓冲满：调用方稍后重送同一帧
    }
    if (ret < 0) {
        return ret;
    }
    return 1;
}

int VideoDecoder::receive(AVFrame*& out, int timeoutMs) {
    out = nullptr;
    if (!ctx_ || !frame_) {
        return -1;
    }

    const i64 waitUs = static_cast<i64>(timeoutMs > 0 ? timeoutMs : 0) * 1000;
    const i64 deadline = now_us() + waitUs;

    for (;;) {
        const int ret = avcodec_receive_frame(ctx_, frame_);
        if (ret == 0) {
            out = frame_;              // 返回值就是内部持有的 frame_
            return 1;                  // 调用方用完必须 release()
        }
        if (ret == AVERROR(EAGAIN)) {
            if (waitUs <= 0 || now_us() >= deadline) {
                return 0;
            }
            sleep_ms(1);               // 1ms 粒度足够：1080p60 每帧 16.7ms
            continue;
        }
        if (ret == AVERROR_EOF) {
            return 0;                  // flush() 之后没有更多帧
        }
        return ret;
    }
}

void VideoDecoder::release(AVFrame* frame) {
    if (frame && frame != frame_) {
        av_frame_unref(frame);
    }
    if (frame_) {
        av_frame_unref(frame_);
    }
}

void VideoDecoder::flush() {
    if (frame_) {
        av_frame_unref(frame_);
    }
    if (ctx_) {
        avcodec_flush_buffers(ctx_);
    }
}

ID3D11Device* VideoDecoder::d3d11_device() const {
    return hardware_ ? d3d11Device_ : nullptr;
}

// -----------------------------------------------------------------------------
// 释放（可重复调用）
// -----------------------------------------------------------------------------
void VideoDecoder::shutdown() {
    if (pkt_) {
        av_packet_free(&pkt_);
    }
    if (frame_) {
        av_frame_free(&frame_);
    }
    if (ctx_) {
        // 顺带 unref ctx_->hw_device_ctx
        avcodec_free_context(&ctx_);
    }
    if (hwDeviceCtx_) {
        // 最后一个引用：触发 D3D11 设备的 Release（FFmpeg 那一份）
        av_buffer_unref(&hwDeviceCtx_);
    }
    if (d3d11Device_) {
        d3d11Device_->Release();       // 我们自己的那一份
        d3d11Device_ = nullptr;
    }
    hardware_ = false;
    hwPixFmt_ = AV_PIX_FMT_NONE;
}

} // namespace ms::client
