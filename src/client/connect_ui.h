// =============================================================================
//  connect_ui.h - 远控端连接界面
//
//  一个普通 Win32 窗口：IP/域名、端口(>10000)、验证码、全屏开关、连接按钮。
//  确认后返回 ConnectParams，主程序拿去做握手。
// =============================================================================
#pragma once

#include "../common/types.h"
#include "../common/config.h"
#include "../common/quality.h"
#include "control_client.h"
#include <string>

namespace ms::client {

struct ConnectDialogResult {
    bool ok = false;
    ConnectParams params;
    bool fullscreen = true;
    bool overlayEnabled = true;       // 连接后显示悬浮状态条
    bool rememberPassword = true;     // 是否记住验证码
    quality::QualityProfile profile;  // 面板上最终生效的画质档
};

class ConnectUi {
public:
    // 模态显示；阻塞直到用户点「连接」或关闭窗口。
    // initial 用来回填上次的地址/端口（密码可留空则要求重新输入）。
    // priorError：上一次连接失败的原因（非空时在错误区就地显示，红字），
    // 用于"失败 -> 改参数 -> 重连"的循环，绝不用 MessageBox。
    static bool show(HINSTANCE hinst, const ClientConfig& initial,
                     ConnectDialogResult& out, const std::string& priorError = "");
};

} // namespace ms::client
