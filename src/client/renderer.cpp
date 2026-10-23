// =============================================================================
//  renderer.cpp - D3D11 渲染 + Direct2D/DirectWrite 悬浮状态条
//
//  低延迟渲染要点
//   * DXGI_SWAP_EFFECT_FLIP_DISCARD + Present(0, ALLOW_TEARING)：不锁垂直同步。
//     tearing 是否可用先问 IDXGIFactory5::CheckFeatureSupport，不可用就退回
//     Present(0, 0)（仍然不等 vsync，只是可能被 DWM 丢弃一帧）。
//   * NV12 -> RGB 全部在像素着色器里做（BT.709 limited range），不碰 CPU。
//   * 硬解帧零拷贝：解码输出的 NV12 纹理整块 CopySubresourceRegion 到我们的
//     NV12 中转纹理，再用两个平面 SRV（Y=R8_UNORM/PlaneSlice 0，
//     UV=R8G8_UNORM/PlaneSlice 1）采样——两条路径共用同一个 PS。
//   * 软解上传：NV12 纹理在 D3D11 里不可 Map（video 格式没有 CPU 访问），
//     所以另外建 R8 + R8G8 两个 DYNAMIC 纹理，按行 Map/拷贝，布局与 NV12
//     的两个平面完全一致，采样接口也完全一致。
//   * 悬浮条用 Direct2D 画在同一个后缓冲上，画完直接 Present。
// =============================================================================

#include "renderer.h"

#include "../common/clock.h"
#include "../common/log.h"

#include <algorithm>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <d3d11_3.h>
#include <d3dcompiler.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include <cstdio>
#include <cstring>
#include <string>

namespace ms::client {

namespace {

// -----------------------------------------------------------------------------
// 内嵌 HLSL（D3DCompile 运行时编译，不需要额外的 .hlsl 文件）
//
//  VS：不设顶点缓冲，用 SV_VertexID 生成一个覆盖全屏的大三角形
//      (-1,-1) (3,-1) (-1,3)，再按 gScale 缩放成 letterbox 后的视频矩形。
//      缩放后的三角形仍然恰好覆盖 [-sx,sx]x[-sy,sy]，所以视频区不多不少。
//  PS：采样 NV12 的两个平面，按 BT.709 limited range 转 RGB。
// -----------------------------------------------------------------------------
const char kShaderSource[] = R"HLSL(
cbuffer Params : register(b0)
{
    float2 gScale;    // NDC 缩放：保持视频宽高比，黑边交给 clear
    float2 gUvScale;  // 逻辑尺寸 / 纹理分配尺寸（1080 常被对齐到 1088）
    float2 gOffset;   // NDC 平移（1:1 模式下画面比窗口大时居中裁切用）
    float  gSharpen;  // 锐化强度（0=关；0.35/0.7/1.2 对应 低/中/高）
    float  gPad;
};

Texture2D<float>  texY  : register(t0);
Texture2D<float2> texUV : register(t1);
SamplerState      smp   : register(s0);

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    // 覆盖全屏的大三角形：(p) ∈ {0,0},{2,0},{0,2}，映射到 NDC 就是
    // (-1,-1),(3,-1),(-1,3)；再乘 gScale 得到 letterbox 后的视频矩形。
    float2 p = float2(float((vid << 1) & 2), float(vid & 2));
    VSOut o;
    // 【UV = p，不能再乘 0.5】
    // 可见的视频矩形对应 p∈[0,1]（乘 gScale 后正好落到 [-s,s]）。
    // 之前写成 p*0.5，等于只采样纹理的左下四分之一并放大 2 倍，
    // 真机表现就是"连上了但只看到远端桌面的左上/左下角一小块"。
    //   V 必须翻转：纹理 v=0 是图像第一行（顶部），而 NDC y=-1 是屏幕底部。
    o.uv  = float2(p.x, 1.0 - p.y);
    o.pos = float4((p * 2.0 - 1.0) * gScale + gOffset, 0.0, 1.0);
    return o;
}

float4 PSMain(VSOut i) : SV_TARGET
{
    float2 uv = i.uv * gUvScale;

    float y = texY.Sample(smp, uv).r;

    // ---- 锐化（Unsharp Mask，只作用在亮度上）----
    // 视频编码 + 缩放会把细节糊掉，游戏里的文字/远处目标尤其明显。
    // 在 Y 上做 4 邻域反锐化：既能把细节拉回来，又不会在彩色边缘产生色斑
    // （色度不动，所以不会出现红/绿描边）。强度 0 时这条分支完全跳过。
    if (gSharpen > 0.0)
    {
        uint tw, th;
        texY.GetDimensions(tw, th);
        float2 t = 1.0 / float2(max(tw, 1u), max(th, 1u));
        float yl = texY.Sample(smp, uv - float2(t.x, 0.0)).r;
        float yr = texY.Sample(smp, uv + float2(t.x, 0.0)).r;
        float yu = texY.Sample(smp, uv - float2(0.0, t.y)).r;
        float yd = texY.Sample(smp, uv + float2(0.0, t.y)).r;
        float avg = (yl + yr + yu + yd) * 0.25;
        y = saturate(y + (y - avg) * gSharpen);
    }

    float2 c = texUV.Sample(smp, uv).rg;

    // BT.709 limited range：Y 16..235，Cb/Cr 16..240
    float Y = (y   - 16.0 / 255.0) * (255.0 / 219.0);
    float U = (c.x - 128.0 / 255.0) * (255.0 / 224.0);
    float V = (c.y - 128.0 / 255.0) * (255.0 / 224.0);

    float3 rgb;
    rgb.r = Y + 1.5748 * V;
    rgb.g = Y - 0.1873 * U - 0.4681 * V;
    rgb.b = Y + 1.8556 * U;
    return float4(saturate(rgb), 1.0);
}
)HLSL";

// 常量缓冲：D3D11 要求 16 字节对齐
struct ShaderParams {
    float scale[2];     // NDC 缩放（letterbox / 拉伸 / 1:1）
    float uvScale[2];   // 视频逻辑尺寸 / 纹理尺寸
    float offset[2];    // NDC 平移（1:1 模式居中裁切）
    float sharpen;      // 锐化强度（0=关）
    float pad;          // 16 字节对齐
};
static_assert(sizeof(ShaderParams) == 32, "constant buffer must be 16-byte aligned");

// UTF-8 -> UTF-16（OverlayStats::extra 是 UTF-8）
std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) {
        return std::wstring();
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) {
        return std::wstring();
    }
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

// 软解上传用的动态平面纹理（R8 / R8G8）
bool make_dynamic_plane(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt,
                        ID3D11Texture2D** tex, ID3D11ShaderResourceView** srv) {
    if (!dev || w == 0 || h == 0) {
        return false;
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.SampleDesc.Quality = 0;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    d.MiscFlags = 0;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, tex)) || !*tex) {
        *tex = nullptr;
        return false;
    }
    if (FAILED(dev->CreateShaderResourceView(*tex, nullptr, srv)) || !*srv) {
        if (*srv) { (*srv)->Release(); *srv = nullptr; }
        (*tex)->Release();
        *tex = nullptr;
        return false;
    }
    return true;
}

} // namespace

Renderer::Renderer() = default;

Renderer::~Renderer() {
    shutdown();
}

// -----------------------------------------------------------------------------
// 初始化
// -----------------------------------------------------------------------------
bool Renderer::init(HWND hwnd, ID3D11Device* sharedDevice, bool vsync, std::string& err) {
    shutdown();
    err.clear();

    if (!hwnd) {
        err = "Renderer::init: hwnd 为空";
        return false;
    }
    hwnd_ = hwnd;
    vsync_ = vsync;

    if (sharedDevice) {
        // 与解码器共用同一个设备：零拷贝的前提。自己 AddRef 一份，shutdown 时还回去
        device_ = sharedDevice;
        device_->AddRef();
        ownDevice_ = false;
    } else {
        static const D3D_FEATURE_LEVEL kLevels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
        };
        // D2D 与 swapchain 都需要 BGRA 支持
        const UINT devFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, devFlags,
                                       kLevels, ARRAYSIZE(kLevels), D3D11_SDK_VERSION,
                                       &device_, nullptr, nullptr);
        if (hr == E_INVALIDARG) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, devFlags,
                                   kLevels + 1, ARRAYSIZE(kLevels) - 1, D3D11_SDK_VERSION,
                                   &device_, nullptr, nullptr);
        }
        if (FAILED(hr) || !device_) {
            err = "D3D11CreateDevice 失败";
            shutdown();
            return false;
        }
        ownDevice_ = true;
    }

    device_->GetImmediateContext(&context_);
    if (!context_) {
        err = "GetImmediateContext 失败";
        shutdown();
        return false;
    }

    if (!create_swapchain(hwnd, err)) {
        shutdown();
        return false;
    }
    if (!ensure_backbuffer_views()) {
        err = "创建后缓冲视图失败";
        shutdown();
        return false;
    }
    if (!create_pipeline(err)) {
        shutdown();
        return false;
    }

    // 悬浮层失败不致命：视频照常播放，把原因作为提示交给调用方
    std::string overlayErr;
    if (!create_overlay(overlayErr)) {
        err = overlayErr;
    }
    return true;
}

// -----------------------------------------------------------------------------
// swapchain：FLIP_DISCARD + 视情况开 ALLOW_TEARING
// -----------------------------------------------------------------------------
bool Renderer::create_swapchain(HWND hwnd, std::string& err) {
    // device -> IDXGIDevice -> IDXGIAdapter -> IDXGIFactory2
    IDXGIDevice* dxgiDevice = nullptr;
    HRESULT hr = device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    if (FAILED(hr) || !dxgiDevice) {
        err = "QueryInterface(IDXGIDevice) 失败";
        return false;
    }
    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(hr) || !adapter) {
        err = "IDXGIDevice::GetAdapter 失败";
        return false;
    }
    IDXGIFactory2* factory = nullptr;
    hr = adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
    adapter->Release();
    if (FAILED(hr) || !factory) {
        err = "GetParent(IDXGIFactory2) 失败";
        return false;
    }

    // tearing：不先问就带上 flag，CreateSwapChain 会直接失败
    tearingSupported_ = false;
    IDXGIFactory5* factory5 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&factory5))) && factory5) {
        BOOL allow = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) {
            tearingSupported_ = (allow != FALSE);
        }
        factory5->Release();
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);
    const LONG cw = rc.right - rc.left;
    const LONG ch = rc.bottom - rc.top;
    const UINT w = static_cast<UINT>(cw > 0 ? cw : 1);
    const UINT h = static_cast<UINT>(ch > 0 ? ch : 1);

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = w;
    desc.Height = h;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   // D2D 叠加要求 BGRA
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // BufferCount 2 -> 3：翻转模型下 2 个缓冲时，一旦两帧在途 Present(0) 就会
    // 阻塞到垂直回扫（主循环被卡住 = 输入/解码跟着顿）。3 个缓冲 + 帧延迟等待对象
    // 才既不等 vsync、也不会阻塞（Moonlight 也是 3+1+1 的缓冲思路）。
    desc.BufferCount = 3;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = (tearingSupported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0) |
                 DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    hr = factory->CreateSwapChainForHwnd(device_, hwnd, &desc, nullptr, nullptr, &swapChain_);
    if (FAILED(hr) || !swapChain_) {
        // 少数驱动/系统不认帧延迟等待对象或 tearing：退回最保守的组合再试
        desc.BufferCount = 3;
        desc.Flags = 0;
        tearingSupported_ = false;
        hr = factory->CreateSwapChainForHwnd(device_, hwnd, &desc, nullptr, nullptr, &swapChain_);
    }
    if (FAILED(hr) || !swapChain_) {
        err = "CreateSwapChainForHwnd 失败";
        factory->Release();
        return false;
    }

    // 取帧延迟等待对象：之后每次呈现前等它，避免 Present(0) 阻塞。
    // 最多 2 帧在途（1 帧正在扫描 + 1 帧可渲染），延迟上限约 1 帧。
    if ((desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0) {
        IDXGISwapChain2* sc2 = nullptr;
        if (SUCCEEDED(swapChain_->QueryInterface(__uuidof(IDXGISwapChain2),
                                                 reinterpret_cast<void**>(&sc2))) && sc2) {
            sc2->SetMaximumFrameLatency(2);
            frameLatencyWaitable_ = sc2->GetFrameLatencyWaitableObject();
            sc2->Release();
        }
    }

    // 全屏切换（Alt+Enter）不归 DXGI 管
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->Release();

    bbWidth_ = static_cast<int>(w);
    bbHeight_ = static_cast<int>(h);
    return true;
}

// -----------------------------------------------------------------------------
// 后缓冲视图：RTV + D2D target（每帧检查一次身份，变了就重建）
// -----------------------------------------------------------------------------
bool Renderer::ensure_backbuffer_views() {
    if (!swapChain_ || !device_) {
        return false;
    }

    ID3D11Texture2D* bb = nullptr;
    HRESULT hr = swapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb));
    if (FAILED(hr) || !bb) {
        return false;
    }

    D3D11_TEXTURE2D_DESC bd{};
    bb->GetDesc(&bd);
    bbWidth_ = static_cast<int>(bd.Width);
    bbHeight_ = static_cast<int>(bd.Height);

    // flip model 理论上 GetBuffer(0) 每次都是同一个表面；万一驱动换了表面，
    // 旧视图就作废。这里只比较指针（不持有引用），变了就整体重建。
    if (bbPtr_ != nullptr && bbPtr_ != bb) {
        release_backbuffer_views();
    }
    bbPtr_ = bb;

    if (!rtv_) {
        hr = device_->CreateRenderTargetView(bb, nullptr, &rtv_);
        if (FAILED(hr)) {
            rtv_ = nullptr;
        }
    }

    if (d2dContext_ && !d2dTarget_) {
        IDXGISurface* surface = nullptr;
        hr = swapChain_->GetBuffer(0, __uuidof(IDXGISurface), reinterpret_cast<void**>(&surface));
        if (SUCCEEDED(hr) && surface) {
            const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                96.0f, 96.0f);
            const HRESULT bhr = d2dContext_->CreateBitmapFromDxgiSurface(surface, &props, &d2dTarget_);
            surface->Release();
            if (FAILED(bhr)) {
                d2dTarget_ = nullptr;
            }
        }
        if (d2dTarget_) {
            d2dContext_->SetTarget(d2dTarget_);
        }
    }

    bb->Release();   // GetBuffer 给的那一份
    return rtv_ != nullptr;
}

void Renderer::release_backbuffer_views() {
    if (d2dContext_) {
        d2dContext_->SetTarget(nullptr);   // 必须先解绑再放位图
    }
    if (d2dTarget_) {
        d2dTarget_->Release();
        d2dTarget_ = nullptr;
    }
    if (rtv_) {
        rtv_->Release();
        rtv_ = nullptr;
    }
    bbPtr_ = nullptr;
}

// -----------------------------------------------------------------------------
// 渲染管线：内嵌 HLSL -> VS/PS + 采样器 + 常量缓冲
// -----------------------------------------------------------------------------
bool Renderer::create_pipeline(std::string& err) {
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errBlob = nullptr;

    // 统一用 4_0：所有 D3D11 feature level（含 10.x）都认，单个全屏四边形不差这点
    const auto fail_with = [&err, &errBlob](const char* what) -> bool {
        err = what;
        if (errBlob) {
            err += ": ";
            err += static_cast<const char*>(errBlob->GetBufferPointer());
            errBlob->Release();
            errBlob = nullptr;
        }
        return false;
    };

    HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "ministream_nv12",
                            nullptr, nullptr, "VSMain", "vs_4_0", compileFlags, 0,
                            &vsBlob, &errBlob);
    if (FAILED(hr) || !vsBlob) {
        if (vsBlob) { vsBlob->Release(); }
        return fail_with("顶点着色器编译失败");
    }
    hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "ministream_nv12",
                    nullptr, nullptr, "PSMain", "ps_4_0", compileFlags, 0,
                    &psBlob, &errBlob);
    if (FAILED(hr) || !psBlob) {
        vsBlob->Release();
        if (psBlob) { psBlob->Release(); }
        return fail_with("像素着色器编译失败");
    }
    if (errBlob) {   // 有 warning 的 blob：不再需要
        errBlob->Release();
        errBlob = nullptr;
    }

    hr = device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs_);
    vsBlob->Release();
    if (FAILED(hr) || !vs_) {
        return fail_with("CreateVertexShader 失败");
    }
    hr = device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps_);
    psBlob->Release();
    if (FAILED(hr) || !ps_) {
        return fail_with("CreatePixelShader 失败");
    }

    // 采样器：线性 + CLAMP（视频边缘不重复采样，NV12 色度顺带做双线性上采样）
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0.0f;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device_->CreateSamplerState(&sd, &sampler_);
    if (FAILED(hr) || !sampler_) {
        return fail_with("CreateSamplerState 失败");
    }

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(ShaderParams);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = device_->CreateBuffer(&cbd, nullptr, &cb_);
    if (FAILED(hr) || !cb_) {
        return fail_with("CreateBuffer(常量缓冲) 失败");
    }

    // 光栅化状态：必须关掉背面剔除。
    // 全屏大三角形 (0,0)(2,0)(0,2) 在屏幕空间是逆时针，而 D3D11 默认
    // CullMode=BACK + FrontCounterClockwise=FALSE 会把它当背面整个剔掉，
    // 结果就是"渲染流程毫无报错、悬浮条正常、视频区永远全黑"。
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    hr = device_->CreateRasterizerState(&rd, &raster_);
    if (FAILED(hr) || !raster_) {
        return fail_with("CreateRasterizerState 失败");
    }

    // NV12 中转纹理要等第一帧拿到解码纹理尺寸后再建（ensure_nv12_texture）
    return true;
}

// -----------------------------------------------------------------------------
// Direct2D / DirectWrite 悬浮层
// -----------------------------------------------------------------------------
bool Renderer::create_overlay(std::string& err) {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_);
    if (FAILED(hr) || !d2dFactory_) {
        err = "D2D1CreateFactory 失败";
        return false;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    hr = device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    if (FAILED(hr) || !dxgiDevice) {
        err = "QueryInterface(IDXGIDevice) 失败";
        return false;
    }

    // 优先用我们自己的 D2D 工厂建设备，失败再退回 D2D1CreateDevice
    ID2D1Factory1* factory1 = nullptr;
    if (SUCCEEDED(d2dFactory_->QueryInterface(__uuidof(ID2D1Factory1), reinterpret_cast<void**>(&factory1))) && factory1) {
        hr = factory1->CreateDevice(dxgiDevice, &d2dDevice_);
        factory1->Release();
    } else {
        hr = D2D1CreateDevice(dxgiDevice, nullptr, &d2dDevice_);
    }
    dxgiDevice->Release();
    if (FAILED(hr) || !d2dDevice_) {
        err = "创建 D2D 设备失败";
        return false;
    }

    hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext_);
    if (FAILED(hr) || !d2dContext_) {
        err = "CreateDeviceContext 失败";
        return false;
    }

    IUnknown* unk = nullptr;
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &unk);
    if (FAILED(hr) || !unk) {
        err = "DWriteCreateFactory 失败";
        return false;
    }
    hr = unk->QueryInterface(__uuidof(IDWriteFactory), reinterpret_cast<void**>(&dwrite_));
    unk->Release();
    if (FAILED(hr) || !dwrite_) {
        err = "QueryInterface(IDWriteFactory) 失败";
        return false;
    }

    if (!create_text_format()) {
        err = "CreateTextFormat 失败";
        return false;
    }

    // 半透明底色 + 文字色
    hr = d2dContext_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black, 0.55f), &bgBrush_);
    if (FAILED(hr) || !bgBrush_) {
        err = "CreateSolidColorBrush(底色) 失败";
        return false;
    }
    hr = d2dContext_->CreateSolidColorBrush(D2D1::ColorF(0.92f, 0.96f, 1.0f, 1.0f), &textBrush_);
    if (FAILED(hr) || !textBrush_) {
        err = "CreateSolidColorBrush(文字) 失败";
        return false;
    }

    // D2D 目标位图要挂在 swapchain 后缓冲上
    return ensure_backbuffer_views();
}

bool Renderer::create_text_format() {
    if (textFormat_) {
        textFormat_->Release();
        textFormat_ = nullptr;
    }
    if (!dwrite_) {
        return false;
    }
    // 字号跟着窗口高度走：1080p 约 15px，最小 14
    const int h = bbHeight_ > 0 ? bbHeight_ : 1080;
    const int sizePx = (h / 70) > 14 ? (h / 70) : 14;
    const HRESULT hr = dwrite_->CreateTextFormat(L"Consolas", nullptr,
                                                DWRITE_FONT_WEIGHT_NORMAL,
                                                DWRITE_FONT_STYLE_NORMAL,
                                                DWRITE_FONT_STRETCH_NORMAL,
                                                static_cast<float>(sizePx), L"zh-cn",
                                                &textFormat_);
    if (FAILED(hr) || !textFormat_) {
        textFormat_ = nullptr;
        return false;
    }
    textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    return true;
}

void Renderer::draw_overlay(const OverlayStats& s) {
    lastStats_ = s;
    hasStats_ = true;
}

// 锐化强度（0=关）。直接改 uniform，下一帧 present 生效，不需要重连/重建管线。
void Renderer::set_sharpen(float strength) {
    if (strength < 0.0f) strength = 0.0f;
    if (strength > 2.0f) strength = 2.0f;
    if (sharpenStrength_ == strength) return;
    sharpenStrength_ = strength;
    MS_INFO("画面锐化 -> %s（强度 %.2f）",
            strength <= 0.0f ? "关闭"
                             : (strength < 0.5f ? "低" : (strength < 1.0f ? "中" : "高")),
            double(strength));
}

void Renderer::draw_centered_message(const std::wstring& text) {
    message_ = text;
    hasMessage_ = true;
    hasStats_ = false;   // 消息优先于悬浮条：同一帧只画一个
}

void Renderer::clear_message() {
    hasMessage_ = false;
}

void Renderer::draw_message_now() {
    if (!hasMessage_ || !d2dContext_ || !d2dTarget_ || !dwrite_ || !textFormat_ ||
        !bgBrush_ || !textBrush_) {
        return;
    }
    if (bbWidth_ <= 0 || bbHeight_ <= 0) {
        return;
    }

    // 大字号：跟随窗口高度，1080p 时约 44px
    const int h = bbHeight_ > 0 ? bbHeight_ : 1080;
    const float sizePx = static_cast<float>((h / 24) > 20 ? (h / 24) : 20);
    IDWriteTextFormat* bigFormat = nullptr;
    HRESULT hr = dwrite_->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
                                           DWRITE_FONT_WEIGHT_NORMAL,
                                           DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL,
                                           sizePx, L"zh-cn", &bigFormat);
    if (FAILED(hr) || !bigFormat) {
        bigFormat = textFormat_;   // 兜底用悬浮条的小字号格式
        bigFormat->AddRef();
    }
    bigFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    bigFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    IDWriteTextLayout* layout = nullptr;
    hr = dwrite_->CreateTextLayout(message_.c_str(), static_cast<UINT32>(message_.size()),
                                   bigFormat, static_cast<float>(bbWidth_) * 0.9f,
                                   static_cast<float>(bbHeight_) * 0.8f, &layout);
    if (FAILED(hr) || !layout) {
        bigFormat->Release();
        return;
    }
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WHOLE_WORD);

    DWRITE_TEXT_METRICS tm{};
    hr = layout->GetMetrics(&tm);
    if (FAILED(hr)) {
        layout->Release();
        bigFormat->Release();
        return;
    }

    const float pad = 18.0f;
    const float boxW = tm.width + pad * 2.0f;
    const float boxH = tm.height + pad * 2.0f;
    const float bbW = static_cast<float>(bbWidth_);
    const float bbH = static_cast<float>(bbHeight_);
    const float x = (bbW - boxW) * 0.5f;
    const float y = (bbH - boxH) * 0.5f;

    // D2D 要接管后缓冲：先解绑 D3D 的渲染目标，避免同一个资源既当 RTV 又当 target
    context_->OMSetRenderTargets(0, nullptr, nullptr);

    d2dContext_->BeginDraw();
    d2dContext_->SetTransform(D2D1::Matrix3x2F::Identity());
    d2dContext_->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(x, y, x + boxW, y + boxH), 10.0f, 10.0f), bgBrush_);
    d2dContext_->DrawTextLayout(D2D1::Point2F(x + pad, y + pad), layout, textBrush_);
    const HRESULT endHr = d2dContext_->EndDraw();
    layout->Release();
    bigFormat->Release();

    if (endHr == D2DERR_RECREATE_TARGET) {
        release_backbuffer_views();
    }
}

// -----------------------------------------------------------------------------
// 轻量提示条（切 TCP/UDP 这类状态变化用）
//   * 顶部居中的小胶囊，高度 ~30px，只占画面很小一条，不挡内容；
//   * 自动消失，不需要用户操作；
//   * 和"全屏居中大字提示"是两套东西：那个只用于连接/断线等需要用户注意的状态。
// -----------------------------------------------------------------------------
void Renderer::show_toast(const std::wstring& text, int durationMs) {
    toastText_ = text;
    toastUntilUs_ = now_us() + i64(durationMs) * 1000;
}

void Renderer::draw_toast_now() {
    if (toastText_.empty()) return;
    if (now_us() > toastUntilUs_) {
        toastText_.clear();
        return;
    }
    if (!d2dContext_ || !d2dTarget_ || !dwrite_ || !bgBrush_ || !textBrush_) return;
    if (bbWidth_ <= 0 || bbHeight_ <= 0) return;

    if (toastFormat_ == nullptr) {
        // 比悬浮条更小一号，避免"提示条"看起来像大面板
        const int h = bbHeight_ > 0 ? bbHeight_ : 1080;
        const float sizePx = static_cast<float>((h / 80) > 12 ? (h / 80) : 12);
        if (FAILED(dwrite_->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
                                             DWRITE_FONT_WEIGHT_NORMAL,
                                             DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL,
                                             sizePx, L"zh-cn", &toastFormat_)) ||
            toastFormat_ == nullptr) {
            return;
        }
        // 注意：这里必须 LEADING 而不是 CENTER！
        // DrawTextLayout 的坐标是 layout 左上角，alignment=CENTER 会把文字居中到
        // layout 的 maxWidth 里（bbWidth*0.9），结果文字跑到胶囊外面去——表现就是
        // "只看到一个空的灰色小条，没有字"。
        toastFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        // 竖直方向必须 TOP：如果设 CENTER，文字会居中到 layout 的 maxHeight
        // （400）中间，也就是跑到胶囊下面去 —— 之前"小条里没字"就是这个原因。
        toastFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dwrite_->CreateTextLayout(toastText_.c_str(),
                                         static_cast<UINT32>(toastText_.size()),
                                         toastFormat_,
                                         static_cast<float>(bbWidth_) * 0.9f, 60.0f,
                                         &layout)) || layout == nullptr) {
        return;
    }
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    DWRITE_TEXT_METRICS tm{};
    if (FAILED(layout->GetMetrics(&tm))) {
        layout->Release();
        return;
    }

    const float padX = 16.0f, padY = 7.0f;
    const float boxW = tm.width + padX * 2.0f;
    const float boxH = tm.height + padY * 2.0f;
    const float x = (static_cast<float>(bbWidth_) - boxW) * 0.5f;
    // 放在"顶部工具栏/小悬浮按钮"下面，避免互相遮挡；只占很窄一条，不挡画面
    const float y = 52.0f;

    context_->OMSetRenderTargets(0, nullptr, nullptr);
    d2dContext_->BeginDraw();
    d2dContext_->SetTransform(D2D1::Matrix3x2F::Identity());
    // 半透明黑底 + 细边框，浅色文字：在任何画面上都能看清，又不刺眼
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
        D2D1::RectF(x, y, x + boxW, y + boxH), boxH * 0.5f, boxH * 0.5f);
    d2dContext_->FillRoundedRectangle(rr, bgBrush_);
    d2dContext_->DrawTextLayout(D2D1::Point2F(x + padX, y + padY), layout, textBrush_);
    const HRESULT endHr = d2dContext_->EndDraw();
    layout->Release();
    if (endHr == D2DERR_RECREATE_TARGET) {
        release_backbuffer_views();
    }
}

void Renderer::draw_overlay_now() {
    // 全屏提示（连接中/等首帧/防火墙/断线原因）优先于悬浮条
    if (hasMessage_) {
        draw_message_now();
        return;
    }
    if (!overlayEnabled_ || !hasStats_ || !d2dContext_ || !d2dTarget_ || !dwrite_ ||
        !textFormat_ || !bgBrush_ || !textBrush_) {
        return;
    }
    if (bbWidth_ <= 0 || bbHeight_ <= 0) {
        return;
    }

    const OverlayStats& s = lastStats_;
    wchar_t line[512] = {};
    _snwprintf_s(line, _countof(line), _TRUNCATE,
                 L"FPS %.1f    码率 %.0f kbps\n"
                 L"丢帧 %.1f%%   队列 %d\n"
                 L"延迟 %.1f ms   RTT %d ms\n"
                 L"解码 %.2f ms   %s",
                 s.fps, s.kbps, s.lossPercent, s.queueDepth,
                 s.latencyMs, s.rttMs, s.decodeMs,
                 s.hardwareDecode ? L"硬解 D3D11VA" : L"软解");
    std::wstring text(line);
    if (!s.extra.empty()) {
        text += L"\n";
        text += utf8_to_wide(s.extra);
    }

    IDWriteTextLayout* layout = nullptr;
    const float maxW = static_cast<float>(bbWidth_) * 0.9f;
    HRESULT hr = dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                           textFormat_, maxW, 4096.0f, &layout);
    if (FAILED(hr) || !layout) {
        return;
    }
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    DWRITE_TEXT_METRICS tm{};
    hr = layout->GetMetrics(&tm);
    if (FAILED(hr)) {
        layout->Release();
        return;
    }

    const float pad = 10.0f;
    const float margin = 16.0f;
    const float boxW = tm.width + pad * 2.0f;
    const float boxH = tm.height + pad * 2.0f;
    const float bbW = static_cast<float>(bbWidth_);
    const float bbH = static_cast<float>(bbHeight_);
    const bool right = (overlayCorner_ == OverlayCorner::TopRight ||
                        overlayCorner_ == OverlayCorner::BottomRight);
    const bool bottom = (overlayCorner_ == OverlayCorner::BottomLeft ||
                         overlayCorner_ == OverlayCorner::BottomRight);
    const float x = right ? (bbW - margin - boxW) : margin;
    const float y = bottom ? (bbH - margin - boxH) : margin;

    // D2D 要接管后缓冲：先解绑 D3D 的渲染目标，避免同一个资源既当 RTV 又当 target
    context_->OMSetRenderTargets(0, nullptr, nullptr);

    d2dContext_->BeginDraw();
    d2dContext_->SetTransform(D2D1::Matrix3x2F::Identity());
    d2dContext_->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(x, y, x + boxW, y + boxH), 8.0f, 8.0f), bgBrush_);
    d2dContext_->DrawTextLayout(D2D1::Point2F(x + pad, y + pad), layout, textBrush_);
    const HRESULT endHr = d2dContext_->EndDraw();
    layout->Release();

    if (endHr == D2DERR_RECREATE_TARGET) {
        // 设备丢了：丢掉目标位图，下一次 ensure_backbuffer_views() 会重建
        release_backbuffer_views();
    }
}

// -----------------------------------------------------------------------------
// 远端光标
// -----------------------------------------------------------------------------
void Renderer::set_cursor_pos(int x, int y, bool visible) {
    cursorX_ = x;
    cursorY_ = y;
    cursorVisible_ = visible;
}

void Renderer::set_cursor_shape(int width, int height, int hotX, int hotY, const Bytes& bgra) {
    if (width <= 0 || height <= 0 || bgra.size() < size_t(width) * size_t(height) * 4u) {
        return;
    }
    cursorW_ = width;
    cursorH_ = height;
    cursorHotX_ = hotX;
    cursorHotY_ = hotY;
    cursorBgra_ = bgra;
    cursorShapeDirty_ = true;      // 下次绘制时重建 D2D 位图
    release_cursor_bitmap();
}

void Renderer::release_cursor_bitmap() {
    if (cursorBmp_ != nullptr) {
        cursorBmp_->Release();
        cursorBmp_ = nullptr;
    }
}

// 视频区在客户区里的像素矩形。必须和着色器里的 gScale 算法完全一致，
// 否则光标会和画面错位（尤其 1:1 模式和超宽窗口有黑边时）。
void Renderer::video_rect(float& outX, float& outY, float& outW, float& outH) const {
    const float bw = static_cast<float>(bbWidth_ > 0 ? bbWidth_ : 1);
    const float bh = static_cast<float>(bbHeight_ > 0 ? bbHeight_ : 1);
    const float vw = static_cast<float>(videoWidth_ > 0 ? videoWidth_ : 1);
    const float vh = static_cast<float>(videoHeight_ > 0 ? videoHeight_ : 1);
    float w = bw, h = bh;
    switch (scaleMode_) {
        case ScaleMode::Stretch:
            w = bw; h = bh;
            break;
        case ScaleMode::Actual:
            w = vw; h = vh;
            break;
        case ScaleMode::Fit:
        default: {
            const float sa = bw / bh;
            const float va = vw / vh;
            if (va > sa) { w = bw; h = bw * vh / vw; }
            else         { h = bh; w = bh * vw / vh; }
            break;
        }
    }
    outX = (bw - w) * 0.5f;
    outY = (bh - h) * 0.5f;
    outW = w;
    outH = h;
}

// 远端像素坐标 -> 客户区坐标（绝对鼠标映射用，与缩放模式一致）
bool Renderer::video_to_client(int vx, int vy, POINT& outClient) const {
    if (bbWidth_ <= 0 || bbHeight_ <= 0 || videoWidth_ <= 0 || videoHeight_ <= 0) {
        return false;
    }
    float rx = 0.0f, ry = 0.0f, rw = 0.0f, rh = 0.0f;
    video_rect(rx, ry, rw, rh);
    const float sx = rw / static_cast<float>(videoWidth_);
    const float sy = rh / static_cast<float>(videoHeight_);
    outClient.x = static_cast<LONG>(rx + static_cast<float>(vx) * sx + 0.5f);
    outClient.y = static_cast<LONG>(ry + static_cast<float>(vy) * sy + 0.5f);
    return true;
}

// 客户区坐标 -> 远端像素坐标（绝对鼠标映射用）
bool Renderer::client_to_video(POINT clientPos, int& vx, int& vy, bool& inside) const {
    inside = false;
    if (bbWidth_ <= 0 || bbHeight_ <= 0 || videoWidth_ <= 0 || videoHeight_ <= 0) {
        return false;
    }
    float rx = 0.0f, ry = 0.0f, rw = 0.0f, rh = 0.0f;
    video_rect(rx, ry, rw, rh);
    const float sx = rw / static_cast<float>(videoWidth_);
    const float sy = rh / static_cast<float>(videoHeight_);
    if (sx <= 0.0f || sy <= 0.0f) {
        return false;
    }
    const float fx = (static_cast<float>(clientPos.x) - rx) / sx;
    const float fy = (static_cast<float>(clientPos.y) - ry) / sy;
    inside = (fx >= 0.0f && fy >= 0.0f &&
              fx < static_cast<float>(videoWidth_) && fy < static_cast<float>(videoHeight_));
    // 夹到画面里：光标拖到黑边上时也发一个边界坐标，避免被控端光标卡在旧位置
    int ivx = static_cast<int>(fx);
    int ivy = static_cast<int>(fy);
    if (ivx < 0) ivx = 0;
    if (ivy < 0) ivy = 0;
    if (ivx >= videoWidth_) ivx = videoWidth_ - 1;
    if (ivy >= videoHeight_) ivy = videoHeight_ - 1;
    vx = ivx;
    vy = ivy;
    return true;
}

// v2：远端视频像素 -> 被控端虚拟桌面物理像素。
// 视频分辨率 = 被控端采集输出（GPU 已按需缩放到 outW×outH 的**物理像素**），
// 所以「视频 px -> 显示器内物理 px」是恒等映射；要补的只有显示器原点。
// 多显示器/负坐标直接体现在原点里；DPI 缩放不引入额外换算（物理像素不受
// 逻辑缩放影响）。若未来出现"视频 != 采集输出"的场景，按 deskPhysW_/H 比例修正。
bool Renderer::video_to_physical_desktop(int vx, int vy,
                                         int& outPhysX, int& outPhysY) const {
    if (videoWidth_ <= 0 || videoHeight_ <= 0) return false;
    // 目标点必须落在画面内：黑边区域对应的远端位置是"不存在的像素"
    if (vx < 0 || vy < 0 || vx >= videoWidth_ || vy >= videoHeight_) return false;

    double sx = 1.0, sy = 1.0;
    if (deskPhysW_ > 0 && deskPhysH_ > 0) {
        sx = double(deskPhysW_) / double(videoWidth_);
        sy = double(deskPhysH_) / double(videoHeight_);
    }
    outPhysX = monitorOriginX_ + static_cast<int>(vx * sx);
    outPhysY = monitorOriginY_ + static_cast<int>(vy * sy);
    return true;
}

void Renderer::draw_cursor_now() {
    if (!cursorDrawEnabled_) {
        return;   // 绝对模式：本机光标就是那个光标，不再画远端光标
    }
    if (!cursorVisible_ || cursorBgra_.empty()) {
        return;   // 被控端光标此刻不可见（游戏内场）或形状还没到
    }
    if (!d2dContext_ || !d2dTarget_ || bbWidth_ <= 0 || bbHeight_ <= 0) {
        return;
    }

    // 形状换了才重建位图（一般一场会话就几次）
    if (cursorBmp_ == nullptr && cursorW_ > 0 && cursorH_ > 0) {
        const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        d2dContext_->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(cursorW_), static_cast<UINT32>(cursorH_)),
            cursorBgra_.data(),
            static_cast<UINT32>(cursorW_) * 4u,
            &props, &cursorBmp_);
        cursorShapeDirty_ = false;
    }
    if (cursorBmp_ == nullptr) {
        return;
    }

    float vx, vy, vw, vh;
    video_rect(vx, vy, vw, vh);
    const float sx = vw / static_cast<float>(videoWidth_ > 0 ? videoWidth_ : 1);
    const float sy = vh / static_cast<float>(videoHeight_ > 0 ? videoHeight_ : 1);
    // 远端坐标是「热点位置」，图片左上角要退掉热点偏移
    const float px = vx + (static_cast<float>(cursorX_) - static_cast<float>(cursorHotX_)) * sx;
    const float py = vy + (static_cast<float>(cursorY_) - static_cast<float>(cursorHotY_)) * sy;
    const float pw = static_cast<float>(cursorW_) * sx;
    const float ph = static_cast<float>(cursorH_) * sy;

    // D2D 要接管后缓冲：先解绑 D3D 渲染目标
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    d2dContext_->BeginDraw();
    d2dContext_->SetTransform(D2D1::Matrix3x2F::Identity());
    d2dContext_->DrawBitmap(cursorBmp_, D2D1::RectF(px, py, px + pw, py + ph), 1.0f,
                            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    const HRESULT endHr = d2dContext_->EndDraw();
    if (endHr == D2DERR_RECREATE_TARGET) {
        release_backbuffer_views();
        release_cursor_bitmap();
    }
}

// -----------------------------------------------------------------------------
// 尺寸变化
// -----------------------------------------------------------------------------
void Renderer::resize(int width, int height) {
    // 最小化时窗口宽高为 0：直接返回，等恢复尺寸再重建
    if (width <= 0 || height <= 0) {
        return;
    }
    if (!swapChain_ || !device_ || !context_) {
        bbWidth_ = width;
        bbHeight_ = height;
        return;
    }
    if (width == bbWidth_ && height == bbHeight_) {
        return;
    }

    // ResizeBuffers 要求先释放所有后缓冲引用（RTV / D2D target / 我们缓存的表面）
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* noSrv[2] = { nullptr, nullptr };
    context_->PSSetShaderResources(0, 2, noSrv);
    context_->Flush();
    release_backbuffer_views();

    const UINT flags = (tearingSupported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0) |
                       (frameLatencyWaitable_ != nullptr
                            ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0);
    swapChain_->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height),
                              DXGI_FORMAT_UNKNOWN, flags);

    // 实际尺寸以重建后的后缓冲为准（ensure_backbuffer_views 里会写回）
    bbWidth_ = width;
    bbHeight_ = height;
    ensure_backbuffer_views();
    create_text_format();   // 字号跟着窗口高度自适应
}

// -----------------------------------------------------------------------------
// 视频纹理
// -----------------------------------------------------------------------------
void Renderer::release_hw_texture() {
    if (nv12SrvY_)  { nv12SrvY_->Release();  nv12SrvY_ = nullptr; }
    if (nv12SrvUV_) { nv12SrvUV_->Release(); nv12SrvUV_ = nullptr; }
    if (nv12Tex_)   { nv12Tex_->Release();   nv12Tex_ = nullptr; }
    hwTexW_ = 0;
    hwTexH_ = 0;
}

void Renderer::release_sw_textures() {
    if (swSrvY_)  { swSrvY_->Release();  swSrvY_ = nullptr; }
    if (swSrvUV_) { swSrvUV_->Release(); swSrvUV_ = nullptr; }
    if (swTexY_)  { swTexY_->Release();  swTexY_ = nullptr; }
    if (swTexUV_) { swTexUV_->Release(); swTexUV_ = nullptr; }
    texW_ = 0;
    texH_ = 0;
}

// 硬解零拷贝的中转纹理：尺寸必须和解码输出的纹理完全一致（1080 常对齐到 1088），
// 这样整块 CopySubresourceRegion 才合法；显示范围由 uvScale 裁掉多余的行。
bool Renderer::ensure_nv12_texture(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (nv12Tex_ && hwTexW_ == static_cast<UINT>(width) && hwTexH_ == static_cast<UINT>(height)) {
        return true;
    }
    release_hw_texture();

    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(width);
    d.Height = static_cast<UINT>(height);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_NV12;
    d.SampleDesc.Count = 1;
    d.SampleDesc.Quality = 0;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    if (FAILED(device_->CreateTexture2D(&d, nullptr, &nv12Tex_)) || !nv12Tex_) {
        nv12Tex_ = nullptr;
        return false;
    }

    // 平面 SRV 必须用 D3D11.3 的 CreateShaderResourceView1 + PlaneSlice。
    //
    // 【踩过的坑】用老接口 CreateShaderResourceView + 换 Format（R8_UNORM/R8G8_UNORM）
    // 的"hack"在 NV12 上不成立：驱动会把整块 NV12 内存按普通纹理解释，
    // 步距全错，画面出现内容重复 + 黑色竖条。NV12 这类 video 格式的平面
    // 只能靠 PlaneSlice 指定。
    // 不支持 D3D11.3 的系统返回 false，调用方会自动退到 CPU 中转路径
    // （那边是两张独立的 R8/R8G8 纹理，天生正确）。
    ID3D11Device3* d3 = nullptr;
    HRESULT hr = device_->QueryInterface(__uuidof(ID3D11Device3),
                                         reinterpret_cast<void**>(&d3));
    if (FAILED(hr) || !d3) {
        MS_WARN("D3D11.3 不可用，NV12 平面采样改走 CPU 中转");
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC1 sv{};
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MostDetailedMip = 0;
    sv.Texture2D.MipLevels = 1;

    sv.Format = DXGI_FORMAT_R8_UNORM;     // 平面 0：Y
    sv.Texture2D.PlaneSlice = 0;
    ID3D11ShaderResourceView1* srvY1 = nullptr;
    hr = d3->CreateShaderResourceView1(nv12Tex_, &sv, &srvY1);
    if (FAILED(hr) || !srvY1) {
        d3->Release();
        release_hw_texture();
        return false;
    }
    nv12SrvY_ = srvY1;   // ID3D11ShaderResourceView1 继承自 ID3D11ShaderResourceView

    sv.Format = DXGI_FORMAT_R8G8_UNORM;   // 平面 1：UV（U/V 交错）
    sv.Texture2D.PlaneSlice = 1;
    ID3D11ShaderResourceView1* srvUV1 = nullptr;
    hr = d3->CreateShaderResourceView1(nv12Tex_, &sv, &srvUV1);
    d3->Release();
    if (FAILED(hr) || !srvUV1) {
        release_hw_texture();
        return false;
    }
    nv12SrvUV_ = srvUV1;

    hwTexW_ = d.Width;
    hwTexH_ = d.Height;
    return true;
}

bool Renderer::ensure_sw_planes(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (swTexY_ && swTexUV_ && texW_ == static_cast<UINT>(width) && texH_ == static_cast<UINT>(height)) {
        return true;
    }
    release_sw_textures();

    const UINT w = static_cast<UINT>(width);
    const UINT h = static_cast<UINT>(height);
    const UINT cw = (w + 1u) / 2u;   // 色度平面宽（偶数分辨率就是 w/2）
    const UINT ch = (h + 1u) / 2u;
    if (!make_dynamic_plane(device_, w, h, DXGI_FORMAT_R8_UNORM, &swTexY_, &swSrvY_)) {
        release_sw_textures();
        return false;
    }
    if (!make_dynamic_plane(device_, cw, ch, DXGI_FORMAT_R8G8_UNORM, &swTexUV_, &swSrvUV_)) {
        release_sw_textures();
        return false;
    }
    texW_ = w;
    texH_ = h;
    return true;
}

// 软解帧 -> 两个平面纹理。支持 NV12 / YUV420P，以及 P010（取每个 16bit 采样的高字节）
bool Renderer::upload_cpu_frame(AVFrame* frame) {
    if (!frame || !context_ || !frame->data[0]) {
        return false;
    }
    const int w = frame->width;
    const int h = frame->height;
    if (w <= 0 || h <= 0 || !ensure_sw_planes(w, h)) {
        return false;
    }

    const int fmt = frame->format;
    const bool isNv12   = (fmt == AV_PIX_FMT_NV12);
    const bool isYuv420 = (fmt == AV_PIX_FMT_YUV420P);
    const bool isP010   = (fmt == AV_PIX_FMT_P010LE);   // Windows 上 P010 就是小端
    if (!isNv12 && !isYuv420 && !isP010) {
        return false;   // 其它格式（BGRA/P010BE...）暂不支持
    }

    // --- Y 平面 ---
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(swTexY_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    for (int y = 0; y < h; ++y) {
        const u8* src = frame->data[0] + static_cast<size_t>(y) * static_cast<size_t>(frame->linesize[0]);
        u8* dst = static_cast<u8*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        if (isP010) {
            for (int x = 0; x < w; ++x) {
                dst[x] = src[static_cast<size_t>(x) * 2 + 1];   // 10bit 在 16bit 的高位
            }
        } else {
            std::memcpy(dst, src, static_cast<size_t>(w));
        }
    }
    context_->Unmap(swTexY_, 0);

    // --- UV 平面（交错）---
    const int cw = (w + 1) / 2;
    const int ch = (h + 1) / 2;
    if (FAILED(context_->Map(swTexUV_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    for (int y = 0; y < ch; ++y) {
        u8* dst = static_cast<u8*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        if (isP010) {
            const u8* src = frame->data[1] + static_cast<size_t>(y) * static_cast<size_t>(frame->linesize[1]);
            for (int x = 0; x < cw; ++x) {
                dst[static_cast<size_t>(x) * 2 + 0] = src[static_cast<size_t>(x) * 4 + 1];  // U 高位
                dst[static_cast<size_t>(x) * 2 + 1] = src[static_cast<size_t>(x) * 4 + 3];  // V 高位
            }
        } else if (isNv12) {
            std::memcpy(dst, frame->data[1] + static_cast<size_t>(y) * static_cast<size_t>(frame->linesize[1]),
                        static_cast<size_t>(cw) * 2u);
        } else {   // YUV420P：U/V 各自成平面，这里交错成 NV12 的 UV
            const u8* up = frame->data[1] + static_cast<size_t>(y) * static_cast<size_t>(frame->linesize[1]);
            const u8* vp = frame->data[2] + static_cast<size_t>(y) * static_cast<size_t>(frame->linesize[2]);
            for (int x = 0; x < cw; ++x) {
                dst[static_cast<size_t>(x) * 2 + 0] = up[x];
                dst[static_cast<size_t>(x) * 2 + 1] = vp[x];
            }
        }
    }
    context_->Unmap(swTexUV_, 0);

    videoWidth_ = w;
    videoHeight_ = h;
    uvScaleX_ = 1.0f;
    uvScaleY_ = 1.0f;
    hwPath_ = false;
    return true;
}

// 硬解帧 -> NV12 中转纹理（GPU 内部整块拷贝）。格式不是 NV12 时退回 CPU 中转。
bool Renderer::acquire_hw_frame(AVFrame* frame) {
    if (!frame || !frame->data[0]) {
        return false;
    }
    if (hwCopyDisabled_) {
        return transfer_hw_frame(frame);
    }

    ID3D11Texture2D* src = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);
    if (sd.Format != DXGI_FORMAT_NV12) {
        // P010/BGRA 之类：NV12 平面 SRV 用不了，兜底走 CPU
        hwCopyDisabled_ = true;
        return transfer_hw_frame(frame);
    }

    // 解码纹理必须和我们在同一个 D3D11 设备上，否则这个拷贝是无效的
    // （调用方应该把 decoder.d3d11_device() 传给 Renderer::init 共享设备）
    ID3D11Device* srcDev = nullptr;
    src->GetDevice(&srcDev);
    const bool sameDevice = (srcDev == device_);
    if (srcDev) {
        srcDev->Release();
    }
    if (!sameDevice) {
        hwCopyDisabled_ = true;
        return transfer_hw_frame(frame);
    }

    if (!ensure_nv12_texture(static_cast<int>(sd.Width), static_cast<int>(sd.Height))) {
        // 建中转纹理或平面 SRV 失败（个别驱动不支持 NV12 采样）：以后都走 CPU
        hwCopyDisabled_ = true;
        return transfer_hw_frame(frame);
    }

    const UINT slice = static_cast<UINT>(reinterpret_cast<intptr_t>(frame->data[1]));
    context_->CopySubresourceRegion(nv12Tex_, 0, 0, 0, 0, src, slice, nullptr);

    videoWidth_ = frame->width;
    videoHeight_ = frame->height;
    // 逻辑尺寸可能比分配尺寸小（1080 vs 1088），采样时裁掉多余的行
    uvScaleX_ = static_cast<float>(videoWidth_) / static_cast<float>(sd.Width);
    uvScaleY_ = static_cast<float>(videoHeight_) / static_cast<float>(sd.Height);
    hwPath_ = true;
    return true;
}

// 硬解帧回退：把 GPU 帧拷回系统内存再走软解上传路径
bool Renderer::transfer_hw_frame(AVFrame* frame) {
    if (!frame) {
        return false;
    }
    if (!swFrame_) {
        swFrame_ = av_frame_alloc();
        if (!swFrame_) {
            return false;
        }
    }
    av_frame_unref(swFrame_);
    if (av_hwframe_transfer_data(swFrame_, frame, 0) < 0) {
        return false;
    }
    return upload_cpu_frame(swFrame_);
}

// -----------------------------------------------------------------------------
// 呈现
// -----------------------------------------------------------------------------
bool Renderer::present(AVFrame* frame, i64* outPresentUs) {
    if (outPresentUs) {
        *outPresentUs = 0;
    }
    if (!frame || !swapChain_ || !context_ || !vs_ || !ps_ || !cb_ || !sampler_) {
        return false;
    }
    if (!ensure_backbuffer_views()) {
        return false;
    }

    // 呈现节流：等交换链有一个空位再渲染（最多 2 帧在途）。
    // 8ms 超时：万一系统/驱动不回报，也不会把主循环卡死（宁可丢一次等待）。
    if (frameLatencyWaitable_ != nullptr) {
        ::WaitForSingleObject(frameLatencyWaitable_, 8);
    }

    // 1) 拿到本帧的两个平面 SRV（两条路径产出的布局是一样的）
    const bool ok = (frame->format == AV_PIX_FMT_D3D11) ? acquire_hw_frame(frame)
                                                        : upload_cpu_frame(frame);
    if (!ok) {
        return false;
    }
    ID3D11ShaderResourceView* srvY  = hwPath_ ? nv12SrvY_  : swSrvY_;
    ID3D11ShaderResourceView* srvUV = hwPath_ ? nv12SrvUV_ : swSrvUV_;
    if (!srvY || !srvUV) {
        return false;
    }

    // 渲染路径诊断（仅 Debug 级别；排查黑屏/花屏时把 log level 调到 1 即可看到）
    {
        static i64 lastPathLogUs = 0;
        const i64 now = now_us();
        if (now - lastPathLogUs > 1000000) {
            lastPathLogUs = now;
            MS_DEBUG("[渲染] 路径=%s 硬解回退=%d frame.format=%d 逻辑=%dx%d 纹理=%ux%u "
                     "uvScale=%.4f,%.4f 后缓冲=%dx%d",
                     hwPath_ ? "GPU零拷贝" : "CPU上传", hwCopyDisabled_ ? 1 : 0,
                     int(frame->format), videoWidth_, videoHeight_, hwTexW_, hwTexH_,
                     uvScaleX_, uvScaleY_, bbWidth_, bbHeight_);
        }
    }

    // 2) 设置渲染目标并清成黑色（letterbox 的黑边）
    context_->OMSetRenderTargets(1, &rtv_, nullptr);
    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    context_->ClearRenderTargetView(rtv_, black);

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = static_cast<float>(bbWidth_ > 0 ? bbWidth_ : 1);
    vp.Height = static_cast<float>(bbHeight_ > 0 ? bbHeight_ : 1);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &vp);

    // 3) 常量缓冲：按显示模式算缩放/平移
    const float bw = static_cast<float>(bbWidth_ > 0 ? bbWidth_ : 1);
    const float bh = static_cast<float>(bbHeight_ > 0 ? bbHeight_ : 1);
    const float vw = static_cast<float>(videoWidth_ > 0 ? videoWidth_ : 1);
    const float vh = static_cast<float>(videoHeight_ > 0 ? videoHeight_ : 1);
    const float screenAspect = bw / bh;
    const float videoAspect  = vw / vh;
    ShaderParams params{};
    switch (scaleMode_) {
        case ScaleMode::Stretch:
            // 拉伸填充：铺满窗口/屏幕（可能变形，但最大化利用像素）
            params.scale[0] = 1.0f;
            params.scale[1] = 1.0f;
            break;
        case ScaleMode::Actual:
            // 原始大小 1:1：视频像素 = 屏幕像素，画面比窗口大就居中裁切
            params.scale[0] = vw / bw;
            params.scale[1] = vh / bh;
            break;
        case ScaleMode::Fit:
        default:
            // 智能缩放：等比缩放 + 黑边（默认，任何窗口都看全）
            params.scale[0] = 1.0f;
            params.scale[1] = 1.0f;
            if (videoAspect > screenAspect) {
                params.scale[1] = screenAspect / videoAspect;   // 视频更宽：上下黑边
            } else {
                params.scale[0] = videoAspect / screenAspect;   // 视频更高：左右黑边
            }
            break;
    }
    params.offset[0] = 0.0f;
    params.offset[1] = 0.0f;
    params.uvScale[0] = uvScaleX_;
    params.uvScale[1] = uvScaleY_;
    params.sharpen = sharpenStrength_;
    params.pad = 0.0f;
    context_->UpdateSubresource(cb_, 0, nullptr, &params, 0, 0);

    // 4) 全屏四边形：VS 用 SV_VertexID 生成三角形，不需要顶点缓冲/输入布局
    ID3D11Buffer* cbs[1] = { cb_ };
    ID3D11ShaderResourceView* srvs[2] = { srvY, srvUV };
    ID3D11SamplerState* samplers[1] = { sampler_ };
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->RSSetState(raster_);
    context_->VSSetShader(vs_, nullptr, 0);
    context_->PSSetShader(ps_, nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, cbs);
    context_->PSSetConstantBuffers(0, 1, cbs);
    context_->PSSetShaderResources(0, 2, srvs);
    context_->PSSetSamplers(0, 1, samplers);
    context_->Draw(3, 0);

    // 解绑 SRV，避免同一资源同时被采样和当渲染目标
    ID3D11ShaderResourceView* noSrv[2] = { nullptr, nullptr };
    context_->PSSetShaderResources(0, 2, noSrv);

    // 5) 悬浮状态条（内部会解绑 RTV 交给 D2D）
    draw_overlay_now();
    // 5.2) 轻量提示条（切 TCP/UDP 等）：小胶囊，不挡画面
    draw_toast_now();
    // 5.5) 远端光标画在最上面（悬浮条之上）：被控端的光标本来就该盖住一切
    draw_cursor_now();

    // 6) Present：关 vsync 时优先带 ALLOW_TEARING，绝不等待刷新周期
    const UINT presentFlags = (!vsync_ && tearingSupported_) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    const i64 t0 = now_us();
    const HRESULT hr = swapChain_->Present(vsync_ ? 1u : 0u, presentFlags);
    if (outPresentUs) {
        *outPresentUs = now_us() - t0;
    }
    return SUCCEEDED(hr);
}

// -----------------------------------------------------------------------------
// 清屏（未连接时的底色）
// -----------------------------------------------------------------------------
void Renderer::clear(float r, float g, float b) {
    if (!swapChain_ || !context_ || !ensure_backbuffer_views()) {
        return;
    }
    context_->OMSetRenderTargets(1, &rtv_, nullptr);
    const float color[4] = { r, g, b, 1.0f };
    context_->ClearRenderTargetView(rtv_, color);
    context_->OMSetRenderTargets(0, nullptr, nullptr);

    // 统计已经来过就把悬浮条继续画上，否则纯底色
    draw_overlay_now();
    draw_toast_now();
    draw_cursor_now();   // 等待首帧/断线提示期间也把光标画出来

    const UINT presentFlags = (!vsync_ && tearingSupported_) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    swapChain_->Present(vsync_ ? 1u : 0u, presentFlags);
}

// -----------------------------------------------------------------------------
// 释放（可重复调用，逆序）
// -----------------------------------------------------------------------------
void Renderer::shutdown() {
    if (context_) {
        context_->ClearState();   // 解绑所有管线资源
    }
    // D2D 目标必须先解绑，再释放后缓冲相关引用，否则 ResizeBuffers/Release 会失败
    release_backbuffer_views();

    if (bgBrush_)    { bgBrush_->Release();    bgBrush_ = nullptr; }
    if (textBrush_)  { textBrush_->Release();  textBrush_ = nullptr; }
    if (toastFormat_){ toastFormat_->Release(); toastFormat_ = nullptr; }
    if (textFormat_) { textFormat_->Release(); textFormat_ = nullptr; }
    if (dwrite_)     { dwrite_->Release();     dwrite_ = nullptr; }
    if (d2dTarget_)  { d2dTarget_->Release();  d2dTarget_ = nullptr; }
    if (d2dContext_) { d2dContext_->Release(); d2dContext_ = nullptr; }
    if (d2dDevice_)  { d2dDevice_->Release();  d2dDevice_ = nullptr; }
    if (d2dFactory_) { d2dFactory_->Release(); d2dFactory_ = nullptr; }

    release_hw_texture();
    release_sw_textures();
    release_cursor_bitmap();
    cursorBgra_.clear();
    cursorVisible_ = false;
    cursorShapeDirty_ = false;
    cursorW_ = cursorH_ = 0;
    cursorHotX_ = cursorHotY_ = 0;
    if (sampler_) { sampler_->Release(); sampler_ = nullptr; }
    if (raster_)  { raster_->Release();  raster_  = nullptr; }
    if (cb_)      { cb_->Release();      cb_ = nullptr; }
    if (vs_)      { vs_->Release();      vs_ = nullptr; }
    if (ps_)      { ps_->Release();      ps_ = nullptr; }

    if (swFrame_) {
        av_frame_free(&swFrame_);
    }

    if (swapChain_) {
        // 等待对象句柄由我们持有，必须先关掉再释放交换链
        if (frameLatencyWaitable_ != nullptr) {
            ::CloseHandle(frameLatencyWaitable_);
            frameLatencyWaitable_ = nullptr;
        }
        swapChain_->Release();
        swapChain_ = nullptr;
    }
    if (context_) {
        context_->Release();
        context_ = nullptr;
    }
    if (device_) {
        // 自建的、共享的都在这里 Release（init 里共享设备已经 AddRef 过）
        device_->Release();
        device_ = nullptr;
    }
    ownDevice_ = false;
    hwnd_ = nullptr;
    bbWidth_ = 0;
    bbHeight_ = 0;
    videoWidth_ = 0;
    videoHeight_ = 0;
    hwCopyDisabled_ = false;
    hwPath_ = false;
    uvScaleX_ = 1.0f;
    uvScaleY_ = 1.0f;
    hasStats_ = false;
    hasMessage_ = false;
    message_.clear();
    toastText_.clear();
    toastUntilUs_ = 0;
    tearingSupported_ = false;
    lastStats_ = OverlayStats{};
}

} // namespace ms::client
