// =============================================================================
//  audio_sender.cpp - WASAPI loopback 采集 + Opus 编码
// =============================================================================
#include "audio_sender.h"

#include "../common/clock.h"
#include "../common/log.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

// FFmpeg 头文件必须包在 extern "C" 里：这套头文件不自带 extern "C" 守卫，
// 直接包含会按 C++ 名字修饰去链接，全是 LNK2019（encoder.h 也这么做的）。
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <cstring>

namespace ms::host {

namespace {

constexpr int kTargetRate = int(proto::kAudioSampleRate);
constexpr int kTargetCh   = proto::kAudioChannels;
constexpr int kFrameSamples = int(proto::kAudioSamplesPerFrame);
constexpr int kInterleaved = kTargetCh;   // 交错样本数/帧

std::string hr_text(HRESULT hr) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)hr);
    return buf;
}

// WASAPI 的混音格式 -> FFmpeg sample_fmt（常见就 float32 / s16）
AVSampleFormat av_fmt_from_wave(const WAVEFORMATEX* f) {
    if (!f) return AV_SAMPLE_FMT_S16;
    if (f->wBitsPerSample == 32) return AV_SAMPLE_FMT_FLT;
    if (f->wBitsPerSample == 16) return AV_SAMPLE_FMT_S16;
    return AV_SAMPLE_FMT_FLT;
}

} // namespace

AudioSender::~AudioSender() { shutdown(); }

void AudioSender::close_device() {
    if (client_ && started_) {
        client_->Stop();
    }
    if (capture_) { capture_->Release(); capture_ = nullptr; }
    if (client_)  { client_->Release();  client_ = nullptr; }
    if (device_)  { device_->Release();  device_ = nullptr; }
    if (enumerator_) { enumerator_->Release(); enumerator_ = nullptr; }
    if (mixFmt_) { CoTaskMemFree(mixFmt_); mixFmt_ = nullptr; }
    started_ = false;
}

void AudioSender::close_codec() {
    if (frame_) { av_frame_free(&frame_); }
    if (pkt_)   { av_packet_free(&pkt_); }
    if (enc_)   { avcodec_free_context(&enc_); }
    if (swr_)   { swr_free(&swr_); }
    codec_ = nullptr;
    pending_.clear();
    conv_.clear();
}

void AudioSender::shutdown() {
    close_device();
    close_codec();
    if (comInit_) {
        ::CoUninitialize();
        comInit_ = false;
    }
    started_ = false;
}

bool AudioSender::init(int bitrateKbps, std::string& err) {
    shutdown();
    failed_ = false;
    lastErr_.clear();
    ts_ = 0;
    seq_ = 0;
    bytes_ = 0;

    const HRESULT co = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (co == RPC_E_CHANGED_MODE) {
        // 线程已经用别的模式初始化过 COM：可以继续用（我们只调 WASAPI）
    } else if (FAILED(co)) {
        err = "CoInitializeEx 失败 " + hr_text(co);
        lastErr_ = err;
        failed_ = true;
        return false;
    } else {
        comInit_ = true;
    }

    // ---- 默认播放设备（loopback 录的就是它的混音）----
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&enumerator_));
    if (FAILED(hr) || !enumerator_) {
        err = "创建音频设备枚举器失败 " + hr_text(hr);
        lastErr_ = err;
        failed_ = true;
        return false;
    }
    hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(hr) || !device_) {
        err = "没有可用的默认播放设备（被控端静音/无音频设备） " + hr_text(hr);
        lastErr_ = err;
        failed_ = true;
        return false;
    }
    // 设备名字打进日志，排障时能确认录的是哪块声卡
    std::string devName = "默认播放设备";
    {
        IPropertyStore* props = nullptr;
        if (SUCCEEDED(device_->OpenPropertyStore(STGM_READ, &props)) && props) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) &&
                v.vt == VT_LPWSTR && v.pwszVal) {
                const int n = ::WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, nullptr, 0,
                                                    nullptr, nullptr);
                if (n > 1) {
                    std::string s(size_t(n - 1), '\0');
                    ::WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, &s[0], n, nullptr, nullptr);
                    devName = s;
                }
            }
            PropVariantClear(&v);
            props->Release();
        }
    }

    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(&client_));
    if (FAILED(hr) || !client_) {
        err = "激活 IAudioClient 失败 " + hr_text(hr);
        lastErr_ = err;
        failed_ = true;
        return false;
    }
    hr = client_->GetMixFormat(&mixFmt_);
    if (FAILED(hr) || !mixFmt_) {
        err = "GetMixFormat 失败 " + hr_text(hr);
        lastErr_ = err;
        failed_ = true;
        return false;
    }

    // loopback 采集：共享模式 + 60ms 缓冲。缓冲越小延迟越低，60ms 对共享模式足够。
    const REFERENCE_TIME kBuf = 60 * 1000 * 10;   // 100ns 单位 -> 60ms
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_LOOPBACK,
                             kBuf, 0, mixFmt_, nullptr);
    if (FAILED(hr)) {
        // 有些设备不接受 60ms，退回 100ms 再试一次
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_LOOPBACK,
                                 100 * 1000 * 10, 0, mixFmt_, nullptr);
    }
    if (FAILED(hr)) {
        err = "IAudioClient::Initialize(loopback) 失败 " + hr_text(hr);
        lastErr_ = err;
        failed_ = true;
        return false;
    }
    hr = client_->GetService(__uuidof(IAudioCaptureClient),
                             reinterpret_cast<void**>(&capture_));
    if (FAILED(hr) || !capture_) {
        err = "GetService(IAudioCaptureClient) 失败 " + hr_text(hr);
        lastErr_ = err;
        failed_ = true;
        return false;
    }

    // ---- swr：混音格式 -> 48k / 立体声 / s16 ----
    AVChannelLayout inLayout{};
    AVChannelLayout outLayout{};
    av_channel_layout_default(&inLayout, int(mixFmt_->nChannels));
    av_channel_layout_default(&outLayout, kTargetCh);
    const AVSampleFormat inFmt = av_fmt_from_wave(mixFmt_);
    int ret = swr_alloc_set_opts2(&swr_, &outLayout, AV_SAMPLE_FMT_S16, kTargetRate,
                                  &inLayout, inFmt, int(mixFmt_->nSamplesPerSec),
                                  0, nullptr);
    if (ret < 0 || !swr_ || swr_init(swr_) < 0) {
        err = "音频重采样器初始化失败（swr）";
        lastErr_ = err;
        failed_ = true;
        return false;
    }

    // ---- Opus 编码器 ----
    codec_ = avcodec_find_encoder_by_name("libopus");
    if (!codec_) codec_ = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    if (!codec_) {
        err = "FFmpeg 里没有 Opus 编码器（libopus 未编进 avcodec）";
        lastErr_ = err;
        failed_ = true;
        return false;
    }
    enc_ = avcodec_alloc_context3(codec_);
    if (!enc_) {
        err = "avcodec_alloc_context3(opus) 失败";
        lastErr_ = err;
        failed_ = true;
        return false;
    }
    enc_->sample_rate = kTargetRate;
    enc_->ch_layout   = AV_CHANNEL_LAYOUT_STEREO;
    enc_->time_base   = AVRational{1, kTargetRate};
    enc_->bit_rate    = (bitrateKbps > 0 ? bitrateKbps : 96) * 1000;
    // libopus 支持 s16 交错输入；直接用，省一次格式转换。
    // （新版 FFmpeg 去掉了 AVCodec::sample_fmts，这里不探测，固定 s16。）
    enc_->sample_fmt  = AV_SAMPLE_FMT_S16;
    // 低延迟：关掉前瞻/复杂模式（Opus 默认就是低延迟，这里只把 VBR 打开省带宽）
    enc_->frame_size = kFrameSamples;
    if ((ret = avcodec_open2(enc_, codec_, nullptr)) < 0) {
        err = "opus 编码器打开失败（" + std::to_string(ret) + "）";
        lastErr_ = err;
        failed_ = true;
        return false;
    }
    frame_ = av_frame_alloc();
    pkt_   = av_packet_alloc();
    if (!frame_ || !pkt_) {
        err = "音频 frame/packet 分配失败";
        lastErr_ = err;
        failed_ = true;
        return false;
    }
    frame_->format      = enc_->sample_fmt;
    frame_->sample_rate = enc_->sample_rate;
    frame_->ch_layout   = enc_->ch_layout;
    frame_->nb_samples  = kFrameSamples;
    if (av_frame_get_buffer(frame_, 0) < 0) {
        err = "音频 frame 缓冲分配失败";
        lastErr_ = err;
        failed_ = true;
        return false;
    }

    hr = client_->Start();
    if (FAILED(hr)) {
        err = "IAudioClient::Start 失败 " + hr_text(hr);
        lastErr_ = err;
        failed_ = true;
        return false;
    }
    started_ = true;
    pending_.reserve(size_t(kFrameSamples) * kInterleaved * 4);

    MS_INFO("音频就绪: loopback %s | 混音 %dHz %dch %dbits -> Opus %dkbps 48kHz 立体声 20ms",
            devName.c_str(), int(mixFmt_->nSamplesPerSec), int(mixFmt_->nChannels),
            int(mixFmt_->wBitsPerSample), (bitrateKbps > 0 ? bitrateKbps : 96));
    return true;
}

// -----------------------------------------------------------------------------
// 把一段 loopback 数据转成 48k/2ch/s16 交错，追加到 pending_
// -----------------------------------------------------------------------------
void AudioSender::convert_and_append(const u8* data, u32 frames, u32 flags) {
    if (!swr_ || frames == 0) return;

    // 静音段（游戏没声音时 WASAPI 会给 SILENT 标记）：直接补等长零样本，
    // 不走 swr（swr_convert 传 0 输入样例不会产出任何东西）。
    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
        const int outCount = int(av_rescale_rnd(frames, kTargetRate,
                                                u32(mixFmt_->nSamplesPerSec), AV_ROUND_UP));
        if (outCount <= 0) return;
        const size_t prev = pending_.size();
        pending_.resize(prev + size_t(outCount) * kTargetCh, 0);
        return;
    }

    const int outMax = int(av_rescale_rnd(
        swr_get_delay(swr_, int(mixFmt_->nSamplesPerSec)) + frames,
        kTargetRate, int(mixFmt_->nSamplesPerSec), AV_ROUND_UP));
    if (outMax <= 0) return;
    conv_.resize(size_t(outMax) * kTargetCh * sizeof(int16_t));

    u8* outPlanes[1] = { conv_.data() };
    const u8* inPlanes[1] = { data };
    const int got = swr_convert(swr_, outPlanes, outMax, inPlanes, int(frames));
    if (got <= 0) return;

    const size_t prev = pending_.size();
    pending_.resize(prev + size_t(got) * kTargetCh);
    std::memcpy(pending_.data() + prev, conv_.data(),
                size_t(got) * kTargetCh * sizeof(int16_t));
}

// -----------------------------------------------------------------------------
// 从 loopback 读原始数据
// -----------------------------------------------------------------------------
bool AudioSender::read_loopback(std::string& err) {
    if (!capture_ || !started_) return true;

    for (int guard = 0; guard < 32; ++guard) {
        UINT32 packetFrames = 0;
        const HRESULT hr = capture_->GetNextPacketSize(&packetFrames);
        if (FAILED(hr)) {
            err = "GetNextPacketSize 失败 " + hr_text(hr);
            return false;
        }
        if (packetFrames == 0) return true;   // 暂时没新数据：正常

        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        const HRESULT hr2 = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (hr2 == AUDCLNT_S_BUFFER_EMPTY) return true;
        if (FAILED(hr2)) {
            err = "GetBuffer 失败 " + hr_text(hr2);
            return false;
        }
        convert_and_append(data, frames, flags);
        capture_->ReleaseBuffer(frames);
    }
    return true;
}

// -----------------------------------------------------------------------------
// 凑满一帧就编码
// -----------------------------------------------------------------------------
bool AudioSender::encode_ready(std::vector<Packet>& out) {
    if (!enc_ || !frame_ || !pkt_) return true;
    const size_t need = size_t(kFrameSamples) * kInterleaved;

    for (int guard = 0; guard < 8 && pending_.size() >= need; ++guard) {
        if (pending_.size() < need) break;
        // 编码器可能要求可写帧；data[0] 是 s16 交错
        if (av_frame_make_writable(frame_) < 0) return false;
        std::memcpy(frame_->data[0], pending_.data(), need * sizeof(int16_t));
        frame_->pts = int64_t(ts_);
        ts_ += kFrameSamples;

        int ret = avcodec_send_frame(enc_, frame_);
        if (ret == AVERROR(EAGAIN)) {
            // 先把编码器里已有的包收回来
            while (avcodec_receive_packet(enc_, pkt_) == 0) {
                AudioSender::Packet p;
                p.seq = ++seq_;
                p.ts  = u32(pkt_->pts >= 0 ? pkt_->pts : int64_t(ts_ - kFrameSamples));
                p.data.assign(pkt_->data, pkt_->data + pkt_->size);
                bytes_ += pkt_->size;
                out.push_back(std::move(p));
                av_packet_unref(pkt_);
            }
            ret = avcodec_send_frame(enc_, frame_);
        }
        if (ret < 0) return false;

        while (avcodec_receive_packet(enc_, pkt_) == 0) {
            AudioSender::Packet p;
            p.seq = ++seq_;
            p.ts  = u32(pkt_->pts >= 0 ? pkt_->pts : int64_t(ts_ - kFrameSamples));
            p.data.assign(pkt_->data, pkt_->data + pkt_->size);
            bytes_ += pkt_->size;
            out.push_back(std::move(p));
            av_packet_unref(pkt_);
        }

        pending_.erase(pending_.begin(), pending_.begin() + ptrdiff_t(need));
    }
    return true;
}

void AudioSender::poll(std::vector<Packet>& out) {
    if (!started_ || failed_) return;

    std::string err;
    if (!read_loopback(err)) {
        MS_WARN("音频采集中断: %s（本会话静音继续）", err.c_str());
        lastErr_ = err;
        failed_ = true;
        close_device();
        return;
    }
    if (!encode_ready(out)) {
        MS_WARN("音频编码失败（本会话静音继续）");
        lastErr_ = "encode failed";
        failed_ = true;
        close_codec();
    }
}

} // namespace ms::host
