// =============================================================================
//  video_probe.cpp - host-side video pipeline probe (console, no network, no UI)
//
//  Runs exactly the first stage of the host streaming thread:
//      DXGI capture -> BGRA->NV12 -> encode -> stats
//  so that "is there a picture / does the encoder produce a stream / what are
//  the fps-bitrate-latency numbers" can be verified on their own, before any
//  network or UI is involved.
//
//  Usage:
//     ministream-videoprobe [seconds] [monitor] [fps] [encoder]
//     e.g.  ministream-videoprobe 5 0 60 auto
//
//  NOTE: keep this file ASCII-only. Windows PowerShell 5.1 decodes files
//        without a BOM using the system ANSI codepage, so non-ASCII text here
//        gets corrupted the moment any PowerShell command rewrites the file.
// =============================================================================
#include "../src/host/capture.h"
#include "../src/host/encoder.h"

#include "../src/common/clock.h"
#include "../src/common/log.h"
#include "../src/common/stats.h"
#include "../src/common/types.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace ms;
using namespace ms::host;

int main(int argc, char** argv) {
    const int seconds   = argc > 1 ? std::atoi(argv[1]) : 5;
    const int monitor   = argc > 2 ? std::atoi(argv[2]) : 0;
    const int fps       = argc > 3 ? std::atoi(argv[3]) : 60;
    const std::string enc = argc > 4 ? argv[4] : "auto";

    log_set_level(LogLevel::Info);
    log_set_file("videoprobe.log");

    std::printf("=== MiniStream video pipeline probe ===\n");

    // ---- monitor enumeration ----
    const auto monitors = ScreenCapture::enumerate_monitors();
    std::printf("monitors detected: %zu\n", monitors.size());
    for (const auto& m : monitors) {
        std::printf("  [%d] %s  %dx%d @(%d,%d) %s%s\n",
                    m.index, m.deviceName.c_str(), m.width, m.height, m.left, m.top,
                    m.isPrimary ? "primary" : "", m.isAttached ? "" : " (detached)");
    }
    if (monitors.empty()) {
        std::printf("ERROR: no monitor found (common under RDP / no display output)\n");
        return 2;
    }

    // ---- capture ----
    // 参数：显示器序号, 采集光标, 输出宽, 输出高（0 = 原生分辨率）
    // 可用命令行第 5 个参数指定缩放到 720p，用来验证 GPU 缩放路径：
    //     ministream-videoprobe 5 0 60 auto 1280x720
    int outW = 0, outH = 0;
    if (argc > 5) {
        const std::string sizeArg = argv[5];
        const size_t x = sizeArg.find('x');
        if (x != std::string::npos) {
            outW = std::atoi(sizeArg.substr(0, x).c_str());
            outH = std::atoi(sizeArg.substr(x + 1).c_str());
        }
    }

    ScreenCapture cap;
    std::string err;
    if (!cap.init(monitor, true, outW, outH, err)) {
        std::printf("ERROR: capture init failed: %s\n", err.c_str());
        return 3;
    }
    std::printf("capture started: monitor %d, native %dx%d -> output %dx%d\n",
                monitor, cap.native_width(), cap.native_height(), cap.width(), cap.height());

    // ---- encoder ----
    EncoderConfig ecfg;
    ecfg.preferred = enc;
    ecfg.codec = proto::VideoCodec::H264;
    ecfg.width = cap.width();
    ecfg.height = cap.height();
    ecfg.fps = fps;
    ecfg.bitrateKbps = auto_bitrate_kbps(ecfg.width, ecfg.height, fps);
    ecfg.keyframeIntervalSec = 2;

    VideoEncoder venc;
    if (!venc.init(ecfg, err)) {
        std::printf("ERROR: encoder init failed: %s\n", err.c_str());
        return 4;
    }
    std::printf("encoder: %s (%s)  %dx%d @%dfps  target %d kbps\n",
                venc.encoder_name().c_str(), venc.is_hardware() ? "hardware" : "software",
                ecfg.width, ecfg.height, fps, ecfg.bitrateKbps);

    std::printf("probing for %d seconds - keep something moving on screen...\n", seconds);

    // ---- main loop ----
    const i64 t0 = now_us();
    const i64 endUs = t0 + i64(seconds) * 1000000;
    const i64 slotUs = i64(1000000) / (fps > 0 ? fps : 60);

    RateCounter captureRate, encodeRate, byteRate;
    LatencyStats encodeLatency, convertLatency;
    u64 framesCaptured = 0, framesEncoded = 0, keyframes = 0, bytesTotal = 0;
    int timeouts = 0;
    int errs = 0;
    i64 lastTick = 0;

    Nv12Frame frame;

    while (now_us() < endUs) {
        const int r = cap.acquire_frame(int(slotUs / 1000), err);
        if (r < 0) {
            std::printf("capture error: %s (reinitializing)\n", err.c_str());
            cap.shutdown();
            if (!cap.init(monitor, true, outW, outH, err)) {
                std::printf("reinit failed: %s\n", err.c_str());
                return 5;
            }
            ++errs;
            continue;
        }
        if (r == 0) { ++timeouts; continue; }

        ++framesCaptured;
        captureRate.add(1);

        const i64 cvt0 = now_us();
        const bool ok = cap.to_nv12(frame, err);
        const i64 cvtUs = now_us() - cvt0;
        cap.release_frame();
        if (!ok) {
            std::printf("BGRA->NV12 failed: %s\n", err.c_str());
            ++errs;
            continue;
        }
        convertLatency.add(cvtUs);

        const i64 enc0 = now_us();
        const int sr = venc.submit(frame.y(), frame.strideY, frame.uv(), frame.strideUV,
                                   now_us(), false);
        if (sr < 0) {
            std::printf("encode submit failed\n");
            ++errs;
            break;
        }
        if (sr == 0) continue;   // encoder busy: drop this frame (real path retries)

        EncodedPacket pkt;
        int pr;
        while ((pr = venc.poll(pkt, 0)) == 1) {
            ++framesEncoded;
            if (pkt.keyframe) ++keyframes;
            bytesTotal += pkt.data.size();
            encodeRate.add(1);
            byteRate.add(pkt.data.size());
            encodeLatency.add(now_us() - enc0);
        }
        if (pr < 0) { std::printf("encode poll failed\n"); ++errs; break; }

        const i64 now = now_us();
        if (now - lastTick > 1000000) {
            lastTick = now;
            std::printf("  t=%4.1fs  cap %.1f fps | enc %.1f fps | %.0f kbps | enc-lat %.2f ms | nv12 %.2f ms | timeouts %d\n",
                        double(now - t0) / 1e6,
                        captureRate.rate(), encodeRate.rate(), byteRate.kbps(),
                        double(encodeLatency.avg()) / 1000.0,
                        double(convertLatency.avg()) / 1000.0,
                        timeouts);
            // 光标诊断：形状/位置是否取到了（远控端"看不到鼠标"就查这一行）
            const PointerInfo& ptr = cap.pointer();
            std::printf("            pointer: avail=%d visible=%d pos=(%d,%d) shapeId=%u %dx%d hot=(%d,%d) bytes=%u\n",
                        int(ptr.available), int(ptr.visible), ptr.x, ptr.y, ptr.shapeId,
                        ptr.width, ptr.height, ptr.hotX, ptr.hotY,
                        unsigned(ptr.bgra.size()));
        }
    }

    const i64 elapsed = now_us() - t0;
    std::printf("\n=== results ===\n");
    std::printf("elapsed         : %.2f s\n", double(elapsed) / 1e6);
    std::printf("captured frames : %llu (%.1f fps)\n",
                (unsigned long long)framesCaptured,
                double(framesCaptured) * 1e6 / double(elapsed));
    std::printf("encoded frames  : %llu (%.1f fps)\n",
                (unsigned long long)framesEncoded,
                double(framesEncoded) * 1e6 / double(elapsed));
    std::printf("keyframes       : %llu\n", (unsigned long long)keyframes);
    std::printf("total bitstream : %.2f MB (avg %.0f kbps)\n",
                double(bytesTotal) / 1048576.0,
                double(bytesTotal) * 8.0 * 1e6 / double(elapsed) / 1000.0);
    std::printf("encode latency  : avg %.2f ms, max %.2f ms\n",
                double(encodeLatency.avg()) / 1000.0, double(encodeLatency.max()) / 1000.0);
    std::printf("nv12 convert    : avg %.2f ms, max %.2f ms\n",
                double(convertLatency.avg()) / 1000.0, double(convertLatency.max()) / 1000.0);
    std::printf("capture timeouts: %d (high is normal when the screen is static)\n", timeouts);
    std::printf("errors          : %d\n", errs);

    const bool pass = framesEncoded > 0 && errs == 0;
    std::printf("\n%s\n", pass ? "PROBE PASS: capture + encode pipeline works."
                                : "PROBE FAIL: see errors above.");

    venc.shutdown();
    cap.shutdown();
    log_close();
    return pass ? 0 : 1;
}
