// =============================================================================
//  audio_player.cpp - Opus 解码 + WASAPI 播放
// =============================================================================
#include "audio_player.h"

#include "../common/clock.h"
#include "../common/log.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

// 同 audio_sender.cpp：FFmpeg 头文件不自带 extern "C"，必须自己包。
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ms::client {

namespace {

constexpr int kSrcRate   = int(proto::kAudioSampleRate);
constexpr int kSrcCh     = proto::kAudioChannels;
constexpr int kFrameMs   = proto::kAudioFrameMs;

// 抖动缓冲目标：攒够 40ms 音频才开始播（吸收网络抖动）
constexpr int kTargetMs = 40;
// 缓冲上限：超过 160ms 就丢最旧的，避免延迟越拖越大
constexpr int kMaxBufferedMs = 160;
// 每次至少凑够 10ms 才写一次设备（减少 COM 调用；192kHz 设备上也成立）
constexpr int kFeedChunkMs = 10;
// 音视频不同步不会影响听感；这里只保证"最近 120ms 内". 
constexpr int64_t kTickMinUs    = 8000;

std::string hr_text(HRESULT hr) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)hr);
    return buf;
}

AVSampleFormat av_fmt_from_wave(const WAVEFORMATEX* f) {
    if (!f) return AV_SAMPLE_FMT_S16;
    if (f->wBitsPerSample == 32) return AV_SAMPLE_FMT_FLT;
    if (f->wBitsPerSample == 16) return AV_SAMPLE_FMT_S16;
    return AV_SAMPLE_FMT_FLT;
}

} // namespace

AudioPlayer::~AudioPlayer() { shutdown(); }

// -----------------------------------------------------------------------------
// 设备
// -----------------------------------------------------------------------------
bool AudioPlayer::open_device(std::string& err) {
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&enumerator_));
    if (FAILED(hr) || !enumerator_) {
        err = "创建音频设备枚举器失败 " + hr_text(hr);
        return false;
    }
    hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(hr) || !device_) {
        err = "没有可用的默认播放设备 " + hr_text(hr);
        return false;
    }
    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(&client_));
    if (FAILED(hr) || !client_) {
        err = "激活 IAudioClient 失败 " + hr_text(hr);
        return false;
    }
    hr = client_->GetMixFormat(&mixFmt_);
    if (FAILED(hr) || !mixFmt_) {
        err = "GetMixFormat 失败 " + hr_text(hr);
        return false;
    }
    // 100ms 缓冲：共享模式够小（延迟主要压在抖动缓冲那 40ms 上）
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                             100 * 1000 * 10, 0, mixFmt_, nullptr);
    if (FAILED(hr)) {
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                 200 * 1000 * 10, 0, mixFmt_, nullptr);
    }
    if (FAILED(hr)) {
        err = "IAudioClient::Initialize 失败 " + hr_text(hr);
        return false;
    }
    hr = client_->GetBufferSize(&bufferFrames_);
    if (FAILED(hr) || bufferFrames_ == 0) {
        err = "GetBufferSize 失败 " + hr_text(hr);
        return false;
    }
    hr = client_->GetService(__uuidof(IAudioRenderClient),
                             reinterpret_cast<void**>(&render_));
    if (FAILED(hr) || !render_) {
        err = "GetService(IAudioRenderClient) 失败 " + hr_text(hr);
        return false;
    }
    frameBytes_ = mixFmt_->nBlockAlign;
    if (frameBytes_ == 0) {
        frameBytes_ = UINT32(std::max<int>(mixFmt_->nChannels, 1) * mixFmt_->wBitsPerSample / 8);
    }
    if (frameBytes_ == 0) {
        err = "播放设备帧大小异常";
        return false;
    }
    return true;
}

void AudioPlayer::close_device() {
    if (client_ && started_) client_->Stop();
    if (render_) { render_->Release(); render_ = nullptr; }
    if (client_) { client_->Release(); client_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
    if (enumerator_) { enumerator_->Release(); enumerator_ = nullptr; }
    if (mixFmt_) { CoTaskMemFree(mixFmt_); mixFmt_ = nullptr; }
    started_ = false;
}

void AudioPlayer::close_codec() {
    if (frame_) av_frame_free(&frame_);
    if (pkt_)   av_packet_free(&pkt_);
    if (dec_)   avcodec_free_context(&dec_);
    if (swr_)   swr_free(&swr_);
    codec_ = nullptr;
}

void AudioPlayer::shutdown() {
    close_device();
    close_codec();
    if (comInit_) { ::CoUninitialize(); comInit_ = false; }
    {
        std::lock_guard<std::mutex> lk(qMtx_);
        queue_.clear();
    }
    pcm_.clear();
    pcmRead_ = 0;
    started_ = false;
}

// -----------------------------------------------------------------------------
// 解码器
// -----------------------------------------------------------------------------
bool AudioPlayer::open_codec(std::string& err) {
    codec_ = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!codec_) {
        err = "FFmpeg 里没有 Opus 解码器";
        return false;
    }
    dec_ = avcodec_alloc_context3(codec_);
    if (!dec_) {
        err = "avcodec_alloc_context3(opus 解码) 失败";
        return false;
    }
    dec_->sample_rate = kSrcRate;
    dec_->ch_layout   = AV_CHANNEL_LAYOUT_STEREO;
    const int ret = avcodec_open2(dec_, codec_, nullptr);
    if (ret < 0) {
        err = "Opus 解码器打开失败（" + std::to_string(ret) + "）";
        return false;
    }
    frame_ = av_frame_alloc();
    pkt_   = av_packet_alloc();
    if (!frame_ || !pkt_) {
        err = "音频解码 frame/packet 分配失败";
        return false;
    }

    AVChannelLayout inLayout{};
    AVChannelLayout outLayout{};
    av_channel_layout_default(&inLayout, kSrcCh);
    av_channel_layout_default(&outLayout, int(mixFmt_->nChannels));
    const int r2 = swr_alloc_set_opts2(&swr_, &outLayout, av_fmt_from_wave(mixFmt_),
                                       int(mixFmt_->nSamplesPerSec),
                                       &inLayout, AV_SAMPLE_FMT_FLT, kSrcRate,
                                       0, nullptr);
    if (r2 < 0 || !swr_ || swr_init(swr_) < 0) {
        err = "音频重采样器初始化失败（播放侧 swr）";
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// init / 启停
// -----------------------------------------------------------------------------
bool AudioPlayer::init(std::string& err) {
    shutdown();
    failed_ = false;
    lastErr_.clear();
    lastTickUs_ = 0;
    lastLogUs_ = 0;

    const HRESULT co = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (co == RPC_E_CHANGED_MODE) {
        // 已经初始化过 COM（别的模式）：WASAPI 照样能用
    } else if (FAILED(co)) {
        err = "CoInitializeEx 失败 " + hr_text(co);
        lastErr_ = err;
        failed_ = true;
        return false;
    } else {
        comInit_ = true;
    }

    if (!open_device(err)) { lastErr_ = err; failed_ = true; return false; }
    if (!open_codec(err))  { lastErr_ = err; failed_ = true; return false; }

    const HRESULT hr = client_->Start();
    if (FAILED(hr)) {
        err = "IAudioClient::Start 失败 " + hr_text(hr);
        lastErr_ = err;
        failed_ = true;
        return false;
    }
    started_ = true;
    MS_INFO("音频播放就绪: 设备 %dHz %dch %dbits | Opus 48kHz 立体声 20ms | 抖动缓冲 %dms",
            int(mixFmt_->nSamplesPerSec), int(mixFmt_->nChannels),
            int(mixFmt_->wBitsPerSample), kTargetMs);
    return true;
}

void AudioPlayer::set_muted(bool on) {
    muted_.store(on, std::memory_order_relaxed);
    if (on) {
        // 静音时把积压的音频丢掉，取消静音后立刻从"当前"开始，不会有延迟
        std::lock_guard<std::mutex> lk(qMtx_);
        queue_.clear();
        pcm_.clear();
        pcmRead_ = 0;
    }
    MS_INFO("声音 -> %s", on ? "静音" : "开启");
}

// -----------------------------------------------------------------------------
// 收包（任意线程）
// -----------------------------------------------------------------------------
void AudioPlayer::push(const proto::AudioPacket& pkt) {
    if (!started_ || failed_ || pkt.data.empty()) return;
    if (muted_.load(std::memory_order_relaxed)) return;
    {
        std::lock_guard<std::mutex> lk(qMtx_);
        // UDP 每包发两份 / TCP 兜底也来一份：按 seq 去重
        if (haveLastRecv_ && i32(u32(pkt.seq - lastRecvSeq_)) <= 0) return;
        lastRecvSeq_ = pkt.seq;
        haveLastRecv_ = true;
        Queued q;
        q.seq = pkt.seq;
        q.arrivedUs = now_us();
        q.pkt = pkt;
        if (queue_.size() >= kMaxQueue) queue_.pop_front();
        queue_.push_back(std::move(q));
        ++recvPackets_;
    }
}

// -----------------------------------------------------------------------------
// 解码 + 出队到 pcm_（设备格式）
// -----------------------------------------------------------------------------
void AudioPlayer::decode_ready() {
    if (!dec_ || !swr_) return;
    for (int guard = 0; guard < 8; ++guard) {
        Queued q;
        {
            std::lock_guard<std::mutex> lk(qMtx_);
            if (queue_.empty()) break;
            Queued& f = queue_.front();
            const i64 now = now_us();
            if (!haveExpect_) {
                expectSeq_ = f.seq;
                haveExpect_ = true;
                gapSinceUs_ = 0;
            }
            const i32 diff = i32(u32(f.seq - expectSeq_));
            if (diff < 0) {              // 过期包：丢
                queue_.pop_front();
                continue;
            }
            if (diff > 0) {              // 序号空洞：等一会儿看能不能补到
                if (gapSinceUs_ == 0) gapSinceUs_ = now;
                if (now - gapSinceUs_ < 80000) break;
                expectSeq_ = f.seq;      // 等不到就跳过（Opus 丢一包最多"咔"一下）
                gapSinceUs_ = 0;
            }
            q = std::move(queue_.front());
            queue_.pop_front();
        }
        if (q.seq == expectSeq_) expectSeq_ = q.seq + 1;
        gapSinceUs_ = 0;

        av_packet_unref(pkt_);
        if (av_new_packet(pkt_, int(q.pkt.data.size())) < 0) continue;
        std::memcpy(pkt_->data, q.pkt.data.data(), q.pkt.data.size());
        if (avcodec_send_packet(dec_, pkt_) < 0) continue;

        while (avcodec_receive_frame(dec_, frame_) == 0) {
            // 统计 RMS（验证"到底有没有声音"，不依赖耳朵）
            if (frame_->format == AV_SAMPLE_FMT_FLT && frame_->data[0]) {
                const float* s = reinterpret_cast<const float*>(frame_->data[0]);
                const size_t n = size_t(frame_->nb_samples) * kSrcCh;
                for (size_t i = 0; i < n; ++i) {
                    rmsSum_ += double(s[i]) * double(s[i]);
                }
                rmsCount_ += n;
            } else if (frame_->format == AV_SAMPLE_FMT_FLTP) {
                for (int ch = 0; ch < kSrcCh; ++ch) {
                    const float* s = reinterpret_cast<const float*>(frame_->data[ch]);
                    if (!s) continue;
                    for (int i = 0; i < frame_->nb_samples; ++i) {
                        rmsSum_ += double(s[i]) * double(s[i]);
                    }
                    rmsCount_ += size_t(frame_->nb_samples);
                }
            }

            if (!muted_.load(std::memory_order_relaxed)) {
                const int outMax = int(av_rescale_rnd(
                    swr_get_delay(swr_, kSrcRate) + frame_->nb_samples,
                    int(mixFmt_->nSamplesPerSec), kSrcRate, AV_ROUND_UP));
                if (outMax <= 0) { av_frame_unref(frame_); ++decodedFrames_; continue; }
                if (outMax > 0) {
                    const size_t add = size_t(outMax) * frameBytes_;
                    const size_t old = pcm_.size();
                    pcm_.resize(old + add);
                    u8* out[1] = { pcm_.data() + old };
                    const int got = swr_convert(swr_, out, outMax,
                                                const_cast<const uint8_t**>(frame_->extended_data),
                                                frame_->nb_samples);
                    pcm_.resize(old + size_t(got > 0 ? got : 0) * frameBytes_);
                }
            }
            av_frame_unref(frame_);
            ++decodedFrames_;
        }
    }

    // 缓冲太长就丢最旧的，限制延迟。注意这里的"帧"是**设备帧**（192kHz 设备上
    // 1ms 就是 192 帧），必须按时间算，不能按帧数写死。
    const UINT32 devRate = (mixFmt_ && mixFmt_->nSamplesPerSec > 0)
                               ? mixFmt_->nSamplesPerSec : proto::kAudioSampleRate;
    const size_t maxFrames = size_t(devRate) * kMaxBufferedMs / 1000;
    const size_t frames = (pcm_.size() - pcmRead_) / frameBytes_;
    if (frames > maxFrames) {
        const size_t dropFrames = frames - maxFrames;
        pcmRead_ += dropFrames * frameBytes_;
        const i64 now2 = now_us();
        if (now2 - lastDropLogUs_ > 1000000) {   // 限流：一秒最多一条
            lastDropLogUs_ = now2;
            MS_DEBUG("音频缓冲超限（%dms），丢弃最旧 %dms 以压住延迟",
                     int(frames * 1000 / devRate), int(dropFrames * 1000 / devRate));
        }
    }
}

// -----------------------------------------------------------------------------
// 写设备
// -----------------------------------------------------------------------------
void AudioPlayer::feed_device() {
    if (!client_ || !render_ || frameBytes_ == 0) return;

    // 攒够目标时长再开始播（抖动缓冲）；否则先补静音占位，避免设备欠载爆音
    const UINT32 devRate = (mixFmt_ && mixFmt_->nSamplesPerSec > 0)
                               ? mixFmt_->nSamplesPerSec : proto::kAudioSampleRate;
    const UINT32 minSpace = std::max<UINT32>(devRate / (1000 / kFeedChunkMs), 32);
    const UINT32 targetFrames = devRate * kTargetMs / 1000;

    // 延迟上限：缓冲超过 90ms 就裁剪回 40ms。
    // 两端声卡时钟不可能完全一致（尤其远控端设备率是 192kHz 时），不裁剪的话
    // 缓冲会一点点涨上去，延迟越拖越大；裁剪的代价是偶尔丢 20ms（听感上几乎无感）。
    {
        const size_t maxFrames = size_t(devRate) * 90 / 1000;
        size_t frames = (pcm_.size() - pcmRead_) / frameBytes_;
        if (frames > maxFrames) {
            const size_t dropFrames = frames - targetFrames;
            pcmRead_ += dropFrames * frameBytes_;
            frames = targetFrames;
            const i64 nowTrim = now_us();
            if (nowTrim - lastDropLogUs_ > 1000000) {
                lastDropLogUs_ = nowTrim;
                MS_DEBUG("音频延迟超限，裁剪缓冲到 %dms", int(targetFrames * 1000 / devRate));
            }
        }
    }

    size_t availFrames = (pcm_.size() - pcmRead_) / frameBytes_;
    const bool enough = availFrames >= targetFrames;

    for (int guard = 0; guard < 8; ++guard) {
        UINT32 pad = 0;
        if (FAILED(client_->GetCurrentPadding(&pad))) return;
        if (pad >= bufferFrames_) return;
        const UINT32 space = bufferFrames_ - pad;
        if (space < minSpace) return;         // 不够一个 10ms 小块就不折腾

        if (availFrames == 0) {
            if (enough) return;
            // 没数据：补静音（保持设备时钟连续，避免开/关设备的爆音）
            BYTE* dst = nullptr;
            if (FAILED(render_->GetBuffer(space, &dst)) || !dst) return;
            std::memset(dst, 0, size_t(space) * frameBytes_);
            render_->ReleaseBuffer(space, 0);
            if (!starving_) { starving_ = true; ++underruns_; }
            return;
        }
        if (!enough && pad > bufferFrames_ / 2) return;   // 还没攒够，等等

        starving_ = false;
        const UINT32 n = UINT32(std::min<size_t>(space, availFrames));
        BYTE* dst = nullptr;
        if (FAILED(render_->GetBuffer(n, &dst)) || !dst) return;
        std::memcpy(dst, pcm_.data() + pcmRead_, size_t(n) * frameBytes_);
        render_->ReleaseBuffer(n, 0);
        pcmRead_ += size_t(n) * frameBytes_;
        availFrames -= n;
        if (n < minSpace) return;
    }
}

// -----------------------------------------------------------------------------
// tick：主线程定期调用
// -----------------------------------------------------------------------------
void AudioPlayer::tick() {
    if (!started_ || failed_) return;
    const i64 now = now_us();
    if (now - lastTickUs_ < kTickMinUs) return;
    lastTickUs_ = now;

    decode_ready();
    feed_device();

    // 已消费的 PCM 回收（避免 vector 无限增长）
    if (pcmRead_ > 0 && (pcm_.size() - pcmRead_) < pcmRead_ / 2) {
        pcm_.erase(pcm_.begin(), pcm_.begin() + ptrdiff_t(pcmRead_));
        pcmRead_ = 0;
    }

    // 每 5 秒报告一次：收包/解码/缓冲/RMS（RMS 非 0 = 确实有声音在传）
    if (now - lastLogUs_ > 5000000) {
        if (lastLogUs_ != 0) {
            const size_t buffered = (pcm_.size() - pcmRead_) / frameBytes_;
            const double rms = (rmsCount_ > 0)
                                   ? std::sqrt(rmsSum_ / double(rmsCount_)) : 0.0;
            const u64 pkts = recvPackets_ - lastLogPackets_;
            const u64 dec  = decodedFrames_ - lastLogDecoded_;
            MS_DEBUG("音频: 收包 %llu 解码 %llu 缓冲 %dms 欠载 %llu RMS %.3f",
                     (unsigned long long)pkts, (unsigned long long)dec,
                     int(buffered * 1000 / ((mixFmt_ && mixFmt_->nSamplesPerSec > 0) ? mixFmt_->nSamplesPerSec : proto::kAudioSampleRate)), (unsigned long long)underruns_,
                     rms);
        }
        lastLogUs_ = now;
        lastLogPackets_ = recvPackets_;
        lastLogDecoded_ = decodedFrames_;
        rmsSum_ = 0.0;
        rmsCount_ = 0;
    }
}

} // namespace ms::client
