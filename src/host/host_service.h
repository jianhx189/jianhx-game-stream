// =============================================================================
//  host_service.h - 被控端串流服务
//
//  线程模型（延迟敏感，刻意分成三条线）：
//    accept 线程 : 监听 TCP -> 握手/口令校验 -> 会话期间跑控制消息与心跳 -> 断线回收
//    stream 线程 : DXGI 采集 -> NV12 -> 编码 -> UDP 分片发送
//    input  线程 : UDP 收包 -> SendInput 注入（必须独立，否则输入延迟会被帧率绑架）
// =============================================================================
#pragma once

#include "../common/types.h"
#include "../common/config.h"
#include "../common/protocol.h"
#include <functional>
#include <memory>
#include <string>

namespace ms::host {

struct HostStatus {
    bool        listening = false;
    bool        clientConnected = false;
    std::string clientAddr;
    std::string encoderName;
    bool        hardwareEncoder = false;
    int         width = 0, height = 0, fps = 0, bitrateKbps = 0;
    double      captureFps = 0;
    double      encodeFps = 0;
    double      sendKbps = 0;
    int         encodeLatencyUs = 0;
    u64         framesSent = 0;
    u64         framesDropped = 0;
    // 来自远控端的反馈（用于后续码率自适应）
    u32         clientRecvKbps = 0;
    u32         clientFramesLost = 0;
    u32         clientLatencyUs = 0;
    std::string lastError;
};

class HostService {
public:
    using StatusSink = std::function<void(const HostStatus&)>;

    HostService();
    ~HostService();
    HostService(const HostService&) = delete;
    HostService& operator=(const HostService&) = delete;

    // 启动监听（非阻塞，工作线程在内部起）
    bool start(const HostConfig& cfg, std::string& err);
    // 停止服务并回收所有线程，可重复调用
    void stop();
    bool running() const;

    HostStatus status() const;
    void set_status_sink(StatusSink sink);

    // 调试/压测用：强制下一帧为 IDR
    void force_keyframe();
    // 运行中改码率
    void set_bitrate(int kbps);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ms::host
