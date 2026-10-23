// =============================================================================
//  encoder.h - 视频编码（FFmpeg libavcodec，优先硬件）
//
//  编码器优先级：NVENC -> AMF -> QSV -> libx264 软编兜底。
//  低延迟关键设置（缺一不可）：
//    AV_CODEC_FLAG_LOW_DELAY、无 B 帧、无 lookahead、无限 GOP + 按需 IDR、
//    CBR 码率控制、preset 取最快档。
//  码流用 Annex-B，SPS/PPS 随每个 IDR 内联，丢包后能独立恢复。
// =============================================================================
#pragma once

#include "../common/types.h"
#include "../common/protocol.h"
#include "capture.h"
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

namespace ms::host {

struct EncoderConfig {
    std::string preferred = "auto";   // auto|nvenc|amf|qsv|cpu
    proto::VideoCodec codec = proto::VideoCodec::H264;
    int width  = 1920;
    int height = 1080;
    int fps    = 60;
    int bitrateKbps = 20000;
    int keyframeIntervalSec = 2;      // 定期 IDR，丢包恢复上限
    int gopSize = 0;                  // 0 = 按 keyframeIntervalSec 计算
    // 编码器细调（对应 Parsec 的 encoder_min_qp / encoder_max_qp /
    // encoder_slices / encoder_vbv_multi 同名键）：-1 表示不限制
    int minQp = -1;
    int maxQp = -1;
    int slices = 1;
    int vbvMultiplier = 5;            // VBV = 码率 × N ÷ fps
};

struct EncodedPacket {
    Bytes data;            // Annex-B 码流
    bool  keyframe = false;
    i64   ptsUs = 0;
    i64   encodeStartUs = 0;
    i64   encodeDoneUs  = 0;
    int   encodeLatencyUs() const { return int(encodeDoneUs - encodeStartUs); }
};

class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    // 按 preferred 顺序尝试，成功后 available_encoder 给出实际用的编码器名
    bool init(const EncoderConfig& cfg, std::string& err);
    void shutdown();
    bool is_open() const { return ctx_ != nullptr; }

    // 提交一帧（NV12 系统内存）。
    //  返回 1 = 已接收；0 = 编码器缓冲满，调用方稍后重试同帧；<0 = 出错
    int submit(const u8* y, int strideY, const u8* uv, int strideUV,
               i64 ptsUs, bool forceKeyframe);

    // 非阻塞收取编码结果。timeoutMs 仅在编码器内部缓冲时用于短暂等待。
    //  返回 1 = 取到一个 packet；0 = 暂无；<0 = 出错
    int poll(EncodedPacket& out, int timeoutMs);

    // 运行中调整码率（自适应用）
    bool set_bitrate(int kbps);

    const std::string& encoder_name() const { return encoderName_; }
    bool is_hardware() const { return hardware_; }
    proto::VideoCodec codec() const { return cfg_.codec; }
    int width()  const { return cfg_.width; }
    int height() const { return cfg_.height; }

    // 探测本机可用的硬件编码器（UI 里展示用）
    static std::string probe_best_encoder(proto::VideoCodec codec);

private:
    AVCodecContext* ctx_   = nullptr;
    AVFrame*        frame_ = nullptr;
    AVPacket*       pkt_   = nullptr;
    EncoderConfig   cfg_;
    std::string     encoderName_;
    bool            hardware_ = false;
    i64             frameIndex_ = 0;
};

} // namespace ms::host
