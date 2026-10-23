// =============================================================================
//  capture.cpp - 屏幕采集实现（DXGI Desktop Duplication + GPU BGRA->NV12）
//
//  采集：IDXGIOutput1::DuplicateOutput（被控端唯一的零拷贝桌面源）。
//  转换：ID3D11VideoProcessor 在 GPU 上做 BGRA -> NV12（BT.709 limited range），
//        只有设备/驱动确实不支持时才回退到 CPU 逐像素转换
//        （1080p60 的 CPU 转换要吃掉 1~2 个核，回退只是"能用就行"）。
//
//  【帧生命周期】Desktop Duplication 的硬性约束：
//    AcquireNextFrame() 成功之后，必须在拿到帧的同一线程、同一 duplication 上
//    调用 ReleaseFrame()。漏掉一次 ReleaseFrame，后续 AcquireNextFrame 就会
//    返回 DXGI_ERROR_INVALID_CALL 或者永远只给同一帧 —— 表现就是"画面卡住"。
//    本文件保证 acquire_frame()/release_frame() 成对：acquire_frame() 进入时会
//    先把调用方忘记释放的帧补释放掉（防御性处理），shutdown() 也会先归还帧再
//    释放 duplication。
// =============================================================================
#include "capture.h"

#include "../common/clock.h"
#include "../common/log.h"

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ms::host {
namespace {

// -----------------------------------------------------------------------------
// 小工具
// -----------------------------------------------------------------------------

// HRESULT -> "0x887A0004" 形式，拼错误串用
std::string hr_text(HRESULT hr) {
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "0x%08lX",
                  static_cast<unsigned long>(static_cast<u32>(hr)));
    return std::string(buf);
}

// WCHAR -> UTF-8（DeviceName 是 WCHAR[32]，Description 是 WCHAR[128]）
std::string wide_to_utf8(const wchar_t* w) {
    if (w == nullptr || w[0] == L'\0') return std::string();
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return std::string();
    std::string s(static_cast<size_t>(need), '\0');
    const int got = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], need, nullptr, nullptr);
    if (got <= 0) return std::string();
    s.resize(static_cast<size_t>(got - 1));   // 去掉结尾的 '\0'
    return s;
}

// 该输出的桌面矩形是不是主显示器。
// 优先问 user32（MONITORINFOF_PRIMARY 最可靠），拿不到 HMONITOR 时退回原点判断。
bool is_primary_output(const RECT& rc) {
    const POINT center{ (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2 };
    HMONITOR mon = ::MonitorFromPoint(center, MONITOR_DEFAULTTONULL);
    if (mon != nullptr) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (::GetMonitorInfoW(mon, &mi) != FALSE) {
            return (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        }
    }
    return rc.left == 0 && rc.top == 0;
}

// DuplicateOutput 常见失败的补充说明（写日志时定位用）
const char* describe_dup_hr(HRESULT hr) {
    switch (hr) {
    case DXGI_ERROR_UNSUPPORTED:
        return "（不支持：显卡/驱动不支持 Desktop Duplication，或运行在不支持的会话里）";
    case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE:
        return "（不可用：本进程/本输出上已有的 duplication 太多，或处于远程桌面会话）";
    case E_ACCESSDENIED:
        return "（拒绝访问：安全桌面、受保护内容或权限不足）";
    case DXGI_ERROR_INVALID_CALL:
        return "（参数无效：D3D11 设备不在驱动该输出的适配器上）";
    default:
        return "";
    }
}

// -----------------------------------------------------------------------------
// DXGI 输出定位
// -----------------------------------------------------------------------------

// 一次输出定位的结果
struct OutputTarget {
    IDXGIOutput1* output = nullptr;   // 调用方负责 Release
    int  width  = 0;
    int  height = 0;
    int  left   = 0;      // 桌面坐标系里的原点（多显示器时不是 0）
    int  top    = 0;
    bool attached = false;
};

// 在指定适配器上定位"第 index 个输出"（index 就是 enumerate_monitors() 的下标来源：
// 适配器顺序 x 适配器内输出顺序，同一个枚举顺序保证两边对得上）。
bool locate_output(IDXGIAdapter* adapter, int index, OutputTarget& target, std::string& err) {
    target.output   = nullptr;
    target.width    = 0;
    target.height   = 0;
    target.left     = 0;
    target.top      = 0;
    target.attached = false;

    if (adapter == nullptr) { err = "DXGI 适配器为空"; return false; }
    if (index < 0) { err = "显示器索引不能为负"; return false; }

    for (UINT i = 0; ; ++i) {
        IDXGIOutput* out = nullptr;
        const HRESULT hr = adapter->EnumOutputs(i, &out);
        if (hr == DXGI_ERROR_NOT_FOUND) break;          // 该适配器输出枚举完毕
        if (FAILED(hr)) { if (out != nullptr) out->Release(); err = "IDXGIAdapter::EnumOutputs 失败, hr=" + hr_text(hr); return false; }
        if (out == nullptr) continue;                   // 防御：理论上不会发生

        if (static_cast<int>(i) == index) {
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(out->GetDesc(&desc))) {
                out->Release();
                err = "IDXGIOutput::GetDesc 失败";
                return false;
            }
            target.width    = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
            target.height   = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
            target.left     = desc.DesktopCoordinates.left;
            target.top      = desc.DesktopCoordinates.top;
            target.attached = (desc.AttachedToDesktop != FALSE);
            const HRESULT qhr = out->QueryInterface(__uuidof(IDXGIOutput1),
                                                    reinterpret_cast<void**>(&target.output));
            out->Release();
            if (FAILED(qhr) || target.output == nullptr) {
                target.output = nullptr;
                err = "IDXGIOutput1 不可用（系统过旧）";
                return false;
            }
            return true;
        }
        out->Release();
    }
    err = "显示器索引越界";
    return false;
}

// 按 enumerate_monitors() 的全局顺序找到第 index 个输出所在的适配器，
// 并给出它在该适配器内的输出序号。
// 返回的适配器是 AddRef 过的，调用方负责 Release。
// 之所以需要它：D3D11 设备必须建在"驱动该显示器的适配器"上，否则
// DuplicateOutput 会失败（核显 + 独显的机器上默认适配器往往不是它）。
bool find_adapter_by_global_index(int index, IDXGIAdapter1** adapter, int* ordinal,
                                 std::string& err) {
    *adapter = nullptr;
    *ordinal = 0;
    if (index < 0) { err = "显示器索引不能为负"; return false; }

    IDXGIFactory1* factory = nullptr;
    HRESULT hr = ::CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || factory == nullptr) {
        err = "CreateDXGIFactory1 失败, hr=" + hr_text(hr);
        return false;
    }

    int global = 0;
    bool found = false;
    for (UINT a = 0; !found; ++a) {
        IDXGIAdapter1* ad = nullptr;
        const HRESULT ahr = factory->EnumAdapters1(a, &ad);
        if (ahr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(ahr) || ad == nullptr) break;

        // 与 enumerate_monitors() 保持完全一致的过滤条件，否则下标会错位
        DXGI_ADAPTER_DESC1 adesc{};
        if (FAILED(ad->GetDesc1(&adesc))) { ad->Release(); continue; }

        for (UINT o = 0; ; ++o) {
            IDXGIOutput* out = nullptr;
            const HRESULT ohr = ad->EnumOutputs(o, &out);
            if (ohr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(ohr) || out == nullptr) break;
            DXGI_OUTPUT_DESC desc{};
            const bool descOk = SUCCEEDED(out->GetDesc(&desc));
            out->Release();
            if (!descOk) continue;                      // 与 enumerate_monitors 一致：无效输出不计数
            if (global == index) {
                *adapter = ad;                          // 移交引用，不 Release
                *ordinal = static_cast<int>(o);
                found = true;
                break;
            }
            ++global;
        }
        if (!found) ad->Release();
    }

    factory->Release();
    if (!found) { err = "显示器索引越界"; return false; }
    return true;
}

// 创建设备：优先显式绑定适配器（adapter != nullptr 时必须用 DRIVER_TYPE_UNKNOWN），
// adapter == nullptr 时用系统默认适配器的硬件驱动。
// feature level 从 11_0 逐个降到 10_0（Desktop Duplication 至少需要 10_0 级别）。
bool create_d3d11_device(IDXGIAdapter* adapter, ID3D11Device** dev, ID3D11DeviceContext** ctx,
                         std::string& err) {
    *dev = nullptr;
    *ctx = nullptr;

    static const D3D_FEATURE_LEVEL kLevels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    // D3D11_CREATE_DEVICE_BGRA_SUPPORT 是硬性要求：没有它 DuplicateOutput 直接失败
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_DRIVER_TYPE driverType = (adapter != nullptr) ? D3D_DRIVER_TYPE_UNKNOWN
                                                            : D3D_DRIVER_TYPE_HARDWARE;

    HRESULT hr = E_FAIL;
    for (const D3D_FEATURE_LEVEL level : kLevels) {
        const D3D_FEATURE_LEVEL want[1] = { level };
        D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_10_0;
        hr = ::D3D11CreateDevice(adapter, driverType, nullptr, flags, want, 1,
                                 D3D11_SDK_VERSION, dev, &got, ctx);
        if (SUCCEEDED(hr) && *dev != nullptr && *ctx != nullptr) return true;
        // 失败时清掉半成品，继续尝试下一个 feature level
        if (*ctx != nullptr) { (*ctx)->Release(); *ctx = nullptr; }
        if (*dev != nullptr) { (*dev)->Release(); *dev = nullptr; }
    }
    err = "D3D11CreateDevice 失败（需要支持 D3D11 的显卡）, hr=" + hr_text(hr);
    return false;
}

// 取出设备所属适配器（AddRef 过的，调用方负责 Release）
IDXGIAdapter* adapter_of_device(ID3D11Device* dev) {
    if (dev == nullptr) return nullptr;
    IDXGIDevice* dxgiDev = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev))) ||
        dxgiDev == nullptr) {
        return nullptr;
    }
    IDXGIAdapter* adapter = nullptr;
    const HRESULT hr = dxgiDev->GetAdapter(&adapter);
    dxgiDev->Release();
    if (FAILED(hr)) return nullptr;
    return adapter;
}

// -----------------------------------------------------------------------------
// NV12 相关
// -----------------------------------------------------------------------------

inline u8 clamp_u8(int v) {
    if (v < 0) return static_cast<u8>(0);
    if (v > 255) return static_cast<u8>(255);
    return static_cast<u8>(v);
}

// BT.709 limited range（Y:16-235, UV:16-240）
//   Kr=0.2126 / Kb=0.0722 -> Cb=(B-Y)/1.8556, Cr=(R-Y)/1.5748
//   CPU 回退路径，标量浮点实现，正确性优先。
inline void rgb_to_yuv709(int r, int g, int b, int& y, int& u, int& v) {
    const float R = static_cast<float>(r);
    const float G = static_cast<float>(g);
    const float B = static_cast<float>(b);
    const float Y  = 0.2126f * R + 0.7152f * G + 0.0722f * B;
    const float Cb = (B - Y) / 1.8556f;
    const float Cr = (R - Y) / 1.5748f;
    y = static_cast<int>(Y  * (219.0f / 255.0f) + 16.0f  + 0.5f);
    u = static_cast<int>(Cb * (224.0f / 255.0f) + 128.0f + 0.5f);
    v = static_cast<int>(Cr * (224.0f / 255.0f) + 128.0f + 0.5f);
}

// 建 NV12 渲染色纹理 + CPU 可读 staging 纹理。
// 两张纹理除了 Usage/BindFlags/CPUAccessFlags 必须完全一致，否则 CopyResource 会失败。
bool create_nv12_textures(ID3D11Device* dev, int w, int h,
                          ID3D11Texture2D** nv12Tex, ID3D11Texture2D** staging,
                          std::string& err) {
    *nv12Tex = nullptr;
    *staging = nullptr;

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = static_cast<UINT>(w);
    td.Height           = static_cast<UINT>(h);
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET;   // 作为 VideoProcessor 的输出目标
    td.CPUAccessFlags   = 0;
    td.MiscFlags        = 0;
    HRESULT hr = dev->CreateTexture2D(&td, nullptr, nv12Tex);
    if (FAILED(hr) || *nv12Tex == nullptr) {
        err = "创建 NV12 纹理失败, hr=" + hr_text(hr);
        return false;
    }

    // staging：回读用（Map(D3D11_MAP_READ)）；MiscFlags 必须与上面一致
    td.Usage          = D3D11_USAGE_STAGING;
    td.BindFlags      = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = dev->CreateTexture2D(&td, nullptr, staging);
    if (FAILED(hr) || *staging == nullptr) {
        (*nv12Tex)->Release();
        *nv12Tex = nullptr;
        err = "创建 NV12 staging 纹理失败, hr=" + hr_text(hr);
        return false;
    }
    return true;
}

// 创建 VideoDevice / VideoContext / 枚举器 / 处理器，并确认 BGRA 输入 + NV12 输出被支持
//   srcW/srcH = 采集到的原始尺寸
//   outW/outH = 编码输出尺寸
// 两者不同时，缩放由 VideoProcessorBlt 在 GPU 上顺带完成，几乎不增加成本。
bool create_video_processor(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                            int srcW, int srcH, int outW, int outH,
                            ID3D11VideoDevice** vdev, ID3D11VideoContext** vctx,
                            ID3D11VideoProcessorEnumerator** vpe, ID3D11VideoProcessor** vp,
                            std::string& err) {
    *vdev = nullptr;
    *vctx = nullptr;
    *vpe  = nullptr;
    *vp   = nullptr;

    HRESULT hr = dev->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(vdev));
    if (FAILED(hr) || *vdev == nullptr) {
        err = "设备不支持 ID3D11VideoDevice";
        return false;
    }
    // VideoContext 是 immediate context 上的另一个接口（同一个对象，多一份引用）
    hr = ctx->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void**>(vctx));
    if (FAILED(hr) || *vctx == nullptr) {
        err = "设备上下文不支持 ID3D11VideoContext";
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat              = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate.Numerator      = 60;
    cd.InputFrameRate.Denominator    = 1;
    cd.InputWidth                    = static_cast<UINT>(srcW);
    cd.InputHeight                   = static_cast<UINT>(srcH);
    cd.OutputFrameRate.Numerator     = 60;
    cd.OutputFrameRate.Denominator   = 1;
    cd.OutputWidth                   = static_cast<UINT>(outW);
    cd.OutputHeight                  = static_cast<UINT>(outH);
    cd.Usage                         = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = (*vdev)->CreateVideoProcessorEnumerator(&cd, vpe);
    if (FAILED(hr) || *vpe == nullptr) {
        err = "CreateVideoProcessorEnumerator 失败, hr=" + hr_text(hr);
        return false;
    }
    hr = (*vdev)->CreateVideoProcessor(*vpe, 0, vp);
    if (FAILED(hr) || *vp == nullptr) {
        err = "CreateVideoProcessor 失败, hr=" + hr_text(hr);
        return false;
    }

    UINT support = 0;
    hr = (*vpe)->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &support);
    if (FAILED(hr) || (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0) {
        err = "视频处理器不支持 BGRA 输入";
        return false;
    }
    support = 0;
    hr = (*vpe)->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support);
    if (FAILED(hr) || (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
        err = "视频处理器不支持 NV12 输出";
        return false;
    }

    // 输出色彩空间必须显式指定：驱动的默认矩阵是 BT.601，会和 CPU 回退路径的
    // BT.709 不一致（颜色偏），而且 H.264 里 >=720p 通常按 BT.709 解释。
    // 输入侧保持默认（full-range RGB），正是桌面 BGRA 的实际情况。
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs{};
    outCs.YCbCr_Matrix   = 1;   // 1 = BT.709
    outCs.Nominal_Range  = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;   // limited range
    (*vctx)->VideoProcessorSetOutputColorSpace(*vp, &outCs);
    return true;
}

// 把 Map 出来的 NV12 staging 拷进紧凑布局的 Nv12Frame。
//
// 【关键陷阱】MappedSubresource.RowPitch 描述的是"平面行宽"，Y 平面和 UV 平面
//   行宽都是它；但驱动不会额外告诉你 UV 平面从哪儿开始，只能按
//     UV 起始偏移 = RowPitch * 纹理高度
//   来算。这里高度再做一次向上取偶的保守处理（我们建纹理时宽高已经取偶，所以
//   实际上是同一个值），因为 Y 平面分配的行数不会小于纹理高度。
//   UV 平面每行同样是 RowPitch 字节，但只有 width 字节有效（width 字节 = width/2
//   组 UV，正好覆盖一行像素）。
//   最后用 DepthPitch 做一次自检：它（驱动给出的整个 2D slice 大小）必须装得下
//   Y + UV 两个平面，否则说明排布假设不成立 —— 这时宁可让 GPU 回读失败，交给
//   调用方走 CPU 路径，也不要读越界。
bool copy_nv12_from_mapped(const D3D11_MAPPED_SUBRESOURCE& mapped, int w, int h,
                           Nv12Frame& out, std::string& err) {
    const size_t rowPitch = static_cast<size_t>(mapped.RowPitch);
    if (mapped.pData == nullptr) { err = "NV12 staging 映射指针为空"; return false; }
    if (rowPitch < static_cast<size_t>(w)) {
        err = "NV12 staging RowPitch 小于宽度，布局不符合预期";
        return false;
    }

    const size_t alignedH = (static_cast<size_t>(h) + 1u) & ~static_cast<size_t>(1);  // 保守取偶
    const size_t uvOffset = rowPitch * alignedH;
    const size_t uvBytes  = rowPitch * (alignedH / 2);
    if (mapped.DepthPitch != 0 && static_cast<size_t>(mapped.DepthPitch) < uvOffset + uvBytes) {
        err = "NV12 staging 的 DepthPitch 装不下 Y+UV，UV 偏移假设不成立";
        return false;
    }
    if (out.data.size() < static_cast<size_t>(out.strideY) * static_cast<size_t>(h) +
                           static_cast<size_t>(out.strideUV) * static_cast<size_t>(h / 2)) {
        err = "Nv12Frame 缓冲区过小";
        return false;
    }

    const u8* src = static_cast<const u8*>(mapped.pData);
    u8* dst = out.data.data();
    // Y 平面：每行 width 字节有效
    for (int y = 0; y < h; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * static_cast<size_t>(out.strideY),
                    src + static_cast<size_t>(y) * rowPitch,
                    static_cast<size_t>(w));
    }
    // UV 平面：起始 = RowPitch * 高度，行内 width 字节（U,V 交错）
    u8* dstUV = dst + static_cast<size_t>(out.strideY) * static_cast<size_t>(h);
    const u8* srcUV = src + uvOffset;
    const int uvRows = h / 2;
    for (int y = 0; y < uvRows; ++y) {
        std::memcpy(dstUV + static_cast<size_t>(y) * static_cast<size_t>(out.strideUV),
                    srcUV + static_cast<size_t>(y) * rowPitch,
                    static_cast<size_t>(w));
    }
    return true;
}

// 建立视频处理器的输入视图（BGRA）。
//
// 文档要求：用作 VideoProcessor 输入的资源必须带 DECODER / VIDEO_ENCODER /
// RENDER_TARGET / UNORDERED_ACCESS_VIEW 之一（或 bind flags 恰好为 0）。
// Desktop Duplication 返回的纹理 bind flags 由驱动决定，不能保证满足，
// 所以先直接对它建视图；失败时把画面拷进一张我们自己建的、带 RENDER_TARGET
// 的 BGRA 纹理再建视图（一次 GPU 拷贝，1080p 约 0.1ms）。这样"驱动不给面子"
// 不会导致整个 GPU 路径永久退化成 CPU 转换。
bool create_input_view(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11VideoDevice* vdev,
                       ID3D11VideoProcessorEnumerator* vpe, ID3D11Texture2D* bgra,
                       ID3D11Texture2D** copyCache, ID3D11VideoProcessorInputView** outView,
                       std::string& err) {
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.FourCC               = 0;
    ivd.ViewDimension        = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.MipSlice   = 0;
    ivd.Texture2D.ArraySlice = 0;   // Desktop Duplication 给的是单 slice 纹理

    HRESULT hr = vdev->CreateVideoProcessorInputView(bgra, vpe, &ivd, outView);
    if (SUCCEEDED(hr) && *outView != nullptr) return true;
    *outView = nullptr;

    D3D11_TEXTURE2D_DESC src{};
    bgra->GetDesc(&src);
    if (src.MipLevels != 1 || src.ArraySize != 1 || src.SampleDesc.Count != 1) {
        err = "采集纹理不是单 mip/单 slice/非多重采样，无法建立视频处理器输入视图";
        return false;
    }

    // 缓存的中转纹理尺寸不对就重建
    if (*copyCache != nullptr) {
        D3D11_TEXTURE2D_DESC cached{};
        (*copyCache)->GetDesc(&cached);
        if (cached.Width != src.Width || cached.Height != src.Height ||
            cached.Format != src.Format) {
            (*copyCache)->Release();
            *copyCache = nullptr;
        }
    }
    if (*copyCache == nullptr) {
        D3D11_TEXTURE2D_DESC cd{};
        cd.Width            = src.Width;
        cd.Height           = src.Height;
        cd.MipLevels        = 1;
        cd.ArraySize        = 1;
        cd.Format           = src.Format;
        cd.SampleDesc.Count = 1;
        cd.SampleDesc.Quality = 0;
        cd.Usage            = D3D11_USAGE_DEFAULT;
        cd.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        cd.CPUAccessFlags   = 0;
        cd.MiscFlags        = 0;
        hr = dev->CreateTexture2D(&cd, nullptr, copyCache);
        if (FAILED(hr) || *copyCache == nullptr) {
            err = "创建 BGRA 中转纹理失败, hr=" + hr_text(hr);
            return false;
        }
    }

    ctx->CopyResource(*copyCache, bgra);
    hr = vdev->CreateVideoProcessorInputView(*copyCache, vpe, &ivd, outView);
    if (FAILED(hr) || *outView == nullptr) {
        *outView = nullptr;
        err = "CreateVideoProcessorInputView 失败, hr=" + hr_text(hr);
        return false;
    }
    return true;
}

// GPU 路径：VideoProcessorBlt(BGRA -> NV12) + CopyResource 到 staging + Map 回读
bool gpu_convert(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11VideoDevice* vdev,
                 ID3D11VideoContext* vctx, ID3D11VideoProcessor* vp,
                 ID3D11VideoProcessorEnumerator* vpe, ID3D11Texture2D* nv12Tex,
                 ID3D11Texture2D* staging, ID3D11Texture2D* bgra,
                 ID3D11Texture2D** bgraCopyCache, int w, int h, Nv12Frame& out,
                 std::string& err) {
    ID3D11VideoProcessorInputView* inView = nullptr;
    if (!create_input_view(dev, ctx, vdev, vpe, bgra, bgraCopyCache, &inView, err)) {
        return false;
    }
    HRESULT hr = S_OK;

    // 输出视图：NV12 纹理，同样是 ArraySlice = 0（Tex2D 视图即 slice 0）
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension        = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ovd.Texture2D.MipSlice   = 0;
    ID3D11VideoProcessorOutputView* outView = nullptr;
    hr = vdev->CreateVideoProcessorOutputView(nv12Tex, vpe, &ovd, &outView);
    if (FAILED(hr) || outView == nullptr) {
        inView->Release();
        err = "CreateVideoProcessorOutputView 失败, hr=" + hr_text(hr);
        return false;
    }

    // 渐进帧 + 单输入流（Blt 内部完成色彩空间转换，必要时顺带缩放）
    vctx->VideoProcessorSetStreamFrameFormat(vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable            = TRUE;
    stream.OutputIndex       = 0;
    stream.InputFrameOrField = 0;
    stream.PastFrames        = 0;
    stream.FutureFrames      = 0;
    stream.pInputSurface     = inView;
    hr = vctx->VideoProcessorBlt(vp, outView, 0, 1, &stream);

    outView->Release();
    inView->Release();
    if (FAILED(hr)) {
        err = "VideoProcessorBlt 失败, hr=" + hr_text(hr);
        return false;
    }

    // 回读：CopyResource 与 Blt 在同一个 immediate context 上，顺序由 D3D11 保证；
    // Map(READ) 会阻塞到 GPU 拷完（staging 是 CPU 可读的，不能直接 Map GPU 纹理）。
    ctx->CopyResource(staging, nv12Tex);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        err = "Map(NV12 staging) 失败, hr=" + hr_text(hr);
        return false;
    }
    const bool ok = copy_nv12_from_mapped(mapped, w, h, out, err);
    ctx->Unmap(staging, 0);
    return ok;
}

// 取一个 BGRA 像素（坐标钳到边界，保证奇数尺寸的桌面也能走完整循环）
inline void fetch_bgra(const u8* base, size_t pitch, int x, int y, int clampW, int clampH,
                       int& r, int& g, int& b) {
    if (x >= clampW) x = clampW - 1;
    if (y >= clampH) y = clampH - 1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    const u8* p = base + static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * 4u;
    b = p[0];   // 内存序是 BGRA
    g = p[1];
    r = p[2];
}

// CPU 回退路径：BGRA staging -> 逐像素 BT.709 limited range -> NV12
bool cpu_convert(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* bgra,
                 int w, int h, Nv12Frame& out, std::string& err) {
    // Desktop Duplication 的纹理没有 CPU 访问权限，必须先拷到临时 staging 再 Map。
    // 临时纹理按输入纹理的真实描述建（尺寸/格式必须完全一致才能 CopyResource）。
    D3D11_TEXTURE2D_DESC srcDesc{};
    bgra->GetDesc(&srcDesc);
    D3D11_TEXTURE2D_DESC sd = srcDesc;
    sd.Usage          = D3D11_USAGE_STAGING;
    sd.BindFlags      = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags      = 0;

    ID3D11Texture2D* staging = nullptr;
    HRESULT hr = dev->CreateTexture2D(&sd, nullptr, &staging);
    if (FAILED(hr) || staging == nullptr) {
        err = "创建 BGRA staging 纹理失败, hr=" + hr_text(hr);
        return false;
    }
    ctx->CopyResource(staging, bgra);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr) || mapped.pData == nullptr) {
        staging->Release();
        err = "Map(BGRA staging) 失败, hr=" + hr_text(hr);
        return false;
    }

    const u8* src = static_cast<const u8*>(mapped.pData);
    const size_t srcPitch = static_cast<size_t>(mapped.RowPitch);
    // 采样源按输入纹理的真实尺寸；输出尺寸可能与之不同（分辨率缩放），
    // 用最近邻采样把 (x,y) 映射回源坐标。
    const int srcFullW = static_cast<int>(srcDesc.Width);
    const int srcFullH = static_cast<int>(srcDesc.Height);

    bool ok = true;
    if (srcPitch < static_cast<size_t>(srcFullW) * 4u || srcFullW <= 0 || srcFullH <= 0) {
        err = "BGRA staging 行宽异常";
        ok = false;
    } else {
        u8* dst = out.data.data();
        // 亮度：逐像素
        for (int y = 0; y < h; ++y) {
            u8* dstRow = dst + static_cast<size_t>(y) * static_cast<size_t>(out.strideY);
            const int sy = static_cast<int>(static_cast<int64_t>(y) * srcFullH / h);
            for (int x = 0; x < w; ++x) {
                int r = 0, g = 0, b = 0, yv = 0, u = 0, v = 0;
                const int sx = static_cast<int>(static_cast<int64_t>(x) * srcFullW / w);
                fetch_bgra(src, srcPitch, sx, sy, srcFullW, srcFullH, r, g, b);
                rgb_to_yuv709(r, g, b, yv, u, v);
                dstRow[x] = clamp_u8(yv);
            }
        }
        // 色度：每个 2x2 块取 RGB 平均后算一组 U/V，按 (U,V) 交错写入 UV 平面
        u8* dstUV = dst + static_cast<size_t>(out.strideY) * static_cast<size_t>(h);
        for (int y = 0; y + 1 < h; y += 2) {
            u8* uvRow = dstUV + static_cast<size_t>(y / 2) * static_cast<size_t>(out.strideUV);
            for (int x = 0; x + 1 < w; x += 2) {
                int sr = 0, sg = 0, sb = 0;
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        int r = 0, g = 0, b = 0;
                        const int sx = static_cast<int>(static_cast<int64_t>(x + dx) * srcFullW / w);
                        const int sy = static_cast<int>(static_cast<int64_t>(y + dy) * srcFullH / h);
                        fetch_bgra(src, srcPitch, sx, sy, srcFullW, srcFullH, r, g, b);
                        sr += r; sg += g; sb += b;
                    }
                }
                int yv = 0, u = 0, v = 0;
                rgb_to_yuv709(sr / 4, sg / 4, sb / 4, yv, u, v);
                uvRow[x]     = clamp_u8(u);
                uvRow[x + 1] = clamp_u8(v);
            }
        }
    }

    ctx->Unmap(staging, 0);
    staging->Release();
    return ok;
}

} // namespace

// =============================================================================
//  Nv12Converter
// =============================================================================

bool Nv12Converter::init(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                         int srcWidth, int srcHeight,
                         int outWidth, int outHeight, std::string& err) {
    err.clear();
    shutdown();   // 幂等：允许重复 init
    if (dev == nullptr || ctx == nullptr) { err = "Nv12Converter::init: 设备为空"; return false; }
    if (srcWidth <= 0 || srcHeight <= 0) { err = "Nv12Converter::init: 采集尺寸无效"; return false; }
    if (outWidth <= 0 || outHeight <= 0) { err = "Nv12Converter::init: 输出尺寸无效"; return false; }

    // NV12 要求宽高都是偶数：向上取偶。正常桌面分辨率本来就是偶数，这行是兜底。
    srcWidth_  = (srcWidth  + 1) & ~1;
    srcHeight_ = (srcHeight + 1) & ~1;
    width_     = (outWidth  + 1) & ~1;
    height_    = (outHeight + 1) & ~1;

    dev_ = dev; dev_->AddRef();   // 转换器自己持一份引用，shutdown 时对称释放
    ctx_ = ctx; ctx_->AddRef();

    // --- 先试 GPU 路径；任何一步失败都只关掉 gpuPath_，不影响 CPU 回退 ---
    ID3D11Texture2D* nv12Tex = nullptr;
    ID3D11Texture2D* staging = nullptr;
    ID3D11VideoDevice* vdev = nullptr;
    ID3D11VideoContext* vctx = nullptr;
    ID3D11VideoProcessorEnumerator* vpe = nullptr;
    ID3D11VideoProcessor* vp = nullptr;

    std::string reason;
    bool gpuOk = create_nv12_textures(dev, width_, height_, &nv12Tex, &staging, reason);
    if (gpuOk) {
        std::string vpErr;
        gpuOk = create_video_processor(dev, ctx, srcWidth_, srcHeight_, width_, height_,
                                       &vdev, &vctx, &vpe, &vp, vpErr);
        if (!gpuOk) reason = vpErr;
    }

    if (gpuOk) {
        nv12Tex_ = nv12Tex;
        staging_ = staging;
        videoDev_ = vdev;
        videoCtx_ = vctx;
        vpEnum_   = vpe;
        vp_       = vp;
        gpuPath_  = true;
    } else {
        if (nv12Tex != nullptr) nv12Tex->Release();
        if (staging != nullptr) staging->Release();
        if (vp      != nullptr) vp->Release();
        if (vpe     != nullptr) vpe->Release();
        if (vctx    != nullptr) vctx->Release();
        if (vdev    != nullptr) vdev->Release();
        gpuPath_ = false;
        // CPU 回退始终可用，所以 init 仍然算成功；这里只提示性能损失
        MS_WARN("Nv12Converter: GPU 色彩转换不可用(%s)，回退 CPU 逐像素转换", reason.c_str());
    }

    initialized_ = true;
    MS_INFO("Nv12Converter 初始化完成: 采集 %dx%d -> 输出 %dx%d, %s",
            srcWidth_, srcHeight_, width_, height_,
            gpuPath_ ? "GPU(ID3D11VideoProcessor)" : "CPU(回退)");
    return true;
}

void Nv12Converter::shutdown() {
    // 逆序释放；全部置空，重复调用安全
    if (vp_ != nullptr)       { vp_->Release();       vp_ = nullptr; }
    if (vpEnum_ != nullptr)   { vpEnum_->Release();   vpEnum_ = nullptr; }
    if (videoCtx_ != nullptr) { videoCtx_->Release(); videoCtx_ = nullptr; }
    if (videoDev_ != nullptr) { videoDev_->Release(); videoDev_ = nullptr; }
    if (staging_ != nullptr)  { staging_->Release();  staging_ = nullptr; }
    if (bgraCopy_ != nullptr) { bgraCopy_->Release(); bgraCopy_ = nullptr; }
    if (nv12Tex_ != nullptr)  { nv12Tex_->Release();  nv12Tex_ = nullptr; }
    if (ctx_ != nullptr)      { ctx_->Release();      ctx_ = nullptr; }
    if (dev_ != nullptr)      { dev_->Release();      dev_ = nullptr; }

    gpuPath_ = false;
    initialized_ = false;
    width_ = 0;
    height_ = 0;
    srcWidth_ = 0;
    srcHeight_ = 0;
}

bool Nv12Converter::convert(ID3D11Texture2D* bgra, Nv12Frame& out, std::string& err) {
    err.clear();
    if (!initialized_) { err = "Nv12Converter 未初始化"; return false; }
    if (bgra == nullptr) { err = "Nv12Converter::convert: 输入纹理为空"; return false; }

    D3D11_TEXTURE2D_DESC srcDesc{};
    bgra->GetDesc(&srcDesc);

    // 输出布局（紧凑 NV12）：
    //   strideY  = 宽度向上取偶
    //   strideUV = strideY（UV 交错，一行 width 字节有效）
    //   Y  占 strideY * height
    //   UV 占 strideY * (height / 2)
    // out.captureTsUs 由调用方填，这里不碰。
    out.width    = width_;
    out.height   = height_;
    out.strideY  = width_;
    out.strideUV = width_;
    const size_t yBytes  = static_cast<size_t>(out.strideY) * static_cast<size_t>(out.height);
    const size_t uvBytes = static_cast<size_t>(out.strideUV) * static_cast<size_t>(out.height / 2);
    out.data.assign(yBytes + uvBytes, static_cast<u8>(0));

    // Desktop Duplication 只给 BGRA8；格式不是它就别走视频处理器（输入视图会失败）
    if (gpuPath_ && nv12Tex_ != nullptr && staging_ != nullptr &&
        videoDev_ != nullptr && videoCtx_ != nullptr && vp_ != nullptr && vpEnum_ != nullptr &&
        ctx_ != nullptr && srcDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        std::string gpuErr;
        if (gpu_convert(dev_, ctx_, videoDev_, videoCtx_, vp_, vpEnum_, nv12Tex_, staging_, bgra,
                        &bgraCopy_, width_, height_, out, gpuErr)) {
            return true;
        }
        // 运行期失败（驱动/尺寸不匹配等）：永久关掉 GPU 路径，后面都走 CPU
        gpuPath_ = false;
        MS_WARN("Nv12Converter: GPU 路径运行失败(%s)，切换到 CPU 回退", gpuErr.c_str());
    }

    return cpu_convert(dev_, ctx_, bgra, width_, height_, out, err);
}

// =============================================================================
//  ScreenCapture
// =============================================================================

ScreenCapture::ScreenCapture() {}

ScreenCapture::~ScreenCapture() {
    shutdown();
}

std::vector<MonitorInfo> ScreenCapture::enumerate_monitors() {
    std::vector<MonitorInfo> list;

    IDXGIFactory1* factory = nullptr;
    const HRESULT hr = ::CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                            reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || factory == nullptr) {
        MS_ERR("CreateDXGIFactory1 失败, hr=%s", hr_text(hr).c_str());
        return list;
    }

    int index = 0;
    for (UINT a = 0; ; ++a) {
        IDXGIAdapter1* adapter = nullptr;
        const HRESULT ahr = factory->EnumAdapters1(a, &adapter);
        if (ahr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(ahr) || adapter == nullptr) break;

        DXGI_ADAPTER_DESC1 adesc{};
        if (SUCCEEDED(adapter->GetDesc1(&adesc))) {
            const std::string friendly = wide_to_utf8(adesc.Description);
            for (UINT o = 0; ; ++o) {
                IDXGIOutput* output = nullptr;
                const HRESULT ohr = adapter->EnumOutputs(o, &output);
                if (ohr == DXGI_ERROR_NOT_FOUND) break;
                if (FAILED(ohr) || output == nullptr) break;

                DXGI_OUTPUT_DESC desc{};
                if (SUCCEEDED(output->GetDesc(&desc))) {
                    MonitorInfo mi;
                    mi.index        = index++;                       // 与 init() 的定位顺序一致
                    mi.deviceName   = wide_to_utf8(desc.DeviceName);  // WCHAR[32] -> UTF-8
                    mi.friendlyName = friendly;                       // 显卡输出描述
                    mi.left         = desc.DesktopCoordinates.left;
                    mi.top          = desc.DesktopCoordinates.top;
                    mi.width        = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
                    mi.height       = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
                    mi.isAttached   = (desc.AttachedToDesktop != FALSE);
                    mi.isPrimary    = is_primary_output(desc.DesktopCoordinates);
                    list.push_back(mi);
                }
                output->Release();
            }
        }
        adapter->Release();
    }

    factory->Release();
    MS_INFO("枚举到 %d 个显示输出", static_cast<int>(list.size()));
    return list;
}

bool ScreenCapture::init(int monitorIndex, bool captureCursor,
                         int outWidth, int outHeight, std::string& err) {
    err.clear();
    if (initialized_) { err = "ScreenCapture::init: 已经初始化"; return false; }
    if (monitorIndex < 0) { err = "ScreenCapture::init: 显示器索引不能为负"; return false; }
    if (outWidth < 0 || outHeight < 0) { err = "ScreenCapture::init: 输出尺寸不能为负"; return false; }
    if ((outWidth == 0) != (outHeight == 0)) {
        err = "ScreenCapture::init: 输出宽高必须同时指定（或同时为 0 表示原生）";
        return false;
    }

    monitorIndex_  = monitorIndex;
    outputOrdinal_ = monitorIndex;
    captureCursor_ = captureCursor;

    // 1) 找到这个全局下标对应的适配器 + 适配器内序号。
    //    多显卡（核显带屏 + 独显）时必须把设备建在"驱动该输出"的适配器上，
    //    否则 DuplicateOutput 会失败；单显卡机器上这一步等价于 monitorIndex 本身。
    IDXGIAdapter1* chosenAdapter = nullptr;
    int ordinalInAdapter = 0;
    std::string findErr;
    if (!find_adapter_by_global_index(monitorIndex, &chosenAdapter, &ordinalInAdapter, findErr)) {
        err = findErr;
        return false;
    }

    std::string devErr;
    if (create_d3d11_device(chosenAdapter, &device_, &context_, devErr)) {
        outputOrdinal_ = ordinalInAdapter;
    } else {
        // 显式适配器建不出来（极少见）：退回默认适配器 + 原始下标
        MS_WARN("在指定适配器上创建 D3D11 设备失败(%s)，改用默认适配器", devErr.c_str());
        outputOrdinal_ = monitorIndex;
        if (!create_d3d11_device(nullptr, &device_, &context_, devErr)) {
            chosenAdapter->Release();
            err = devErr;
            shutdown();
            return false;
        }
    }
    chosenAdapter->Release();

    // 2) 建 duplication（内部走 IDXGIDevice -> IDXGIAdapter -> EnumOutputs 定位输出）
    std::string dupErr;
    if (!create_duplication(dupErr)) {
        err = dupErr;
        shutdown();
        return false;
    }

    // 3) 输出尺寸：0 表示跟随原生。缩放交给 VideoProcessor 在 GPU 上做。
    //    NV12 要求偶数，这里先取偶，保证 width()/to_nv12() 与编码器尺寸一致。
    const int wantW = (outWidth  > 0) ? outWidth  : nativeWidth_;
    const int wantH = (outHeight > 0) ? outHeight : nativeHeight_;
    outWidth_  = (wantW + 1) & ~1;
    outHeight_ = (wantH + 1) & ~1;

    // 4) GPU 优先的 BGRA->NV12 转换器（内部失败会自动回退 CPU）
    std::string convErr;
    if (!converter_.init(device_, context_, nativeWidth_, nativeHeight_,
                         outWidth_, outHeight_, convErr)) {
        err = "Nv12Converter 初始化失败: " + convErr;
        shutdown();
        return false;
    }

    initialized_ = true;
    MS_INFO("屏幕采集已启动: monitor=%d(%d@adapter), 原生 %dx%d -> 输出 %dx%d, cursor=%d, converter=%s",
            monitorIndex_, outputOrdinal_, nativeWidth_, nativeHeight_, outWidth_, outHeight_,
            captureCursor_ ? 1 : 0,
            converter_.using_gpu_converter() ? "gpu" : "cpu");
    return true;
}

void ScreenCapture::shutdown() {
    // 逆序释放。注意顺序：必须先归还帧、再释放 duplication，最后才是设备和转换器
    // （转换器里持有 device/context 的引用，所以它要排在设备之前）。
    destroy_duplication();
    converter_.shutdown();
    if (context_ != nullptr) { context_->Release(); context_ = nullptr; }
    if (device_ != nullptr)  { device_->Release();  device_  = nullptr; }

    initialized_        = false;
    nativeWidth_        = 0;
    nativeHeight_       = 0;
    outWidth_           = 0;
    outHeight_          = 0;
    monitorIndex_       = 0;
    outputOrdinal_      = 0;
    lastHadCursor_      = false;
    consecutiveTimeouts_ = 0;
    pointer_            = PointerInfo{};
    monitorLeft_        = 0;
    monitorTop_         = 0;
    haveShape_          = false;
    lastShapeInfo_      = DXGI_OUTDUPL_POINTER_SHAPE_INFO{};
    lastShapeBuf_.clear();
    lastShapeBufLen_    = 0;
    lastShapeTryUs_     = 0;
}

bool ScreenCapture::create_duplication(std::string& err) {
    err.clear();
    if (device_ == nullptr) { err = "D3D11 设备未创建"; return false; }

    // 同一输出上能同时存在的 duplication 数量有限，先彻底释放旧的
    release_frame();
    if (dup_ != nullptr) { dup_->Release(); dup_ = nullptr; }

    // ID3D11Device -> IDXGIDevice -> IDXGIAdapter -> EnumOutputs 定位输出
    IDXGIAdapter* adapter = adapter_of_device(device_);
    if (adapter == nullptr) { err = "IDXGIDevice::GetAdapter 失败"; return false; }

    OutputTarget target;
    std::string locErr;
    const bool located = locate_output(adapter, outputOrdinal_, target, locErr);
    adapter->Release();
    if (!located) { err = locErr; return false; }

    if (target.width <= 0 || target.height <= 0) {
        target.output->Release();
        err = "显示输出尺寸无效";
        return false;
    }
    if (!target.attached) {
        target.output->Release();
        err = "该显示输出未连接到桌面";
        return false;
    }
    // 分辨率/位置变化：转换器缓存是按旧尺寸建的，必须让调用方重新 init
    if (nativeWidth_ != 0 && (target.width != nativeWidth_ || target.height != nativeHeight_)) {
        target.output->Release();
        err = "桌面分辨率已变化，需要重新初始化采集器";
        return false;
    }

    IDXGIOutputDuplication* dup = nullptr;
    const HRESULT hr = target.output->DuplicateOutput(device_, &dup);
    target.output->Release();
    if (FAILED(hr) || dup == nullptr) {
        err = "IDXGIOutput1::DuplicateOutput 失败, hr=" + hr_text(hr) + describe_dup_hr(hr);
        return false;
    }

    dup_ = dup;
    nativeWidth_  = target.width;
    nativeHeight_ = target.height;
    monitorLeft_  = target.left;
    monitorTop_   = target.top;
    // 光标状态整体重置：重建 duplication 后指针信息要重新取一遍
    // （shapeSeq_ 是计数器，故意不重置，保证客户端一定看得出"形状有新版本"）
    pointer_   = PointerInfo{};
    haveShape_ = false;
    lastShapeTryUs_ = 0;
    consecutiveTimeouts_ = 0;
    // captureCursor_ 由 Desktop Duplication 的桌面合成策略决定，这里只记录调用方意图；
    // 真正合成光标由系统完成，last_had_cursor() 告诉调用方本帧是否带光标更新。
    MS_INFO("Desktop Duplication 就绪: %dx%d (cursor=%d)", nativeWidth_, nativeHeight_,
            captureCursor_ ? 1 : 0);
    return true;
}

void ScreenCapture::destroy_duplication() {
    // 顺序必须是这样：先归还帧（ReleaseFrame），再释放 duplication。
    // 反过来的话这一帧永远算"未释放"，重建 duplication 后也可能拿不到新帧。
    release_frame();
    if (dup_ != nullptr) { dup_->Release(); dup_ = nullptr; }
}

// -----------------------------------------------------------------------------
// 光标
// -----------------------------------------------------------------------------
namespace {

inline void put_bgra(u8*& d, u8 b, u8 g, u8 r, u8 a) {
    d[0] = b; d[1] = g; d[2] = r; d[3] = a;
    d += 4;
}

} // namespace

// DXGI 的三种光标位图 -> 统一的「预乘 alpha BGRA」
bool ScreenCapture::convert_pointer_shape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& si,
                                          const u8* src, size_t srcLen) {
    const int w = int(si.Width);
    const int h = int(si.Height);
    if (w <= 0 || h <= 0 || w > 512 || h > 512 || src == nullptr) return false;
    const int pitch = int(si.Pitch);
    if (pitch <= 0) return false;
    if (srcLen < size_t(pitch) * size_t(h)) return false;   // 至少要有掩码/像素行

    pointer_.width  = w;
    pointer_.height = h;
    pointer_.hotX   = int(si.HotSpot.x);
    pointer_.hotY   = int(si.HotSpot.y);
    pointer_.bgra.assign(size_t(w) * size_t(h) * 4u, 0);
    u8* d = pointer_.bgra.data();

    switch (si.Type) {
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR: {
        // 彩色指针：本身就是 BGRA，但 alpha 不可靠 —— 整体当不透明
        for (int y = 0; y < h; ++y) {
            const u8* row = src + size_t(y) * size_t(pitch);
            for (int x = 0; x < w; ++x) {
                const u8* s = row + size_t(x) * 4u;
                put_bgra(d, s[0], s[1], s[2], 255);
            }
        }
        return true;
    }
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: {
        // 带 alpha 掩码的彩色指针。alpha=0 的严格语义是"把桌面反色"，
        // 实际极少出现；按透明处理，视觉上和系统光标基本一致。
        for (int y = 0; y < h; ++y) {
            const u8* row = src + size_t(y) * size_t(pitch);
            for (int x = 0; x < w; ++x) {
                const u8* s = row + size_t(x) * 4u;
                const u8 a = s[3];
                if (a == 0) {
                    put_bgra(d, 0, 0, 0, 0);
                } else {
                    put_bgra(d, u8(u32(s[0]) * a / 255u), u8(u32(s[1]) * a / 255u),
                             u8(u32(s[2]) * a / 255u), a);
                }
            }
        }
        return true;
    }
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
    default: {
        // 单色指针：前半是 AND 掩码，后半是 XOR 掩码，各 Height 行。
        // 规则：AND=1 -> 透明（露出桌面）；AND=0 -> XOR=1 画白，XOR=0 画黑。
        // （AND=1&XOR=1 的"反色"分支几乎没人用，这里按透明处理）
        if (srcLen < size_t(pitch) * size_t(h) * 2u) return false;
        for (int y = 0; y < h; ++y) {
            const u8* andRow = src + size_t(y) * size_t(pitch);
            const u8* xorRow = src + size_t(h + y) * size_t(pitch);
            for (int x = 0; x < w; ++x) {
                const u8 bit = u8(7 - (x & 7));
                const bool andBit = ((andRow[x >> 3] >> bit) & 1u) != 0;
                const bool xorBit = ((xorRow[x >> 3] >> bit) & 1u) != 0;
                if (andBit) {
                    put_bgra(d, 0, 0, 0, 0);
                } else {
                    const u8 v = xorBit ? 255 : 0;
                    put_bgra(d, v, v, v, 255);
                }
            }
        }
        return true;
    }
    }
}

void ScreenCapture::update_pointer(const DXGI_OUTDUPL_FRAME_INFO& info) {
    if (dup_ == nullptr) return;

    // ---- 1) 位置 + 可见性 ---------------------------------------------------
    // 【关键】用 GetCursorInfo，而不是死等 Desktop Duplication 的
    // PointerPosition 更新：dup 的指针位置只在"这次真有鼠标更新"时才是新值，
    // 而且 Connect 之后没人动鼠标就一直是旧值。
    // GetCursorInfo 反映「系统光标此刻可见吗、在哪」：游戏调用 ShowCursor(FALSE)
    // 进内场时 fVisible 立刻变 0，回菜单又变 1 —— 这正是我们要照抄的状态。
    bool gotPos = false;
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (::GetCursorInfo(&ci) != FALSE) {
        pointer_.available = true;
        // 可见性 = 系统说"在显示" 且 光标句柄非空。
        // 有的游戏不是 ShowCursor(FALSE)，而是 SetCursor(NULL) 把光标换成空句柄，
        // 这时 fVisible 可能仍为真，但实际屏幕上什么都没有 —— 一起判掉。
        pointer_.visible   = ((ci.flags & CURSOR_SHOWING) != 0) && ci.hCursor != nullptr;
        // 整桌面坐标 -> 被采集显示器内的坐标（多显示器必须减掉原点）
        pointer_.x = int(ci.ptScreenPos.x) - monitorLeft_;
        pointer_.y = int(ci.ptScreenPos.y) - monitorTop_;
        gotPos = true;
    }
    if (!gotPos && info.LastMouseUpdateTime.QuadPart != 0) {
        // 兜底：某些会话里 GetCursorInfo 拿不到（安全桌面等），用 dup 的值
        pointer_.available = true;
        pointer_.visible   = (info.PointerPosition.Visible != FALSE);
        pointer_.x = int(info.PointerPosition.Position.x) - monitorLeft_;
        pointer_.y = int(info.PointerPosition.Position.y) - monitorTop_;
    }
    // 光标跑到别的显示器上了：本路画面里不该出现（多显示器配置）
    if (pointer_.available &&
        (pointer_.x < 0 || pointer_.y < 0 ||
         pointer_.x >= nativeWidth_ || pointer_.y >= nativeHeight_)) {
        pointer_.visible = false;
    }

    // ---- 2) 形状 -----------------------------------------------------------
    // 鼠标更新时立刻取；鼠标没更新时，只要还没拿到形状就每 500ms 重试一次。
    // （刚连上时用户可能还没动鼠标，不重试的话客户端会很久都没有光标图片）
    const bool mouseUpdated = (info.LastMouseUpdateTime.QuadPart != 0);
    if (!mouseUpdated) {
        if (haveShape_ && pointer_.shapeId != 0) return;   // 形状没变，不用每帧查
        if (now_us() - lastShapeTryUs_ < 500000) return;   // 重试节流
        lastShapeTryUs_ = now_us();
    }

    // 先用一个足够大的缓冲直接取，失败再按 required 扩容重试。
    // 【别用「0 缓冲查大小」那套】实测 GetFramePointerShape(0, nullptr, &needed, &si)
    // 在部分系统上返回 E_INVALIDARG 且不改 needed，于是永远拿不到光标图片，
    // 表现为"远控端一直看不到鼠标"。
    if (shapeBuf_.size() < 64u * 1024u) shapeBuf_.resize(64u * 1024u);
    DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
    UINT got = UINT(shapeBuf_.size());
    HRESULT hr = dup_->GetFramePointerShape(got, shapeBuf_.data(), &got, &si);
    if (hr == DXGI_ERROR_MORE_DATA && got > 0 && got <= 4u * 1024u * 1024u) {
        shapeBuf_.resize(got);
        got = UINT(shapeBuf_.size());
        hr = dup_->GetFramePointerShape(got, shapeBuf_.data(), &got, &si);
    }
    // 前几次查询留痕（排障"看不到光标"时看这里：hr 和 size 就能定位）
    {
        static int dbg = 0;
        if (dbg < 6) {
            ++dbg;
            MS_INFO("[光标] 形状查询 hr=0x%08lX size=%u type=%d %ux%u",
                    static_cast<unsigned long>(static_cast<u32>(hr)), unsigned(got),
                    int(si.Type), unsigned(si.Width), unsigned(si.Height));
        }
    }
    if (FAILED(hr) || got == 0 || si.Width == 0 || si.Height == 0) return;

    // 形状没变就别重发（4KB 不大，但每次鼠标移动都发也没必要）
    if (haveShape_ && si.Type == lastShapeInfo_.Type &&
        si.Width == lastShapeInfo_.Width && si.Height == lastShapeInfo_.Height &&
        si.Pitch == lastShapeInfo_.Pitch &&
        si.HotSpot.x == lastShapeInfo_.HotSpot.x && si.HotSpot.y == lastShapeInfo_.HotSpot.y &&
        got == lastShapeBufLen_ && lastShapeBufLen_ > 0 &&
        std::memcmp(shapeBuf_.data(), lastShapeBuf_.data(), got) == 0) {
        return;
    }

    if (!convert_pointer_shape(si, shapeBuf_.data(), size_t(got))) return;
    lastShapeInfo_  = si;
    lastShapeBuf_.assign(shapeBuf_.begin(), shapeBuf_.begin() + got);
    lastShapeBufLen_ = size_t(got);
    haveShape_      = true;
    pointer_.shapeId = ++shapeSeq_;
    MS_INFO("光标形状已就绪: %dx%d 热点(%d,%d) type=%d shapeId=%u",
            pointer_.width, pointer_.height, pointer_.hotX, pointer_.hotY,
            int(si.Type), pointer_.shapeId);
}

int ScreenCapture::acquire_frame(int timeoutMs, std::string& next_error) {
    next_error.clear();
    if (!initialized_) {
        next_error = "采集器未初始化";
        return -1;
    }
    if (dup_ == nullptr) {
        // 上一次 ACCESS_LOST 重建失败（或被 shutdown）后会出现这种状态：必须重新 init
        next_error = "Desktop Duplication 不可用，需要重新初始化采集器";
        return -1;
    }

    // 防御：调用方如果忘记 release_frame()，这里先补上，否则 AcquireNextFrame 会
    // 因为上一帧没归还而失败/卡死（Desktop Duplication 的帧必须成对释放）。
    if (acquired_ != nullptr) release_frame();

    const UINT timeout = static_cast<UINT>(timeoutMs < 0 ? 0 : timeoutMs);
    int lostRetries = 0;
    for (;;) {
        DXGI_OUTDUPL_FRAME_INFO info{};
        IDXGIResource* resource = nullptr;
        const HRESULT hr = dup_->AcquireNextFrame(timeout, &info, &resource);

        if (hr == S_OK) {
            ID3D11Texture2D* tex = nullptr;
            const HRESULT qhr = resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                                         reinterpret_cast<void**>(&tex));
            resource->Release();   // IDXGIResource 只是借用，用完立刻放（dup_ 自己持有帧）
            if (FAILED(qhr) || tex == nullptr) {
                // 已经 Acquire 成功，必须配对 ReleaseFrame，否则后续取帧全部失败
                dup_->ReleaseFrame();
                next_error = "QueryInterface(ID3D11Texture2D) 失败";
                return -1;
            }
            acquired_            = tex;
            lastHadCursor_       = (info.LastMouseUpdateTime.QuadPart != 0);
            // 光标：位置/可见性/形状。必须在本帧持有期间取（ReleaseFrame 后失效）
            update_pointer(info);
            consecutiveTimeouts_ = 0;
            return 1;
        }

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // 画面没变化：不算错误，调用方可重复上一帧
            ++consecutiveTimeouts_;
            return 0;
        }

        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
            // 显示模式切换 / 会话重连 / 全屏独占退出后 duplication 失效：销毁重建一次
            if (lostRetries >= 1) {
                next_error = "AcquireNextFrame 反复失败(ACCESS_LOST/INVALID_CALL), hr=" + hr_text(hr);
                return -1;
            }
            ++lostRetries;
            MS_WARN("Duplication 失效(hr=%s)，尝试重建一次", hr_text(hr).c_str());
            destroy_duplication();
            std::string rebuildErr;
            if (!create_duplication(rebuildErr)) {
                next_error = rebuildErr;
                return -1;
            }
            continue;   // 用同一次 timeout 再试一次
        }

        next_error = "AcquireNextFrame 失败, hr=" + hr_text(hr);
        return -1;
    }
}

void ScreenCapture::release_frame() {
    // 必须与 acquire_frame() 的 S_OK 成对：先放纹理引用，再 ReleaseFrame。
    if (acquired_ != nullptr) {
        acquired_->Release();
        acquired_ = nullptr;
    }
    if (dup_ != nullptr) {
        // 没有未释放的帧时返回 DXGI_ERROR_INVALID_CALL，忽略即可
        dup_->ReleaseFrame();
    }
}

bool ScreenCapture::to_nv12(Nv12Frame& out, std::string& err) {
    err.clear();
    if (acquired_ == nullptr) {
        err = "to_nv12: 当前没有已获取的帧（acquire_frame 返回 1 才能转换）";
        return false;
    }
    return converter_.convert(acquired_, out, err);
}

} // namespace ms::host
