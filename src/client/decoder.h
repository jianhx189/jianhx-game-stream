// =============================================================================
//  decoder.h - 视频解码（FFmpeg libavcodec，优先 D3D11 硬解）
//
//  硬解产出 AV_PIX_FMT_D3D11（GPU 纹理，零拷贝交给渲染器）；
//  如果硬解不可用则回退软解，产出 CPU 的 NV12/YUV420P。
// =============================================================================
#pragma once

#include "../common/types.h"
#include "../common/protocol.h"
#include <string>

// d3d11_device() 返回 ID3D11Device*，编译器必须认识这个类型。
// avcodec.h 不会带进 d3d11.h，所以这里显式包含（与 host/capture.h 的做法一致）。
#include <d3d11.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace ms::client {

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool init(proto::VideoCodec codec, int width, int height,
              bool preferHardware, std::string& err);
    void shutdown();
    bool is_open() const { return ctx_ != nullptr; }
    bool is_hardware() const { return hardware_; }

    // 送入一个完整压缩帧（Annex-B，可能含 SPS/PPS/IDR）。
    //  返回 1 = 已接收；0 = 解码器缓冲满（调用方稍后重试）；<0 = 出错
    int send(const u8* data, size_t len, i64 ptsUs);

    // 取解码结果。
    //  返回 1 = 拿到一帧（out 已在内部持有引用，调用方用完必须调 release()）
    //       0 = 暂无输出
    //      <0 = 出错
    int receive(AVFrame*& out, int timeoutMs);

    // 归还上一帧
    void release(AVFrame* frame);

    void flush();

    const std::string& decoder_name() const { return decoderName_; }
    // 硬解时返回其 D3D11 设备（渲染器需要共享同一设备）
    ID3D11Device* d3d11_device() const;

private:
    // 硬解初始化：成功返回 true；失败返回 false，并保证不残留半成品
    bool open_hardware(std::string& why);
    // 软解初始化（硬解失败回退时也走这里）
    bool open_software(std::string& why);
    // get_format 回调：在解码器给出的候选格式里挑我们要的硬件格式
    static AVPixelFormat get_hw_format(AVCodecContext* ctx, const AVPixelFormat* fmts);

    AVCodecContext* ctx_ = nullptr;
    AVBufferRef*    hwDeviceCtx_ = nullptr;
    AVFrame*        frame_ = nullptr;
    AVPacket*       pkt_ = nullptr;          // send() 复用，避免每帧分配
    std::string     decoderName_;
    bool            hardware_ = false;

    // --- 以下为实现内部状态 ---
    const AVCodec*  codec_ = nullptr;        // avcodec_find_decoder* 的结果（静态对象，不需释放）
    ID3D11Device*   d3d11Device_ = nullptr;  // 硬解自建的设备，自己持有一份引用
    AVPixelFormat   hwPixFmt_ = AV_PIX_FMT_NONE;
    int             width_ = 0;              // 协商/期望分辨率，仅用于建上下文
    int             height_ = 0;
};

} // namespace ms::client
