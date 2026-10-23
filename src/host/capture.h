// =============================================================================
//  capture.h - 屏幕采集（Windows DXGI Desktop Duplication）
//
//  被控端第一步：拿到桌面画面。走 GPU 路径，绝不回读 CPU 做色彩转换
//  （CPU 转 BGRA->NV12 在 1080p60 下要吃掉 1~2 个核）。
// =============================================================================
#pragma once

#include "../common/types.h"
#include <string>
#include <vector>

#include <d3d11.h>
#include <dxgi1_2.h>

namespace ms::host {

struct MonitorInfo {
    int         index = 0;
    std::string deviceName;    // \\.\DISPLAY1
    std::string friendlyName;  // 显卡输出描述
    int  width = 0;
    int  height = 0;
    int  left = 0;
    int  top = 0;
    bool isPrimary = false;
    bool isAttached = true;
};

// 一帧 NV12（系统内存，供 FFmpeg 软输入）
struct Nv12Frame {
    std::vector<u8> data;      // Y 平面 + UV 平面，紧凑排列
    int strideY  = 0;
    int strideUV = 0;
    int width    = 0;
    int height   = 0;
    i64 captureTsUs = 0;

    const u8* y()  const { return data.data(); }
    const u8* uv() const { return data.data() + size_t(strideY) * size_t(height); }
};

// 桌面光标。
// 【为什么必须单独取】Desktop Duplication 给出的桌面纹理里**没有光标**——
// 光标是系统在合成层单独画上去的。所以远控端如果只发视频，用户就看不到鼠标。
// 系统通过 IDXGIOutputDuplication::GetFramePointerShape/Position 把光标暴露出来，
// 而且很贴心地给了 Visible 标志：游戏进内场时自己把系统光标藏起来，这个标志
// 就变 FALSE；回菜单又变 TRUE。客户端照抄这个标志，就能天然区分
// 「游戏菜单（有光标）/ 游戏内场（无光标）」。
struct PointerInfo {
    bool available = false;   // 是否拿到过指针信息（拿不到就不要假装有光标）
    bool visible = false;     // 系统光标当前是否可见
    int  x = 0, y = 0;        // 相对被采集显示器左上角的像素坐标
    u32  shapeId = 0;         // 形状版本号（每次变化 +1；0 = 还没有形状）
    int  width = 0, height = 0;
    int  hotX = 0, hotY = 0;
    std::vector<u8> bgra;     // 预乘 alpha 的 BGRA，width*height*4
};

// BGRA(GPU) -> NV12 转换器
//   优先用 ID3D11VideoProcessor（硬件色彩空间转换 + 缩放）；
//   若设备不支持则回退到 CPU 逐像素转换（最近邻缩放）。
//   srcW×srcH 是采集到的原始尺寸，outW×outH 是要交给编码器的尺寸；
//   两者不同时 GPU 路径在 Blt 里顺带缩放，CPU 路径用最近邻采样。
class Nv12Converter {
public:
    bool init(ID3D11Device* dev, ID3D11DeviceContext* ctx,
              int srcWidth, int srcHeight,
              int outWidth, int outHeight, std::string& err);
    void shutdown();
    // 把 BGRA 纹理转成 NV12 并回读到 out。纹理释放由调用方负责。
    bool convert(ID3D11Texture2D* bgra, Nv12Frame& out, std::string& err);
    bool using_gpu_converter() const { return gpuPath_; }
    int  out_width()  const { return width_; }
    int  out_height() const { return height_; }

private:
    // 下面两个只在实现内部使用（CopyResource / Map / 建设备相关 staging）。
    // convert() 拿到的只有纹理指针，没有设备上下文，所以必须自己留一份引用。
    ID3D11Device*                dev_      = nullptr;
    ID3D11DeviceContext*         ctx_      = nullptr;
    ID3D11VideoDevice*           videoDev_  = nullptr;
    ID3D11VideoContext*          videoCtx_  = nullptr;
    ID3D11VideoProcessor*        vp_       = nullptr;
    ID3D11VideoProcessorEnumerator* vpEnum_ = nullptr;
    ID3D11Texture2D*             nv12Tex_  = nullptr;   // GPU 侧 NV12（输出尺寸）
    ID3D11Texture2D*             staging_  = nullptr;   // CPU 可读 staging（输出尺寸）
    // 仅当采集纹理自身的 bind flag 不满足视频处理器输入视图要求时才建的中转纹理
    // （懒创建，见 capture.cpp 里 create_input_view 的说明）
    ID3D11Texture2D*             bgraCopy_ = nullptr;
    int srcWidth_ = 0, srcHeight_ = 0;    // 采集原始尺寸（向上取偶）
    int width_ = 0, height_ = 0;          // 输出尺寸（向上取偶）
    bool gpuPath_ = false;
    bool initialized_ = false;
};

// -----------------------------------------------------------------------------
// Desktop Duplication 采集器
// -----------------------------------------------------------------------------
class ScreenCapture {
public:
    ScreenCapture();
    ~ScreenCapture();
    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    // monitorIndex 对应 enumerate_monitors() 的下标。
    //   outWidth/outHeight 传 0 表示跟随原生分辨率；
    //   非 0 时在 GPU 上缩放到该尺寸（编码器会按这个尺寸初始化）。
    bool init(int monitorIndex, bool captureCursor,
              int outWidth, int outHeight, std::string& err);
    void shutdown();
    bool is_initialized() const { return initialized_; }

    // 等待并取一帧。
    //  返回  1 = 拿到新帧（current_texture() 有效）
    //         0 = 超时（画面无变化，调用方可选择重复上一帧或跳过）
    //        <0 = 需要重新初始化（如分辨率切换 / ACCESS_LOST），next_error 说明原因
    int acquire_frame(int timeoutMs, std::string& next_error);

    // 释放上一帧（必须成对调用，否则 Desktop Duplication 会停止投递）
    void release_frame();

    // 把当前帧转成 NV12 系统内存。内部持有转换器缓存。
    // 注意：out 的尺寸是「输出尺寸」(width()/height())，可能已在 GPU 上缩放。
    bool to_nv12(Nv12Frame& out, std::string& err);

    // 让下一帧强制返回（用于已到帧率时间但桌面无变化时重复编码）
    bool force_next_frame() const { return false; }

    // 输出尺寸 = 交给编码器的尺寸（已含用户指定的缩放）
    int width()  const { return outWidth_; }
    int height() const { return outHeight_; }
    // 桌面原生尺寸
    int native_width()  const { return nativeWidth_; }
    int native_height() const { return nativeHeight_; }

    ID3D11Device*        device()  const { return device_; }
    ID3D11DeviceContext* context() const { return context_; }

    // 当前所有可用显示器
    static std::vector<MonitorInfo> enumerate_monitors();

    // 上次取帧是否包含光标更新
    bool last_had_cursor() const { return lastHadCursor_; }

    // 当前光标状态（位置/可见性/形状）。每帧 acquire_frame 成功后更新。
    const PointerInfo& pointer() const { return pointer_; }

private:
    bool create_duplication(std::string& err);
    void destroy_duplication();
    // 从本帧信息里更新光标位置/可见性/形状（内部做形状变化判断 + BGRA 转换）
    void update_pointer(const DXGI_OUTDUPL_FRAME_INFO& info);
    // 形状缓冲区 -> 预乘 BGRA（MONOCHROME / COLOR / MASKED_COLOR 统一处理）
    bool convert_pointer_shape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& si,
                              const u8* src, size_t srcLen);

    ID3D11Device*        device_   = nullptr;
    ID3D11DeviceContext* context_  = nullptr;
    IDXGIOutputDuplication* dup_   = nullptr;
    ID3D11Texture2D*     acquired_ = nullptr;   // 正在持有的帧
    Nv12Converter        converter_;

    int  monitorIndex_ = 0;      // 对 enumerate_monitors() 的下标（全局顺序）
    int  outputOrdinal_ = 0;     // 该显示器在“设备所属适配器”内的输出序号
                                 // （多显卡机器上可能与 monitorIndex_ 不同）
    int  nativeWidth_ = 0, nativeHeight_ = 0;   // 桌面尺寸
    int  outWidth_ = 0, outHeight_ = 0;         // 交给编码器的尺寸（含缩放）
    bool captureCursor_ = true;
    bool initialized_ = false;
    bool lastHadCursor_ = false;
    int  consecutiveTimeouts_ = 0;

    // ---- 光标 ----
    PointerInfo pointer_;
    // 被采集显示器在桌面坐标系里的原点（DXGI 的指针坐标是整桌面坐标，必须减掉它）
    int monitorLeft_ = 0, monitorTop_ = 0;
    // 形状版本号：只增不减（客户端靠它判断"形状变了，重建位图"）
    u32 shapeSeq_ = 0;
    std::vector<u8> shapeBuf_;   // GetFramePointerShape 的临时缓冲（复用）
    bool haveShape_ = false;     // 是否已经比对过上一份形状
    DXGI_OUTDUPL_POINTER_SHAPE_INFO lastShapeInfo_{};
    std::vector<u8> lastShapeBuf_;   // 上一份形状原始数据（判重用）
    size_t lastShapeBufLen_ = 0;
    i64    lastShapeTryUs_ = 0;      // 形状重试节流（还没拿到形状时用）
};

} // namespace ms::host
