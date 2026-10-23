// =============================================================================
//  audio_sender.h - 被控端声音采集 + Opus 编码
//
//  目的：把被控端**系统播放的声音**（游戏/音乐）传给远控端。
//  做法：
//    * WASAPI loopback（把"正在播放"的混音流录下来，不需要虚拟声卡）
//    * swr 统一成 48kHz / 立体声 / s16
//    * FFmpeg libopus 编码，20ms 一帧（960 采样），默认 96kbps
//    * 调用方（串流线程）每轮 poll() 取走已编码的包，自己决定走 UDP 还是 TCP
//
//  失败绝不拖垮视频：初始化失败/设备热拔只会让 failed() 变 true，
//  串流线程继续跑，只是没有声音。
// =============================================================================
#pragma once

#include <windows.h>
#include <mmreg.h>

#include "../common/types.h"
#include "../common/protocol.h"

#include <string>
#include <vector>

struct IAudioClient;
struct IAudioCaptureClient;
struct IMMDevice;
struct IMMDeviceEnumerator;
struct AVCodecContext;
struct AVCodec;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace ms::host {

class AudioSender {
public:
    // 一个已编码好的音频包（20ms 一帧 Opus）
    struct Packet {
        u32   seq = 0;      // 单调递增（远控端按它丢重复/乱序）
        u32   ts = 0;       // 48kHz 采样计数
        Bytes data;         // 裸 Opus 包
    };

    AudioSender() = default;
    ~AudioSender();

    // 打开默认播放设备的 loopback 并准备好编码器；失败返回 false（不致命）
    bool init(int bitrateKbps, std::string& err);
    void shutdown();

    // 采集 + 编码：把当前已就绪的包追加到 out（可能 0~2 个）。
    // 正常每 20ms 调一次即可；串流线程每轮都调也没问题（内部有数据才会编）。
    void poll(std::vector<Packet>& out);

    bool active() const { return started_; }
    bool failed() const { return failed_; }
    const std::string& error() const { return lastErr_; }

private:
    bool read_loopback(std::string& err);              // 读原始混音数据
    void convert_and_append(const u8* data, u32 frames, u32 flags);
    bool encode_ready(std::vector<Packet>& out);       // 凑满 20ms 就编码
    void close_device();
    void close_codec();

    // ---- WASAPI ----
    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice*           device_     = nullptr;
    IAudioClient*        client_     = nullptr;
    IAudioCaptureClient* capture_    = nullptr;
    WAVEFORMATEX*        mixFmt_     = nullptr;
    bool  comInit_  = false;
    bool  started_  = false;
    bool  failed_   = false;
    std::string lastErr_;

    // ---- 重采样（任意混音格式 -> 48k/2ch/s16）----
    SwrContext* swr_ = nullptr;
    std::vector<u8> conv_;                 // 转换临时缓冲
    std::vector<int16_t> pending_;         // 交错立体声 s16，凑满 960 帧就编

    // ---- Opus 编码 ----
    const AVCodec*    codec_  = nullptr;
    AVCodecContext*   enc_    = nullptr;
    AVFrame*          frame_  = nullptr;
    AVPacket*         pkt_    = nullptr;
    u32  ts_     = 0;      // 已提交的采样计数
    u32  seq_    = 0;
    u64  bytes_  = 0;      // 已编码字节数（日志用）
};

} // namespace ms::host
