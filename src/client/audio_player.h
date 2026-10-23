// =============================================================================
//  audio_player.h - 远控端声音播放（Opus 解码 + WASAPI 输出）
//
//  为什么不用 voice engine / 第三方库：自用工具，依赖越少越好，WASAPI 直接播
//  延迟最低（共享模式下 ~20-30ms），和视频一样"能立刻听到"。
//
//  数据流：
//    UDP/TCP 收包 -> push()（任意线程，加锁入队，按 seq 去重）
//    tick()（主线程，约 8ms 一次）-> 解码 -> 抖动缓冲（目标 40ms）-> WASAPI 写
//
//  抖动缓冲：网线抖动/视频突发会让音频包到得不均匀。缓冲 2 帧（40ms）再开始播，
//  攒多了（>120ms）丢最旧的，保证延迟不随时间越拖越大。
// =============================================================================
#pragma once

#include <windows.h>
#include <mmreg.h>

#include "../common/types.h"
#include "../common/protocol.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

struct IAudioClient;
struct IAudioRenderClient;
struct IMMDevice;
struct IMMDeviceEnumerator;
struct AVCodecContext;
struct AVCodec;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace ms::client {

class AudioPlayer {
public:
    AudioPlayer() = default;
    ~AudioPlayer();

    bool init(std::string& err);
    void shutdown();

    // 任意线程可调：把收到的音频包放进解码队列（按 seq 去重/丢过期）
    void push(const proto::AudioPacket& pkt);

    // 主线程定时调用（内部约 8ms 节流）：解码 + 写设备
    void tick();

    void set_muted(bool on);
    bool muted() const { return muted_.load(std::memory_order_relaxed); }

    bool active() const { return started_; }
    bool failed() const { return failed_; }
    const std::string& error() const { return lastErr_; }

private:
    struct Queued {
        u32 seq = 0;
        i64 arrivedUs = 0;
        proto::AudioPacket pkt;
    };

    void decode_ready();
    void feed_device();
    void close_device();
    void close_codec();
    bool open_device(std::string& err);
    bool open_codec(std::string& err);

    // ---- WASAPI ----
    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice*           device_     = nullptr;
    IAudioClient*        client_     = nullptr;
    IAudioRenderClient*  render_     = nullptr;
    WAVEFORMATEX*        mixFmt_     = nullptr;
    UINT32               bufferFrames_ = 0;
    UINT32               frameBytes_   = 0;   // 每个设备帧的字节数（所有声道合计）
    bool  comInit_ = false;
    bool  started_ = false;
    bool  failed_  = false;
    std::string lastErr_;

    // ---- 解码 ----
    const AVCodec*  codec_ = nullptr;
    AVCodecContext* dec_   = nullptr;
    AVFrame*        frame_ = nullptr;
    AVPacket*       pkt_   = nullptr;
    SwrContext*     swr_   = nullptr;        // FLT(48k/2ch) -> 设备混音格式

    // ---- 队列 / 抖动缓冲 ----
    static constexpr size_t kMaxQueue = 48;
    std::mutex qMtx_;
    std::deque<Queued> queue_;
    u32  lastRecvSeq_ = 0;
    bool haveLastRecv_ = false;

    std::vector<u8> pcm_;            // 已解码/已转换的 PCM 字节（按设备格式）
    size_t pcmRead_ = 0;             // 已消费偏移
    u32  expectSeq_ = 0;
    bool haveExpect_ = false;
    i64  gapSinceUs_ = 0;            // 发现序号空洞的时刻

    std::atomic<bool> muted_{false};
    i64 lastTickUs_ = 0;
    bool starving_ = false;              // 当前是否处于"没数据在补静音"状态（欠载计数去抖）

    // 统计
    u64 recvPackets_ = 0;
    u64 decodedFrames_ = 0;
    u64 underruns_ = 0;
    u64 lastLogUs_ = 0;
    i64 lastDropLogUs_ = 0;              // 缓冲超限日志限流（一秒最多一条）
    u64 lastLogPackets_ = 0;
    u64 lastLogDecoded_ = 0;
    double rmsSum_ = 0.0;
    u64 rmsCount_ = 0;
};

} // namespace ms::client
