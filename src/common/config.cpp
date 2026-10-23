// =============================================================================
//  config.cpp - INI 解析与配置校验
// =============================================================================
#include "config.h"
#include "log.h"
#include "fsutil.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace ms {
namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r' || s[b-1] == '\n')) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

} // namespace

bool IniFile::load(const std::string& path) {
    std::string text;
    if (!fsutil::read_text_file(path, text)) return false;

    sections_.clear();
    data_.clear();

    std::string line, cur;
    std::istringstream in(text);
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        if (t[0] == '[') {
            const size_t e = t.find(']');
            if (e == std::string::npos) continue;
            cur = trim(t.substr(1, e - 1));
            if (data_.find(cur) == data_.end()) sections_.push_back(cur);
            continue;
        }
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(t.substr(0, eq));
        const std::string v = trim(t.substr(eq + 1));
        if (k.empty()) continue;
        auto& kv = data_[cur];
        bool replaced = false;
        for (auto& p : kv) {
            if (p.first == k) { p.second = v; replaced = true; break; }
        }
        if (!replaced) kv.emplace_back(k, v);
        if (std::find(sections_.begin(), sections_.end(), cur) == sections_.end())
            sections_.push_back(cur);
    }
    return true;
}

bool IniFile::save(const std::string& path) const {
    std::string out;
    for (const auto& sec : sections_) {
        auto it = data_.find(sec);
        if (it == data_.end()) continue;
        out += "[";
        out += sec;
        out += "]\n";
        for (const auto& kv : it->second) {
            out += kv.first;
            out += "=";
            out += kv.second;
            out += "\n";
        }
        out += "\n";
    }
    return fsutil::write_text_file(path, out);
}

bool IniFile::has(const std::string& sec, const std::string& key) const {
    auto it = data_.find(sec);
    if (it == data_.end()) return false;
    for (const auto& kv : it->second) if (kv.first == key) return true;
    return false;
}

std::string IniFile::get_str(const std::string& sec, const std::string& key, const std::string& def) const {
    auto it = data_.find(sec);
    if (it == data_.end()) return def;
    for (const auto& kv : it->second) if (kv.first == key) return kv.second;
    return def;
}

int IniFile::get_int(const std::string& sec, const std::string& key, int def) const {
    const std::string s = get_str(sec, key, "");
    if (s.empty()) return def;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str()) return def;
    return int(v);
}

bool IniFile::get_bool(const std::string& sec, const std::string& key, bool def) const {
    const std::string s = lower(get_str(sec, key, ""));
    if (s.empty()) return def;
    if (s == "1" || s == "true" || s == "yes" || s == "on")  return true;
    if (s == "0" || s == "false" || s == "no" || s == "off") return false;
    return def;
}

double IniFile::get_double(const std::string& sec, const std::string& key, double def) const {
    const std::string s = get_str(sec, key, "");
    if (s.empty()) return def;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return def;
    return v;
}

void IniFile::set_str(const std::string& sec, const std::string& key, const std::string& val) {
    if (std::find(sections_.begin(), sections_.end(), sec) == sections_.end())
        sections_.push_back(sec);
    auto& kv = data_[sec];
    for (auto& p : kv) if (p.first == key) { p.second = val; return; }
    kv.emplace_back(key, val);
}

void IniFile::set_int(const std::string& sec, const std::string& key, int val) {
    set_str(sec, key, std::to_string(val));
}

void IniFile::set_bool(const std::string& sec, const std::string& key, bool val) {
    set_str(sec, key, val ? "true" : "false");
}

// -----------------------------------------------------------------------------
// HostConfig
// -----------------------------------------------------------------------------
bool HostConfig::load(const std::string& path, HostConfig& out, std::string& err) {
    IniFile ini;
    out.configPath = path;
    if (!ini.load(path)) {
        err = "无法读取配置文件: " + path + "（将使用默认值）";
        return false;
    }
    out.port        = u16(ini.get_int("server", "port", defaults::kPort));
    out.backupPort  = u16(ini.get_int("server", "backup_port", 0));
    out.password    = ini.get_str("server", "password", "changeme");
    out.bind        = ini.get_str("server", "bind", "0.0.0.0");

    out.fps         = ini.get_int("video", "fps", defaults::kFps);
    out.bitrateKbps = ini.get_int("video", "bitrate_kbps", 0);
    out.width       = ini.get_int("video", "width", 0);
    out.height      = ini.get_int("video", "height", 0);
    out.encoder     = lower(ini.get_str("video", "encoder", "auto"));
    out.codec       = lower(ini.get_str("video", "codec", "auto"));
    out.minQp       = ini.get_int("video", "min_qp", -1);
    out.maxQp       = ini.get_int("video", "max_qp", -1);
    out.slices      = ini.get_int("video", "slices", 1);
    out.vbvMultiplier = ini.get_int("video", "vbv_multi", 5);
    out.monitor     = ini.get_int("video", "monitor", 0);
    out.keyframeIntervalSec = ini.get_int("video", "keyframe_interval_sec", 0);

    out.inputEnabled = ini.get_bool("input", "enabled", true);
    out.audioEnabled = ini.get_bool("audio", "enabled", true);
    out.audioBitrateKbps = ini.get_int("audio", "bitrate_kbps", 96);
    out.logLevel     = ini.get_int("log", "level", 2);
    return true;
}

bool HostConfig::save(std::string& err) const {
    IniFile ini;
    ini.set_int("server", "port", int(port));
    ini.set_int("server", "backup_port", int(backupPort));
    ini.set_str("server", "password", password);
    ini.set_str("server", "bind", bind);

    ini.set_int("video", "fps", fps);
    ini.set_int("video", "bitrate_kbps", bitrateKbps);
    ini.set_int("video", "width", width);
    ini.set_int("video", "height", height);
    ini.set_str("video", "encoder", encoder);
    ini.set_str("video", "codec", codec);
    ini.set_int("video", "min_qp", minQp);
    ini.set_int("video", "max_qp", maxQp);
    ini.set_int("video", "slices", slices);
    ini.set_int("video", "vbv_multi", vbvMultiplier);
    ini.set_int("video", "monitor", monitor);
    ini.set_int("video", "keyframe_interval_sec", keyframeIntervalSec);

    ini.set_bool("input", "enabled", inputEnabled);
    ini.set_bool("audio", "enabled", audioEnabled);
    ini.set_int("audio", "bitrate_kbps", audioBitrateKbps);
    ini.set_int("log", "level", logLevel);

    if (!ini.save(configPath)) {
        err = "无法写入配置文件: " + configPath;
        return false;
    }
    return true;
}

bool HostConfig::validate(std::string& err) const {
    if (!is_valid_port(port)) {
        err = "端口必须在 10001-65535 之间（当前 " + std::to_string(port) + "）";
        return false;
    }
    if (password.empty()) {
        err = "访问密码不能为空";
        return false;
    }
    if (fps != 30 && fps != 60 && fps != 120 && fps != 144 && fps != 240) {
        err = "帧率只支持 30/60/120/144/240（当前 " + std::to_string(fps) + "）";
        return false;
    }
    if (bitrateKbps < 0) { err = "码率不能为负"; return false; }
    if (backupPort != 0 && !is_valid_port(backupPort)) {
        err = "备用线路端口不合法（当前 " + std::to_string(backupPort) + "）";
        return false;
    }
    if (codec != "h264" && codec != "hevc" && codec != "h265" && codec != "auto") {
        err = "编码格式只支持 h264 / hevc（当前 " + codec + "）";
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// ClientConfig
// -----------------------------------------------------------------------------
bool ClientConfig::load(const std::string& path, ClientConfig& out, std::string& err) {
    IniFile ini;
    out.configPath = path;
    if (!ini.load(path)) {
        err = "无法读取配置文件: " + path + "（将使用默认值）";
        return false;
    }
    out.host       = ini.get_str("client", "host", "127.0.0.1");
    out.port       = u16(ini.get_int("client", "port", defaults::kPort));
    out.password   = ini.get_str("client", "password", "changeme");
    out.fullscreen = ini.get_bool("client", "fullscreen", true);
    // 双线容灾：备用线路（第二台 FRP）。留空 = 不启用。
    out.backupHost = ini.get_str("client", "backup_host", "");
    out.backupPort = u16(ini.get_int("client", "backup_port", 0));
    out.autoReconnect   = ini.get_int("client", "auto_reconnect", 1);
    out.minimizeToTray  = ini.get_bool("client", "minimize_to_tray", true);

    out.fps         = ini.get_int("video", "fps", defaults::kFps);
    out.bitrateKbps = ini.get_int("video", "bitrate_kbps", 0);
    out.width       = ini.get_int("video", "width", 0);
    out.height      = ini.get_int("video", "height", 0);
    out.sharpen     = ini.get_int("video", "sharpen", 1);

    out.captureOnConnect = ini.get_bool("input", "capture_on_connect", true);
    out.releaseHotkey    = ini.get_str("input", "release_hotkey", "CTRL+ALT+SHIFT+Q");
    {
        // 鼠标模式：auto / absolute / relative（也接受 0/1/2）
        const std::string mm = ini.get_str("input", "mouse_mode", "auto");
        out.mouseMode = (mm == "absolute" || mm == "abs" || mm == "1") ? 1
                      : ((mm == "relative" || mm == "rel" || mm == "2") ? 2 : 0);
    }

    out.preset           = ini.get_int("video", "preset", 0);
    out.rememberPassword = ini.get_bool("video", "remember_password", true);

    out.audioEnabled   = ini.get_bool("audio", "enabled", true);
    out.overlayEnabled = ini.get_bool("overlay", "enabled", true);
    out.overlayCorner  = ini.get_int("overlay", "corner", 1);
    out.logLevel       = ini.get_int("log", "level", 2);
    return true;
}

bool ClientConfig::save(std::string& err) const {
    IniFile ini;
    ini.set_str("client", "host", host);
    ini.set_int("client", "port", int(port));
    ini.set_str("client", "password", password);
    ini.set_bool("client", "fullscreen", fullscreen);
    ini.set_str("client", "backup_host", backupHost);
    ini.set_int("client", "backup_port", int(backupPort));
    ini.set_int("client", "auto_reconnect", autoReconnect);
    ini.set_bool("client", "minimize_to_tray", minimizeToTray);

    ini.set_int("video", "fps", fps);
    ini.set_int("video", "bitrate_kbps", bitrateKbps);
    ini.set_int("video", "width", width);
    ini.set_int("video", "height", height);
    ini.set_int("video", "sharpen", sharpen < 0 ? 0 : (sharpen > 3 ? 3 : sharpen));

    ini.set_bool("input", "capture_on_connect", captureOnConnect);
    ini.set_str("input", "release_hotkey", releaseHotkey);
    ini.set_str("input", "mouse_mode",
                mouseMode == 1 ? "absolute" : (mouseMode == 2 ? "relative" : "auto"));

    ini.set_int("video", "preset", preset);
    ini.set_bool("video", "remember_password", rememberPassword);

    ini.set_bool("audio", "enabled", audioEnabled);
    ini.set_bool("overlay", "enabled", overlayEnabled);
    ini.set_int("overlay", "corner", overlayCorner);
    ini.set_int("log", "level", logLevel);

    if (!ini.save(configPath)) {
        err = "无法写入配置文件: " + configPath;
        return false;
    }
    return true;
}

bool ClientConfig::validate(std::string& err) const {
    if (host.empty()) { err = "服务端地址不能为空"; return false; }
    if (!is_valid_port(port)) {
        err = "端口必须在 10001-65535 之间（当前 " + std::to_string(port) + "）";
        return false;
    }
    if (password.empty()) { err = "验证码不能为空"; return false; }
    if (overlayCorner < 0 || overlayCorner > 3) { err = "overlay corner 取值 0-3"; return false; }
    // 备用线路：两者都填才算启用；只填一半是配置错误（静默失效最难查）
    if (!backupHost.empty() && backupPort != 0 && !is_valid_port(backupPort)) {
        err = "备用线路端口不合法（当前 " + std::to_string(backupPort) + "）";
        return false;
    }
    return true;
}

} // namespace ms
