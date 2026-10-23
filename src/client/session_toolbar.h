// =============================================================================
//  session_toolbar.h - 远控中的顶部工具栏（RustDesk 式）
//
//  设计目标：**绝不能打断游戏**。
//   * 平时完全隐藏；光标碰到屏幕顶边才滑出来，离开 3 秒后自动缩回去
//   * WS_EX_NOACTIVATE + WS_EX_TOPMOST：显示时不抢键盘焦点，
//     游戏键鼠捕获状态原样保留（RustDesk 工具栏同款做法）
//   * 所有控件都通过父窗口转发命令，不会让渲染窗口失焦
//  功能：画质档位（均衡/画质/流畅/自定义）、码率滑条、帧率、关键帧间隔、
//        显示模式（全屏/窗口）、悬浮条开关、断开连接。
// =============================================================================
#pragma once

#include "../common/types.h"
#include "../common/quality.h"
#include "control_client.h"

#include <functional>
#include <mutex>
#include <string>

namespace ms::client {

class SessionToolbar {
public:
    // 渲染窗口（工具栏是它的子窗口，盖在顶部）
    bool init(HWND parentWnd, std::string& err);
    void shutdown();

    // ---- 生命周期 ----
    // 光标碰到顶边 -> show；离开 -> 计时自动 hide
    void on_mouse_move(POINT screenPos);
    void poll_auto_hide();               // 每帧调一次，管理 3 秒自动隐藏
    bool visible() const { return shown_; }
    // 主动唤出（释放键鼠快捷键的联动）：与光标碰顶边等价，但不依赖光标位置判定
    void summon();
    // 收起画质面板（点画面/应用后调用；工具栏保持显示）
    void collapse_panel();
    // 光标是否停在我们的界面（工具栏/画质面板/小悬浮按钮）上。
    // 用来避免"点本机按钮时把同一发点击也注入到远端游戏"。
    bool cursor_over_ui() const;

    // ---- 会话状态同步 ----
    void set_stream_params(int nativeW, int nativeH, int curW, int curH, int curFps, int curKbps);
    void set_connection_status(const std::string& text);   // 顶部一行状态字
    // 双线状态：备线接入时显示「双线·键鼠走备线」，断开时显示「单线」
    void set_backup_line_status(bool active, const wchar_t* rttText = nullptr);
    // 锐化档位（0=关 1=低 2=中 3=高）：初始化画质面板里的下拉，不触发回调
    void set_sharpen_level(int level);
    // 编码格式（1=H.264 2=HEVC）：初始化「编码」下拉的当前项，不触发回调
    void set_codec_value(int codec);
    // 声音按钮的初始勾选状态
    void set_audio_enabled(bool on);

    // ---- 命令回调（全部在父窗口线程被调用）----
    // quality：完整档位参数；needReconnect 表示该改动需要重连（分辨率/fps 改变）
    struct Commands {
        std::function<void()>                on_disconnect;
        std::function<void(bool fullscreen)> on_toggle_fullscreen;
        std::function<void(bool overlayOn)>  on_toggle_overlay;
        // 声音开/关：false = 本机静音（被控端继续发，切回来立刻有声音）
        std::function<void(bool audioOn)>    on_toggle_audio;
        std::function<void(const quality::QualityProfile& q, bool needReconnect)> on_quality_change;
        std::function<void(int kbps)>        on_bitrate_change;   // 运行期热调，不用重连
        std::function<void(int sec)>         on_keyframe_interval_change;
        // 显示缩放模式：0=智能缩放 1=拉伸填充 2=原始大小(1:1)
        std::function<void(int mode)>        on_scale_mode;
        // 锐化档位：0=关 1=低 2=中 3=高（着色器 uniform，热生效）
        std::function<void(int level)>       on_sharpen_change;
        // 编码格式切换：1=H.264 2=HEVC(H.265)。改完立即生效（被控端重建编码器，
        // 远控端自动重建解码器并等下一个 IDR），不需要重连。
        std::function<void(int codec)>       on_codec_change;
    };
    void set_commands(const Commands& c) { cmds_ = c; }

private:
    static LRESULT CALLBACK toolbar_proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK panel_proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK dock_proc(HWND, UINT, WPARAM, LPARAM);
    // 进程内只有一个工具栏实例，窗口过程用它找回 this
    static SessionToolbar* s_this_;
    static SessionToolbar* instance(HWND wnd);   // 优先 GWLP_USERDATA

    HWND parent_ = nullptr;
    HWND wnd_ = nullptr;           // 容器条
    HWND dock_ = nullptr;          // 顶部中间的小悬浮按钮（点击展开工具栏）
    bool dockHovered_ = false;
    HWND pnlQuality_ = nullptr;    // 展开后的画质面板
    HWND cbPreset_ = nullptr;
    HWND cbRes_ = nullptr;
    HWND cbFps_ = nullptr;
    HWND cbBitrate_ = nullptr;
    HWND edBitrate_ = nullptr;     // 自定义码率输入框
    HWND chkOverlay_ = nullptr;
    HWND chkAudio_ = nullptr;      // 声音开/关（静音只是本地停止播放，被控端照常发）
    HWND btnDisconnect_ = nullptr;
    HWND btnFullscreen_ = nullptr;
    HWND btnScale_ = nullptr;      // 缩放模式（智能缩放/拉伸/1:1）循环切换
    HWND btnChevron_ = nullptr;    // 展开/收起画质面板
    HWND stStatus_ = nullptr;
    // 画质面板每行的文字标签（原来靠下拉项文本暗示，用户根本看不出哪行是什么）
    HWND stLblPreset_ = nullptr, stLblRes_ = nullptr, stLblFps_ = nullptr;
    HWND stLblKf_ = nullptr, stLblBitrate_ = nullptr, stLblSharpen_ = nullptr;
    HWND cbSharpen_ = nullptr;      // 锐化档位下拉
    HWND stLblCodec_ = nullptr;     // 「编码」行标签
    HWND cbCodec_ = nullptr;        // 编码格式下拉（H.264 / H.265）
    int  codecValue_ = 1;           // 当前编码格式（1=H.264 2=HEVC）
    HWND btnPanelClose_ = nullptr;  // 面板右上角「收起」（不再靠再点一次"画质"）
    HFONT font_ = nullptr;

    // ---- cpp 内部新增控件 ----
    HWND btnApply_ = nullptr;      // 面板「应用」按钮
    HWND cbKeyframe_ = nullptr;    // 关键帧间隔下拉

    bool  shown_ = false;
    bool  expanded_ = false;
    int   sepX_ = 516;             // 状态字与按钮之间的分隔线 x（rebuild_layout 里算）
    i64   lastActivityUs_ = 0;
    int   nativeW_ = 0, nativeH_ = 0;
    int   curFps_ = 60, curKbps_ = 0;
    int   sharpenLevel_ = 1;       // 0=关 1=低 2=中 3=高
    // 按钮防抖：同一个按钮在双击时间内的第二次 BN_CLICKED 直接吞掉。
    // 原来"画质"是纯 toggle，用户双击（或鼠标连击）就会瞬间开了又关，
    // 看起来就是"面板闪一下自己消失 / 按钮点不开"。
    i64   lastClickUs_[8] = {};
    bool  click_debounced(int slot);   // true = 这次点击应被忽略

    Commands cmds_;

    // ---- 运行期状态（仅 cpp 内部使用）----
    std::wstring statusText_;        // 顶部状态字缓存
    std::wstring connStatus_;        // 连接状态附加文本

    // ---- 跨线程状态同步 ----------------------------------------------------
    // set_stream_params() 会被**控制线程**调到（重协商完成回调在 control_loop
    // 里执行），而它要更新的是窗口控件。Win32 窗口只能被创建它的线程碰，
    // 直接在别的线程 SetWindowTextW 会阻塞在消息投递上，并且和 UI 线程同时
    // 读写 std::wstring 成员有堆破坏风险（表现为随机闪退）。
    // 所以这里只缓存参数并 PostMessage 回 UI 线程，由 UI 线程真正刷新控件。
    std::mutex   syncMtx_;
    int          pendingW_ = 0, pendingH_ = 0;
    int          pendingFps_ = 0, pendingKbps_ = 0;
    std::wstring pendingConn_;
    bool         pendingConnDirty_ = false;
    void post_sync();
    void apply_sync();               // 只在 UI 线程调用

    HFONT fontMono_ = nullptr;       // Consolas 9pt（状态字）
    HBRUSH brushToolbar_ = nullptr;  // WM_CTLCOLOR 返回的背景刷（init 建/shutdown 删）
    HBRUSH brushPanel_ = nullptr;
    int  curW_ = 0, curH_ = 0;       // set_stream_params 记下的当前分辨率
    int  curKeyframeSec_ = 2;        // 当前关键帧间隔（下拉初始位置）
    bool fullscreenState_ = false;   // 全屏按钮的当前状态
    int  scaleMode_ = 0;             // 0=智能缩放 1=拉伸 2=1:1（按钮循环）
    bool suppressComboEvents_ = false; // 程序改下拉选中项时吞掉 CBN_SELCHANGE
    int  comboPresetCustomIdx_ = -1; // cbPreset_ 里「自定义」项的索引

    void show(bool on);
    void set_expanded(bool on);
    void rebuild_layout();
    // 按"父窗口客户区"摆放工具栏/画质面板/小悬浮按钮（窗口化时不会被标题栏挡）
    void place_windows();
    void paint_dock();            // 画顶部小悬浮按钮（GDI）
    void apply_preset_selection();
    quality::QualityProfile collect_profile(bool* needReconnect) const;
    void refresh_status_text();      // 重建 statusText_ 并更新 stStatus_
    void mark_manual_edit();         // 手动改参数 -> 预设变「自定义」
    void notify_cursor_overlay(bool on); // WM_APP+0x520 通知父窗口显示/隐藏光标
};

} // namespace ms::client
