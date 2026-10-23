// =============================================================================
//  client_session.h - 远控端会话
//
//  线程模型：
//    主线程    : 窗口消息（Raw Input 键鼠）+ 解码 + 渲染（不引入额外拷贝）
//    接收线程  : UDP 收包与分片重组（VideoReceiver 内部）
//    控制线程  : TCP 心跳/时钟同步/断线检测
// =============================================================================
#pragma once

#include "../common/types.h"
#include "control_client.h"
#include <string>

namespace ms::client {

struct SessionOptions {
    bool        fullscreen = true;
    bool        overlayEnabled = true;
    int         overlayCorner = 1;      // 0=左上 1=右上 2=左下 3=右下
    std::string releaseHotkey = "CTRL+ALT+SHIFT+Q";
    bool        captureOnConnect = true;
    bool        vsync = false;
    // 鼠标模式：0=自动（被控端光标可见->绝对映射，隐藏->相对原始位移）
    //           1=绝对（远端光标 = 本机鼠标位置，1:1，用于桌面/菜单）
    //           2=相对（游戏内场：本机光标隐藏，发原始位移）
    int         mouseMode = 0;
    // 锐化档位：0=关 1=低 2=中 3=高（客户端着色器反锐化，热生效）
    int         sharpen = 1;
    // 是否播放被控端声音（本机静音开关在工具栏「声音」）
    bool        audioEnabled = true;
};

struct ClientStatus {
    bool        connected = false;
    std::string serverAddr;
    std::string decoderName;
    bool        hardwareDecode = false;
    int         width = 0, height = 0, fps = 0;
    double      recvFps = 0;
    double      recvKbps = 0;
    double      lossPercent = 0;
    double      latencyMs = 0;
    double      decodeMs = 0;
    int         queueDepth = 0;
    int         rttMs = 0;
    u64         framesDecoded = 0;
    u64         framesDropped = 0;
    std::string lastError;
    // 掉线原因（被控端踢出时它给的 Disconnect.reason，或本机控制通道断开）
    std::string disconnectReason;
};

class ClientSession {
public:
    ClientSession() = default;
    ~ClientSession();

    // 阻塞跑完整个会话，直到断开或用户退出。hwnd 是已创建好的渲染窗口。
    // 返回 true 表示正常结束（用户主动退出），false 表示出错（err 给出原因）。
    bool run(HWND hwnd, const ConnectParams& params, const SessionOptions& opts, std::string& err);

    // 从窗口线程外部请求结束
    void request_stop();
    void request_keyframe();

    ClientStatus status() const;

    // 会话期间用户改过的锐化档位（0-3）。主程序在会话结束后写回 client.ini。
    int sharpen_level() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace ms::client
