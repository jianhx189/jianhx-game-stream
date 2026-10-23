// =============================================================================
//  encoder.cpp - 视频编码实现（FFmpeg libavcodec）
//
//  设计要点
//   * 候选顺序 nvenc -> amf -> qsv -> cpu；auto 时逐个 avcodec_open2，成功即用。
//   * 全链路低延迟：AV_CODEC_FLAG_LOW_DELAY + 无 B 帧 + 无 lookahead +
//     最小缓冲 + CBR + 按需 IDR。
//   * 故意不设 AV_CODEC_FLAG_GLOBAL_HEADER：输出 Annex-B，SPS/PPS 内联在
//     每个 IDR 之前（用 repeat-headers=1 保证每个 IDR 都带），客户端丢包后
//     拿到下一个 IDR 就能独立解码恢复。
//   * 【不要开 intra-refresh】它用"逐帧刷新条带"替代真 IDR，刷新周期内丢包
//     留下的糊块会滞留到条带扫过；而远控端靠「请求 IDR 重新对齐解码器」恢复，
//     两者语义冲突。详见 apply_low_latency_opts 里的注释。
//   * 输入只接受 NV12 系统内存（采集侧已经用 VideoProcessor 转好），
//     不在这里做软件色彩转换（1080p60 的 CPU 转换要吃掉 1~2 个核）。
// =============================================================================
#include "encoder.h"

#include "../common/clock.h"
#include "../common/log.h"

// encoder.h 已经把 avcodec/opt 放进 extern "C"，这里补两个本文件要用的
extern "C" {
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

#include <cstring>
#include <string>
#include <vector>

namespace ms::host {
namespace {

// -----------------------------------------------------------------------------
// 候选编码器
// -----------------------------------------------------------------------------
struct Candidate {
    const char* name;
    bool        hardware;
};

std::string lower_ascii(const std::string& s) {
    std::string r = s;
    for (char& c : r) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    return r;
}

// 按 preferred 生成尝试列表。
//   auto（或无法识别的值）= 硬件优先，全部按顺序试；
//   显式指定某一族时只试该族，失败就直接报错（不偷偷降到软编）。
std::vector<Candidate> build_candidates(const std::string& preferred, proto::VideoCodec codec) {
    const bool hevc = (codec == proto::VideoCodec::HEVC);

    const Candidate nvenc{ hevc ? "hevc_nvenc" : "h264_nvenc", true  };
    const Candidate amf  { hevc ? "hevc_amf"   : "h264_amf",   true  };
    const Candidate qsv  { hevc ? "hevc_qsv"   : "h264_qsv",   true  };
    const Candidate cpu  { hevc ? "libx265"    : "libx264",    false };

    const std::string p = lower_ascii(preferred);
    if (p == "nvenc") return std::vector<Candidate>{ nvenc };
    if (p == "amf")   return std::vector<Candidate>{ amf   };
    if (p == "qsv")   return std::vector<Candidate>{ qsv   };
    if (p == "cpu" || p == "software" || p == "x264" || p == "x265") {
        return std::vector<Candidate>{ cpu };
    }
    return std::vector<Candidate>{ nvenc, amf, qsv, cpu };
}

const char* codec_tag(proto::VideoCodec c) {
    return c == proto::VideoCodec::HEVC ? "hevc" : "h264";
}

// -----------------------------------------------------------------------------
// 私有参数设置：不同编码器支持的键完全不同，因此逐个尝试、失败一律忽略。
// 绝不能因为某个编码器不认某个键就判定初始化失败。
// -----------------------------------------------------------------------------
bool set_priv_str(AVCodecContext* ctx, const char* key, const char* val) {
    return ctx->priv_data != nullptr && av_opt_set(ctx->priv_data, key, val, 0) == 0;
}

bool set_priv_int(AVCodecContext* ctx, const char* key, i64 val) {
    return ctx->priv_data != nullptr && av_opt_set_int(ctx->priv_data, key, val, 0) == 0;
}

void apply_low_latency_opts(AVCodecContext* ctx, const std::string& name) {
    if (name.find("nvenc") != std::string::npos) {
        // NVENC 的 preset 是 p1..p7（数字越小越快、质量越低）。
        // 老版本 FFmpeg 只认 llhq/llhp 这类名字，所以按 p1 -> p4 -> llhq 退。
        if (!set_priv_str(ctx, "preset", "p1") && !set_priv_str(ctx, "preset", "p4")) {
            set_priv_str(ctx, "preset", "llhq");
        }
        set_priv_str(ctx, "tune", "ll");            // 低延迟调优
        set_priv_str(ctx, "rc", "cbr");             // 恒定码率，丢包后恢复更稳
        set_priv_str(ctx, "zerolatency", "1");      // 关掉编码器内部缓冲
        set_priv_str(ctx, "delay", "0");            // 不延迟输出
        set_priv_str(ctx, "forced-idr", "1");       // pict_type=I 时强制出 IDR
        set_priv_str(ctx, "rc-lookahead", "0");     // 前瞻会引入帧级延迟
        set_priv_str(ctx, "surfaces", "4");         // 最小 surface 池，抑制排队
        set_priv_str(ctx, "bf", "0");               // 无 B 帧
        // ---- 【这里故意不开 intra-refresh】----
        // 旧实现设了 "intra-refresh" 并把 GOP 压到 1 秒，想"丢包自愈"，实测它
        // 反而是画面局部花屏/糊块的根源（用户抱怨的正是"一条持续约 1 秒的糊块"）：
        //   1) intra-refresh 把一个 IDR 摊成"逐帧刷新的帧内条带"，整个刷新周期
        //      （1 秒）里画面上**总有一条正在刷新的区域**。这个周期内一旦丢包
        //      破坏了参考帧，糊块就只能等刷新条带扫过才消失，滞留近一个周期，
        //      即使用户带宽充足、后续包一个不丢也照样糊。
        //   2) 它等于换掉了周期性真 IDR，而远控端的恢复语义是「缺片→丢整帧→
        //      请求 IDR→等服务端 IDR 来重新对齐解码器」。刷新条带不是重同步点，
        //      两套语义互相冲突：客户端等不到干净的 IDR，只能一直糊/冻结。
        // 现在改为：周期 IDR（gop_size = fps*keyframeIntervalSec，默认 2 秒，
        // 见 configure_context / resolve_gop_size）+ 按需 IDR（forced-idr=1 配合
        // submit() 里的 frame_->pict_type=I）。效果：最坏等 2 秒自然恢复；主动
        // 请求关键帧时下一帧（60fps 约 16ms）就能拿到 IDR。IDR 是整帧重编码，
        // 参考链完全重建，不会留下任何局部糊块。
    } else if (name.find("amf") != std::string::npos) {
        set_priv_str(ctx, "usage", "ultralowlatency");
        set_priv_str(ctx, "quality", "speed");
        set_priv_str(ctx, "rc", "cbr");
        set_priv_str(ctx, "bf", "0");
    } else if (name.find("qsv") != std::string::npos) {
        set_priv_str(ctx, "preset", "veryfast");
        set_priv_str(ctx, "async_depth", "1");      // 异步队列深度 1，别攒帧
        set_priv_str(ctx, "bf", "0");
    } else if (name.find("libx26") != std::string::npos) {
        set_priv_str(ctx, "preset", "veryfast");
        set_priv_str(ctx, "tune", "zerolatency");
        if (name.find("x265") != std::string::npos) {
            set_priv_str(ctx, "x265-params", "bframes=0:rc-lookahead=0");
        } else {
            set_priv_str(ctx, "x264-params",
                         "bframes=0:sync-lookahead=0:rc-lookahead=0");
        }
    }

    // ---- 各编码器公共：每个 IDR 前内联 SPS/PPS ----
    // 我们故意不设 AV_CODEC_FLAG_GLOBAL_HEADER（要 Annex-B 裸流），有些编码器就
    // 只在**第一个** IDR 前带 SPS/PPS。远控端中途接入、或丢包后请求 IDR 重新
    // 对齐解码器时，拿到的后续 IDR 没有参数集就解不出来（黑屏/花屏直到重连）。
    // repeat-headers 强制每个 IDR 都自带 SPS/PPS，IDR 可独立解码。
    // 只有部分编码器（NVENC 等）认这个键，老 FFmpeg/驱动不认时 av_opt_set 失败，
    // 直接忽略即可，不影响初始化。
    set_priv_str(ctx, "repeat-headers", "1");
}

// gop_size：显式给了就用；keyframeIntervalSec > 0 时按周期；== 0 表示"无限 GOP"，
// 只在被控端/远控端显式请求时出 IDR。
// 依据（UU远程/GameViewer 逆向报告）：一个 IDR 是 P 帧的 5~15 倍，周期 GOP 会造成
// 每秒一次的码率锯齿 → pacer/网络排队 → 规律性卡顿；实测其长稳会话 24 万帧仅 7 个 I 帧。
// 我们保留"按需 IDR"（远控端缺帧即请求，最小间隔 600ms），所以无限 GOP 是安全的。
int resolve_gop_size(const EncoderConfig& cfg) {
    if (cfg.gopSize > 0) return cfg.gopSize;
    const int fps = cfg.fps > 0 ? cfg.fps : defaults::kFps;
    if (cfg.keyframeIntervalSec <= 0) return fps * 600;   // ≈无限（10 分钟）兜底
    return fps * cfg.keyframeIntervalSec;
}

void configure_context(AVCodecContext* ctx, const EncoderConfig& cfg) {
    ctx->width        = cfg.width;
    ctx->height       = cfg.height;
    ctx->time_base    = AVRational{ 1, cfg.fps };     // 每帧一个 tick
    ctx->framerate    = AVRational{ cfg.fps, 1 };
    ctx->gop_size     = resolve_gop_size(cfg);
    ctx->max_b_frames = 0;                            // B 帧 = 额外编码延迟

    i64 br = i64(cfg.bitrateKbps) * 1000;
    if (br <= 0) br = i64(auto_bitrate_kbps(cfg.width, cfg.height, cfg.fps)) * 1000;
    ctx->bit_rate = br;
    // CBR：上限 = 目标。
    ctx->rc_max_rate = br;
    // VBV = target × 5 ÷ fps（60fps 时为 83ms）。
    // 依据：UU远程/GameViewer 逆向报告实测 1615/1615 次重配置零误差——这是低延迟
    // CBR 的关键。缓冲取 1 秒（原实现）会让编码器攒出"1 秒的突发额度"，
    // 一个复杂场景就能瞬间打满队列，表现为周期性顿一下；5 帧的缓冲既够平滑又不会积压。
    const int fpsForBuf = cfg.fps > 0 ? cfg.fps : defaults::kFps;
    int vbvFrames = cfg.vbvMultiplier > 0 ? cfg.vbvMultiplier : 5;
    if (vbvFrames > 60) vbvFrames = 60;
    i64 vbv = br * i64(vbvFrames) / i64(fpsForBuf);
    const i64 kVbvMin = 200000;                       // 至少 200kbit，免得极小码率崩
    if (vbv < kVbvMin) vbv = kVbvMin;
    const i64 kBufCap = i64(1) << 30;
    ctx->rc_buffer_size = static_cast<int>(vbv > kBufCap ? kBufCap : vbv);

    ctx->pix_fmt = AV_PIX_FMT_NV12;
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    // 单参考帧 + 单 slice（Moonlight 对 H.264 硬解端的硬要求）：
    // 参考帧 >1 会让解码端 DPB 变大、引入重排等待；多 slice 会让单片丢失的
    // 影响扩散（我们靠 FEC + 整帧重传恢复，不需要多 slice 抗丢包）。
    ctx->refs   = 1;
    ctx->slices = (cfg.slices > 0 && cfg.slices <= 8) ? cfg.slices : 1;
    // QP 上下限（Parsec 的 encoder_min_qp / encoder_max_qp 同名键）：
    //   max_qp 越小 -> 复杂场景宁可超码率也不糊（画质优先）
    //   min_qp 越大 -> 静态画面也不会用超高画质浪费码率（流畅优先）
    // 通过 AVOption 设置，编码器不支持（如部分硬编）时静默忽略。
    if (cfg.minQp >= 0 && cfg.minQp <= 51) {
        av_opt_set_int(ctx->priv_data, "qpmin", cfg.minQp, 0);
        av_opt_set_int(ctx->priv_data, "min_qp", cfg.minQp, 0);
    }
    if (cfg.maxQp >= 0 && cfg.maxQp <= 51) {
        av_opt_set_int(ctx->priv_data, "qpmax", cfg.maxQp, 0);
        av_opt_set_int(ctx->priv_data, "max_qp", cfg.maxQp, 0);
    }
    // 注意：故意不设 AV_CODEC_FLAG_GLOBAL_HEADER，
    //       要 Annex-B 且 SPS/PPS 内联在每个 IDR 前。
}

} // namespace

// -----------------------------------------------------------------------------
// VideoEncoder
// -----------------------------------------------------------------------------
VideoEncoder::VideoEncoder() = default;

VideoEncoder::~VideoEncoder() { shutdown(); }

std::string VideoEncoder::probe_best_encoder(proto::VideoCodec codec) {
    const std::vector<Candidate> cands = build_candidates("auto", codec);
    for (const Candidate& c : cands) {
        if (avcodec_find_encoder_by_name(c.name) != nullptr) return std::string(c.name);
    }
    return std::string();
}

bool VideoEncoder::init(const EncoderConfig& cfg, std::string& err) {
    shutdown();      // 支持重复 init
    err.clear();

    if (cfg.width <= 0 || cfg.height <= 0 || cfg.fps <= 0) {
        err = "编码参数非法：width/height/fps 必须为正数";
        MS_ERR("%s", err.c_str());
        return false;
    }

    cfg_ = cfg;
    cfg_.width  &= ~1;      // NV12 要求宽高为偶数
    cfg_.height &= ~1;
    if (cfg_.width <= 0 || cfg_.height <= 0) {
        err = "编码参数非法：宽高取偶后为 0";
        MS_ERR("%s", err.c_str());
        return false;
    }

    const std::vector<Candidate> cands = build_candidates(cfg_.preferred, cfg_.codec);
    std::string lastErr = "没有可尝试的编码器";

    for (const Candidate& c : cands) {
        const AVCodec* codec = avcodec_find_encoder_by_name(c.name);
        if (codec == nullptr) {
            lastErr = std::string(c.name) + ": 本机 FFmpeg 未提供该编码器";
            MS_WARN("编码器不可用: %s", c.name);
            continue;
        }

        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (ctx == nullptr) {
            lastErr = std::string(c.name) + ": avcodec_alloc_context3 失败（内存不足）";
            continue;
        }

        configure_context(ctx, cfg_);
        apply_low_latency_opts(ctx, c.name);   // 内部逐键忽略失败

        const int ret = avcodec_open2(ctx, codec, nullptr);
        if (ret < 0) {
            char eb[AV_ERROR_MAX_STRING_SIZE] = { 0 };
            av_strerror(ret, eb, sizeof(eb));
            lastErr = std::string(c.name) + ": " + eb;
            MS_WARN("编码器 %s 打开失败: %s，尝试下一个候选", c.name, eb);
            avcodec_free_context(&ctx);
            continue;
        }

        ctx_         = ctx;
        encoderName_ = c.name;
        hardware_    = c.hardware;
        break;
    }

    if (ctx_ == nullptr) {
        err = std::string("视频编码器初始化失败（codec=") + codec_tag(cfg_.codec) +
              ", preferred=" + cfg_.preferred + "）: " + lastErr;
        MS_ERR("%s", err.c_str());
        return false;
    }

    frame_ = av_frame_alloc();
    pkt_   = av_packet_alloc();
    if (frame_ == nullptr || pkt_ == nullptr) {
        err = "av_frame_alloc / av_packet_alloc 失败";
        MS_ERR("%s", err.c_str());
        shutdown();
        return false;
    }

    frame_->format = AV_PIX_FMT_NV12;
    frame_->width  = cfg_.width;
    frame_->height = cfg_.height;
    const int ret = av_frame_get_buffer(frame_, 0);
    if (ret < 0) {
        char eb[AV_ERROR_MAX_STRING_SIZE] = { 0 };
        av_strerror(ret, eb, sizeof(eb));
        err = std::string("av_frame_get_buffer 失败: ") + eb;
        MS_ERR("%s", err.c_str());
        shutdown();
        return false;
    }

    frameIndex_ = 0;
    MS_INFO("视频编码器就绪: %s (codec=%s, hardware=%d, %dx%d@%d, bitrate=%d kbps, gop=%d)",
            encoderName_.c_str(), codec_tag(cfg_.codec), hardware_ ? 1 : 0,
            cfg_.width, cfg_.height, cfg_.fps, cfg.bitrateKbps, ctx_->gop_size);
    return true;
}

void VideoEncoder::shutdown() {
    if (ctx_   != nullptr) avcodec_free_context(&ctx_);
    if (frame_ != nullptr) av_frame_free(&frame_);
    if (pkt_   != nullptr) av_packet_free(&pkt_);
    ctx_   = nullptr;
    frame_ = nullptr;
    pkt_   = nullptr;
    encoderName_.clear();
    hardware_   = false;
    frameIndex_ = 0;
}

int VideoEncoder::submit(const u8* y, int strideY, const u8* uv, int strideUV,
                         i64 ptsUs, bool forceKeyframe) {
    (void)ptsUs;   // 时基由 fps 决定；采集时间戳由上层随帧携带

    if (ctx_ == nullptr || frame_ == nullptr) return -1;

    const int w = cfg_.width;
    const int h = cfg_.height;
    if (y == nullptr || uv == nullptr || strideY < w || strideUV < w) {
        MS_ERR("submit 参数非法: y=%p uv=%p strideY=%d strideUV=%d 需要 >= %d",
               static_cast<const void*>(y), static_cast<const void*>(uv),
               strideY, strideUV, w);
        return -1;
    }

    frame_->format = AV_PIX_FMT_NV12;
    frame_->width  = w;
    frame_->height = h;

    // 编码器可能还持有上一帧的引用，这里必要时重新分配缓冲区
    int ret = av_frame_make_writable(frame_);
    if (ret < 0) {
        char eb[AV_ERROR_MAX_STRING_SIZE] = { 0 };
        av_strerror(ret, eb, sizeof(eb));
        MS_ERR("av_frame_make_writable 失败: %s", eb);
        return -1;
    }

    // 逐行拷贝：源 stride 可能大于 width（DXGI staging 行对齐），不能整块 memcpy
    u8* dstY  = frame_->data[0];
    u8* dstUV = frame_->data[1];
    const size_t dy = static_cast<size_t>(frame_->linesize[0]);
    const size_t du = static_cast<size_t>(frame_->linesize[1]);
    for (int r = 0; r < h; ++r) {
        std::memcpy(dstY + static_cast<size_t>(r) * dy,
                    y + static_cast<size_t>(r) * static_cast<size_t>(strideY),
                    static_cast<size_t>(w));
    }
    for (int r = 0; r < h / 2; ++r) {
        std::memcpy(dstUV + static_cast<size_t>(r) * du,
                    uv + static_cast<size_t>(r) * static_cast<size_t>(strideUV),
                    static_cast<size_t>(w));   // NV12 的 UV 行 = width 字节
    }

    frame_->pts       = frameIndex_++;   // 以 1/fps 为时基
    frame_->pict_type = forceKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;

    ret = avcodec_send_frame(ctx_, frame_);
    if (ret == AVERROR(EAGAIN)) return 0;        // 编码器缓冲满，调用方稍后重试同帧
    if (ret < 0) {
        char eb[AV_ERROR_MAX_STRING_SIZE] = { 0 };
        av_strerror(ret, eb, sizeof(eb));
        MS_ERR("avcodec_send_frame 失败: %s", eb);
        return -1;
    }
    return 1;
}

int VideoEncoder::poll(EncodedPacket& out, int timeoutMs) {
    out.data.clear();
    out.keyframe      = false;
    out.ptsUs         = 0;
    out.encodeStartUs = 0;
    out.encodeDoneUs  = 0;

    if (ctx_ == nullptr || pkt_ == nullptr) return -1;

    const i64 t0 = now_us();
    int ret = avcodec_receive_packet(ctx_, pkt_);
    if (ret == AVERROR(EAGAIN) && timeoutMs > 0) {
        // 编码器内部还在跑：给几毫秒的短暂等待机会，再试一次
        sleep_ms(timeoutMs > 2 ? 2 : timeoutMs);
        ret = avcodec_receive_packet(ctx_, pkt_);
    }
    if (ret == AVERROR(EAGAIN)) return 0;        // 暂无输出
    if (ret < 0) {
        char eb[AV_ERROR_MAX_STRING_SIZE] = { 0 };
        av_strerror(ret, eb, sizeof(eb));
        MS_ERR("avcodec_receive_packet 失败: %s", eb);
        return -1;
    }

    const i64 t1 = now_us();
    out.data.assign(pkt_->data, pkt_->data + pkt_->size);
    out.keyframe      = (pkt_->flags & AV_PKT_FLAG_KEY) != 0;
    out.encodeStartUs = t0;      // 本次收取开始（通常紧接 submit）
    out.encodeDoneUs  = t1;      // 拿到码流的时刻
    if (pkt_->pts != AV_NOPTS_VALUE && cfg_.fps > 0) {
        out.ptsUs = av_rescale_q(pkt_->pts, ctx_->time_base, AVRational{ 1, 1000000 });
    }
    av_packet_unref(pkt_);
    return 1;
}

bool VideoEncoder::set_bitrate(int kbps) {
    if (ctx_ == nullptr || kbps <= 0) return false;

    const i64 br = i64(kbps) * 1000;
    ctx_->bit_rate = br;
    if (ctx_->rc_max_rate > 0) ctx_->rc_max_rate = br;   // 保持 CBR 的「上限=目标」
    // VBV 跟着码率一起缩（target × 5 ÷ fps），规则见 configure_context 的注释
    const int fpsForBuf = cfg_.fps > 0 ? cfg_.fps : defaults::kFps;
    i64 vbv = br * 5 / i64(fpsForBuf);
    if (vbv < 200000) vbv = 200000;
    const i64 kBufCap = i64(1) << 30;
    ctx_->rc_buffer_size = static_cast<int>(vbv > kBufCap ? kBufCap : vbv);

    // 部分封装把码率放在私有选项里，两个键都试，失败忽略
    set_priv_int(ctx_, "b", br);
    set_priv_int(ctx_, "bitrate", br);

    MS_INFO("码率调整为 %d kbps (%s)", kbps, encoderName_.c_str());
    return true;
}

} // namespace ms::host
