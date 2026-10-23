// =============================================================================
//  config.h - INI 配置读写
//
//  需求：本地配置文件存密码；端口/码率/帧率/显示器可选；提供示例配置。
//  刻意不引入 JSON/YAML 库，自己写 60 行 INI 解析，保持零第三方依赖。
// =============================================================================
#pragma once

#include "types.h"
#include "quality.h"
#include <map>
#include <string>

namespace ms {

class IniFile {
public:
    bool load(const std::string& path);
    bool save(const std::string& path) const;

    bool has(const std::string& sec, const std::string& key) const;
    std::string get_str(const std::string& sec, const std::string& key, const std::string& def = "") const;
    int         get_int(const std::string& sec, const std::string& key, int def = 0) const;
    bool        get_bool(const std::string& sec, const std::string& key, bool def = false) const;
    double      get_double(const std::string& sec, const std::string& key, double def = 0.0) const;

    void set_str(const std::string& sec, const std::string& key, const std::string& val);
    void set_int(const std::string& sec, const std::string& key, int val);
    void set_bool(const std::string& sec, const std::string& key, bool val);

private:
    // 保持插入顺序，写回文件时可读性更好
    std::vector<std::string> sections_;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> data_;
};

// -----------------------------------------------------------------------------
// 被控端配置 host.ini
// -----------------------------------------------------------------------------
struct HostConfig {
    // [server]
    u16         port       = defaults::kPort;     // 必须 > 10000
    std::string password   = "changeme";
    std::string bind       = "0.0.0.0";
    // 双线：备用线路端口。0 = 不启用。
    // 备线是独立的 TCP 连接，只传键鼠/控制，不传视频。主线照旧传视频+控制。
    // 客户端 client.ini 里也要配 backup_host/backup_port 指向同一端口。
    u16         backupPort = 0;

    // [video]
    int         fps        = defaults::kFps;      // 30/60/120/144
    int         bitrateKbps = 0;                  // 0 = 按分辨率自适应
    int         width      = 0;                   // 0 = 原生
    int         height     = 0;
    std::string encoder    = "auto";              // auto|nvenc|amf|qsv|cpu
    // 视频编码格式：h264 / hevc(H.265) / auto。
    // auto = 自动协商：远控端声明支持 HEVC 时自动选 H.265（同画质省 30~40% 码率），
    // 否则退回 H.264（兼容性最好）。推荐 auto。
    std::string codec      = "auto";
    // ---- 编码器细调（对齐 Parsec 暴露的 config 键）------------------------
    // Parsec 的 parsecd 里有这些同名键：encoder_min_qp / encoder_max_qp /
    // encoder_slices / encoder_vbv_multi / encoder_idr_interval。
    // QP 限制：-1 = 不限制（走码率控制）；设了上下限可以让编码器在复杂场景
    // 宁可超码率也不糊，或者反过来（画质优先 / 流畅优先的硬开关）。
    int         minQp      = -1;
    int         maxQp      = -1;
    int         slices     = 1;      // 1 = 单 slice（低延迟，Moonlight 同款）
    int         vbvMultiplier = 5;   // VBV = 码率 × N ÷ fps（N 帧的缓冲）
    int         monitor    = 0;                   // 显示器索引
    int         keyframeIntervalSec = 0;          // 0=按需 IDR（推荐）；>0=周期兜底（秒）

    // [input]
    bool        inputEnabled = true;

    // [audio]：被控端系统声音（游戏/音乐）传给远控端
    bool        audioEnabled = true;
    int         audioBitrateKbps = 96;   // 32..320

    // [log]
    int         logLevel   = 2;                   // 0..4

    // 运行时信息（不写入 ini）
    std::string configPath;

    static bool load(const std::string& path, HostConfig& out, std::string& err);
    bool save(std::string& err) const;
    // 校验并纠正非法值（端口范围、fps 白名单等）
    bool validate(std::string& err) const;
};

// -----------------------------------------------------------------------------
// 远控端配置 client.ini
// -----------------------------------------------------------------------------
struct ClientConfig {
    // [client]
    std::string host      = "127.0.0.1";
    u16         port      = defaults::kPort;
    std::string password  = "changeme";
    bool        fullscreen = true;
    // ---- 备用线路（双线容灾）--------------------------------------------
    // 主线路连不上 / 会话中途掉线时，自动改用备用线路（通常是第二台 FRP）。
    // 留空表示不启用。端口 0 表示与主线路相同。
    std::string backupHost;
    u16         backupPort = 0;
    // 掉线后是否自动重连（先试当前线路，再试备用线路，交替）。0 = 不自动重连
    int         autoReconnect = 1;
    // 关闭窗口时最小化到托盘而不退出（1 = 常驻后台，0 = 直接退出）
    bool        minimizeToTray = true;

    // [video]
    int         fps        = defaults::kFps;
    int         bitrateKbps = 0;
    int         width      = 0;    // 期望分辨率，0 = 跟随被控端
    int         height     = 0;
    // 锐化档位：0=关 1=低 2=中 3=高（客户端着色器里对亮度做反锐化，
    // 用来把视频编码糊掉的字/远处目标拉回来；全程热切换，不用重连）
    int         sharpen    = 1;

    // [input]
    bool        captureOnConnect = true;
    std::string releaseHotkey    = "CTRL+ALT+SHIFT+Q";
    // 鼠标模式：0=自动（被控端光标可见->绝对映射，隐藏->相对）
    //           1=绝对（桌面/菜单式：远端光标 = 你本机鼠标位置，1:1）
    //           2=相对（游戏内场式：发原始位移，本机光标隐藏）
    int         mouseMode        = 0;

    // [video]（连接面板的画质档记忆）
    int         preset = 0;           // quality::Preset
    bool        rememberPassword = true;

    // [audio]：远控端是否播放被控端声音（本机静音开关在工具栏「声音」）
    bool        audioEnabled = true;

    // [overlay]
    bool        overlayEnabled = true;
    int         overlayCorner  = 1;   // 0=左上 1=右上 2=左下 3=右下

    // [log]
    int         logLevel   = 2;

    std::string configPath;

    static bool load(const std::string& path, ClientConfig& out, std::string& err);
    bool save(std::string& err) const;
    bool validate(std::string& err) const;
};

} // namespace ms
