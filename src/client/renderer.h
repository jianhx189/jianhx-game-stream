// =============================================================================
//  renderer.h - D3D11 渲染 + 悬浮状态条
//
//  低延迟渲染要点：
//   * 用 DXGI_SWAP_EFFECT_FLIP_DISCARD + 关闭垂直同步（Present(0, ALLOW_TEARING)）
//     否则每帧至少多等一个刷新周期（16.7ms @60Hz），端到端 <30ms 就不可能。
//   * NV12 -> RGB 用像素着色器在 GPU 完成，不做 CPU 转换。
//   * 悬浮状态条用 Direct2D/DirectWrite 叠加，与视频共用同一个 swapchain。
// =============================================================================
#pragma once

#include "../common/types.h"
#include <string>

#include <d3d11.h>
#include <dxgi1_2.h>

// 下面的私有成员用到了 ID2D1Factory / ID2D1Device / ID2D1DeviceContext /
// ID2D1Bitmap1 / IDWriteFactory，头文件必须自带这些声明；
// d2d1_1.h 会带进 d2d1.h（ID2D1Device 等 1.1 接口在这里）。
#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>

struct AVFrame;

namespace ms::client {

struct OverlayStats {
    double fps = 0;            // 接收帧率
    double kbps = 0;           // 接收码率
    double lossPercent = 0;    // 丢帧率
    double latencyMs = 0;      // 端到端估算（采集->呈现）
    int    queueDepth = 0;
    int    rttMs = 0;
    double decodeMs = 0;
    bool   hardwareDecode = false;
    std::string extra;         // 第二行附注
};

enum class OverlayCorner { TopLeft = 0, TopRight = 1, BottomLeft = 2, BottomRight = 3 };

// 画面缩放模式（RustDesk 式三档，工具栏可切）
enum class ScaleMode {
    Fit = 0,      // 智能缩放：等比缩放 + 黑边（默认，任何窗口都能看全）
    Stretch = 1,  // 拉伸填充：铺满窗口/屏幕，可能变形
    Actual = 2,   // 原始大小 1:1：像素精确，超出屏幕的部分裁掉（居中）
};

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // hwnd 为已创建的窗口。device 可为 nullptr（自己建），
    // 若解码器已建 D3D11 设备则传入以共享纹理。
    bool init(HWND hwnd, ID3D11Device* sharedDevice, bool vsync, std::string& err);
    void shutdown();
    bool is_open() const { return swapChain_ != nullptr; }

    void resize(int width, int height);

    // 呈现一帧。outPresentUs 回填 GPU 提交耗时。
    //  frame 可能是 AV_PIX_FMT_D3D11(硬解) 或 NV12/YUV420P(软解)
    bool present(AVFrame* frame, i64* outPresentUs);

    // 画悬浮状态条（在 present 之后、Present 之前调用由内部完成）
    void draw_overlay(const OverlayStats& s);
    void set_overlay_enabled(bool on) { overlayEnabled_ = on; }
    void set_overlay_corner(OverlayCorner c) { overlayCorner_ = c; }
    void set_vsync(bool on) { vsync_ = on; }
    // 缩放模式（Fit/Stretch/Actual），每帧 present 时生效
    void set_scale_mode(ScaleMode m) { scaleMode_ = m; }
    ScaleMode scale_mode() const { return scaleMode_; }

    // ---- 锐化（游戏画面细节）-----------------------------------------------
    // 0=关，0.35/0.7/1.2 = 低/中/高。着色器里对亮度做 4 邻域反锐化，
    // 色度不动（不会在彩色边缘出现红绿描边）。编码糊掉的字/远处目标靠它拉回来。
    void set_sharpen(float strength);
    float sharpen() const { return sharpenStrength_; }

    // ---- 远端光标 ----------------------------------------------------------
    // 位置/可见性来自被控端（UDP 小包）。visible=false 就不画 —— 游戏进内场时
    // 被控端系统光标本身是隐藏的，这里跟着隐藏，手感和本地玩一致。
    void set_cursor_pos(int x, int y, bool visible);
    // 形状（预乘 BGRA）。只在形状变化时被调用，内部懒建 D2D 位图。
    void set_cursor_shape(int width, int height, int hotX, int hotY, const Bytes& bgra);
    // 现在是否在画远端光标（客户端据此决定要不要藏起本机光标）
    bool cursor_ready() const { return cursorVisible_ && !cursorBgra_.empty(); }
    // 绝对鼠标映射用：是否允许画远端光标。
    // 绝对模式下本机光标是可见的（它就是"那个光标"），再画一个远端光标会重叠/打架。
    void set_cursor_draw_enabled(bool on) { cursorDrawEnabled_ = on; }

    // ---- 坐标映射（绝对鼠标 / 光标定位用）---------------------------------
    // 远端像素坐标 <-> 客户端窗口客户区坐标，和当前缩放模式（智能/拉伸/1:1）一致。
    //  outClient 是客户区坐标（左上角为 0,0）。
    bool video_to_client(int vx, int vy, POINT& outClient) const;
    //  clientPos 为客户区坐标；inside 表示该点是否落在实际视频画面内。
    bool client_to_video(POINT clientPos, int& vx, int& vy, bool& inside) const;

    // ---- v2 物理桌面坐标换算（绝对鼠标）------------------------------------
    // 远端视频像素坐标 -> 被控端「虚拟桌面物理像素」坐标（SetCursorPos 可直接用）。
    //   1. 视频 px -> 被采集显示器内的桌面 px（视频分辨率可能 != 桌面逻辑分辨率）
    //   2. 桌面逻辑 px -> 物理 px（被控端显示器 DPI 缩放，125%/150% 时必须换算）
    //   3. 加显示器原点（多显示器 / 负坐标）
    // DPI 信息（被控端缩放比）由被控端在 StreamReady 后另行下发目前不可行 ——
    // 这里用「客户端窗口全屏 = 显示器全屏」的常见布局无法推导远端 DPI，因此
    // 采样规则：视频分辨率就是被控端采集输出分辨率（GPU 缩放后的物理像素数），
    // 被控端注入端会用「显示器当前物理尺寸」做最终校验（见 input_inject）。
    // 返回 false 表示目标点落在画面外（黑边），调用方应丢弃本次事件。
    bool video_to_physical_desktop(int vx, int vy, int& outPhysX, int& outPhysY) const;
    // 被控端显示器物理像素尺寸（StreamReady/Reconfigured 后由会话层设置；
    // 与视频分辨率一致时（默认串流=原生）换算退化为纯平移）
    void set_desktop_pixels(int physW, int physH) { deskPhysW_ = physW; deskPhysH_ = physH; }
    // 被控端采集显示器在虚拟桌面里的原点（多显示器/负坐标；会话层从
    // StreamReady 扩展字段或对端协商结果里拿到，这里由会话层设置）
    void set_desktop_origin(int x, int y) { monitorOriginX_ = x; monitorOriginY_ = y; }

    // 全屏居中大字号提示（连接中/等首帧/防火墙提示/断线原因）。
    // 与 draw_overlay 互斥：同一时间只显示一个，由调用方按状态切换。
    void draw_centered_message(const std::wstring& text);
    void clear_message();

    // 轻量提示条（顶部居中小胶囊，几秒后自动消失）。
    // 用于"已切 TCP / 已切回 UDP"这类状态变化：只占一个很小的条，
    // 不遮挡画面中心，不打断游戏。
    void show_toast(const std::wstring& text, int durationMs = 3200);

    // 清屏（未连接时显示提示）
    void clear(float r, float g, float b);

    ID3D11Device* device() const { return device_; }
    int backbuffer_width() const { return bbWidth_; }
    int backbuffer_height() const { return bbHeight_; }

private:
    bool create_swapchain(HWND hwnd, std::string& err);
    bool create_pipeline(std::string& err);
    bool create_overlay(std::string& err);
    bool upload_cpu_frame(AVFrame* frame);
    bool acquire_hw_frame(AVFrame* frame);

    // 后缓冲视图（RTV + D2D target）：ResizeBuffers 之前必须全部释放
    bool ensure_backbuffer_views();
    void release_backbuffer_views();
    bool create_text_format();
    // 硬解中转：NV12 纹理 + 两个平面 SRV（Y=R8_UNORM，UV=R8G8_UNORM）
    bool ensure_nv12_texture(int width, int height);
    // 软解上传：两个动态平面纹理 + SRV
    bool ensure_sw_planes(int width, int height);
    void release_hw_texture();
    void release_sw_textures();
    // 硬解帧不是 NV12（P010/BGRA...）时：av_hwframe_transfer_data 回 CPU 再上传
    bool transfer_hw_frame(AVFrame* frame);
    void draw_overlay_now();
    void draw_message_now();
    // 轻量提示条（顶部居中，不挡画面）
    void draw_toast_now();
    // 远端光标：画在视频之上、悬浮条之上（光标永远是最后画的）
    void draw_cursor_now();
    void release_cursor_bitmap();
    // 视频区在客户区里的像素矩形（与着色器的 Fit/Stretch/Actual 规则一致）
    void video_rect(float& outX, float& outY, float& outW, float& outH) const;

    ID3D11Device*        device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    bool                 ownDevice_ = false;
    IDXGISwapChain1*     swapChain_ = nullptr;
    HWND                 hwnd_ = nullptr;

    ID3D11VertexShader*  vs_ = nullptr;
    ID3D11PixelShader*   ps_ = nullptr;
    ID3D11SamplerState*  sampler_ = nullptr;
    ID3D11RasterizerState* raster_ = nullptr;   // CULL_NONE：全屏三角形绕序是逆时针
    ID3D11Buffer*        cb_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;

    ID3D11Texture2D*     nv12Tex_ = nullptr;      // 硬解零拷贝中转目标（NV12）
    ID3D11ShaderResourceView* nv12SrvY_ = nullptr;
    ID3D11ShaderResourceView* nv12SrvUV_ = nullptr;
    // 软解上传目标。NV12 纹理在 D3D11 里不可 Map（video 格式无 CPU 访问），
    // 所以拆成 R8 + R8G8 两个动态纹理，数据布局与 NV12 的两个平面完全一致，
    // 采样时同样是 t0=Y / t1=UV，两条路径共用同一个着色器。
    ID3D11Texture2D*          swTexY_  = nullptr;
    ID3D11Texture2D*          swTexUV_ = nullptr;
    ID3D11ShaderResourceView* swSrvY_  = nullptr;
    ID3D11ShaderResourceView* swSrvUV_ = nullptr;

    UINT  texW_ = 0, texH_ = 0;                 // 软解平面纹理尺寸
    UINT  hwTexW_ = 0, hwTexH_ = 0;             // 硬解 NV12 中转纹理尺寸
    bool  hwCopyDisabled_ = false;              // NV12 中转建不出来时永久走 CPU
    bool  hwPath_ = false;                      // 本帧是否走的是硬解零拷贝
    float uvScaleX_ = 1.0f, uvScaleY_ = 1.0f;   // 逻辑尺寸 / 纹理尺寸
    AVFrame* swFrame_ = nullptr;                // 硬解回退 CPU 时的中转帧
    // 后缓冲身份：只用来判断 GetBuffer(0) 拿到的还是不是同一个表面
    // （flip model 下若表面身份会变，缓存视图就作废；这里不持有引用）
    ID3D11Texture2D* bbPtr_ = nullptr;

    // Direct2D 悬浮层
    ID2D1Factory*        d2dFactory_ = nullptr;
    ID2D1Device*         d2dDevice_ = nullptr;
    ID2D1DeviceContext*  d2dContext_ = nullptr;
    ID2D1Bitmap1*        d2dTarget_ = nullptr;
    IDWriteFactory*      dwrite_ = nullptr;
    IDWriteTextFormat*   textFormat_ = nullptr;
    ID2D1SolidColorBrush* bgBrush_ = nullptr;
    ID2D1SolidColorBrush* textBrush_ = nullptr;
    bool                 hasStats_ = false;     // 收到过统计才画悬浮条
    // 全屏提示文字（连接中/等首帧/防火墙/断线原因），hasMessage_ 时覆盖悬浮条
    bool                 hasMessage_ = false;
    std::wstring         message_;
    // 轻量提示条（切 TCP/UDP 等状态变化）
    std::wstring         toastText_;
    i64                  toastUntilUs_ = 0;
    IDWriteTextFormat*   toastFormat_ = nullptr;

    int  bbWidth_ = 0, bbHeight_ = 0;
    int  videoWidth_ = 0, videoHeight_ = 0;
    int  deskPhysW_ = 0, deskPhysH_ = 0;   // 被控端显示器物理像素（0=未知，用视频尺寸）
    int  monitorOriginX_ = 0, monitorOriginY_ = 0;  // 被采集显示器虚拟桌面原点
    bool vsync_ = false;
    // 呈现节流（Moonlight Pacer 的核心思路）：
    // 交换链带 FRAME_LATENCY_WAITABLE_OBJECT，最多允许 2 帧在途；
    // 每次渲染前等它"有空位"再画。好处是 Present(0) 不会因为队列满而阻塞
    // （阻塞在主线程上就是"鼠标一顿一顿"），同时把呈现节奏锁在显示器刷新上。
    HANDLE frameLatencyWaitable_ = nullptr;
    ScaleMode scaleMode_ = ScaleMode::Fit;   // 显示缩放模式（工具栏可切）
    float sharpenStrength_ = 0.0f;           // 锐化强度（0=关）
    bool overlayEnabled_ = true;
    bool tearingSupported_ = false;
    OverlayCorner overlayCorner_ = OverlayCorner::TopRight;
    OverlayStats  lastStats_;

    // ---- 远端光标状态 ----
    int   cursorX_ = 0, cursorY_ = 0;       // 远端桌面坐标（热点位置）
    bool  cursorVisible_ = false;
    int   cursorW_ = 0, cursorH_ = 0;
    int   cursorHotX_ = 0, cursorHotY_ = 0;
    Bytes cursorBgra_;                      // 预乘 alpha BGRA（形状）
    bool  cursorShapeDirty_ = false;
    bool  cursorDrawEnabled_ = true;        // 绝对模式下关掉（本机光标就是那个光标）
    ID2D1Bitmap* cursorBmp_ = nullptr;
};

} // namespace ms::client
