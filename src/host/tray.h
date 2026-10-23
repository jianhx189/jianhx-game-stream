// =============================================================================
//  tray.h - 被控端极简 UI（托盘图标 + 一个配置窗口）
//
//  需求：无多用户、无日志上传、UI 极简。
//  所以主窗口是隐藏的，托盘是唯一常驻入口；配置窗口按需弹出。
// =============================================================================
#pragma once

#include "../common/types.h"
#include "../common/config.h"
#include <functional>
#include <string>

#include <windows.h>   // HINSTANCE

namespace ms::host {

class HostUi {
public:
    using Action = std::function<void()>;
    using ConfigSaved = std::function<void(const HostConfig&)>;

    HostUi() = default;
    ~HostUi();

    // 注册窗口类、创建隐藏消息窗口、加载托盘图标
    bool create(HINSTANCE hinst, HostConfig* cfg, const std::string& configPath, std::string& err);
    void destroy();

    // 在调用线程跑消息循环（阻塞直到退出）
    int  message_loop();
    // 从任意线程请求退出
    void post_quit();

    // 更新托盘提示与配置窗口状态行（线程安全）
    void set_status(const std::string& text);
    void notify(const std::string& title, const std::string& msg);

    void set_on_service_start(Action a)   { onStart_ = std::move(a); }
    void set_on_service_stop(Action a)    { onStop_  = std::move(a); }
    void set_on_quit(Action a)            { onQuit_  = std::move(a); }
    void set_on_config_saved(ConfigSaved f){ onSaved_ = std::move(f); }

    // 当前服务是否在监听（用于菜单勾选）
    void set_running(bool r);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    HINSTANCE hinst_ = nullptr;
    HostConfig* cfg_ = nullptr;
    std::string configPath_;
    Action onStart_, onStop_, onQuit_;
    ConfigSaved onSaved_;
};

} // namespace ms::host
