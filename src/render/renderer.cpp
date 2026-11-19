#include "render/renderer.h"

#include <algorithm>

#include <d3dcompiler.h>
#include <dxgi1_5.h>

#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include "core/log.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace wasd::render {

static const char* kShader = R"(
cbuffer Color : register(b0) { float4 r0; float4 r1; float4 r2; };
Texture2D texY : register(t0);
Texture2D texU : register(t1);
Texture2D texV : register(t2);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 toRgb(float3 yuv) {
    float4 p = float4(yuv, 1);
    return float4(saturate(float3(dot(r0, p), dot(r1, p), dot(r2, p))), 1);
}
float4 ps_nv12(VSOut i) : SV_Target {
    float y = texY.Sample(smp, i.uv).r;
    float2 uv = texU.Sample(smp, i.uv).rg;
    return toRgb(float3(y, uv));
}
float4 ps_i420(VSOut i) : SV_Target {
    return toRgb(float3(texY.Sample(smp, i.uv).r, texU.Sample(smp, i.uv).r, texV.Sample(smp, i.uv).r));
}
)";

// Rows of a 3x4 matrix applied to (Y, U, V, 1), each component normalized to [0,1].
static void buildMatrix(bool bt709, bool fullRange, float m[12]) {
    const float kr = bt709 ? 0.2126f : 0.299f, kb = bt709 ? 0.0722f : 0.114f, kg = 1 - kr - kb;
    const float ys = fullRange ? 1.0f : 255.0f / 219.0f, yo = fullRange ? 0.0f : -16.0f / 255.0f;
    const float cs = fullRange ? 1.0f : 255.0f / 224.0f;
    const float rv = 2 * (1 - kr) * cs, bu = 2 * (1 - kb) * cs;
    const float gu = -bu * kb / kg, gv = -rv * kr / kg;
    const float c = 128.0f / 255.0f;
    const float rows[3][3] = {{ys, 0, rv}, {ys, gu, gv}, {ys, bu, 0}};
    for (int r = 0; r < 3; ++r) {
        m[r * 4 + 0] = rows[r][0];
        m[r * 4 + 1] = rows[r][1];
        m[r * 4 + 2] = rows[r][2];
        m[r * 4 + 3] = rows[r][0] * yo - rows[r][1] * c - rows[r][2] * c;
    }
}

bool Renderer::init(HWND hwnd, std::string& err) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION,
                                   &device_, nullptr, &ctx_);
    if (FAILED(hr)) {
        flags &= ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &device_,
                               nullptr, &ctx_);
    }
    if (FAILED(hr)) {
        err = "D3D11CreateDevice failed";
        return false;
    }
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(ctx_.As(&mt))) mt->SetMultithreadProtected(TRUE);

    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    device_.As(&dxgiDev);
    dxgiDev->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIFactory5> f5;
    if (SUCCEEDED(factory.As(&f5))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow))) tearing_ = allow;
    }
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &sd, nullptr, nullptr, &swap_);
    if (FAILED(hr)) {
        err = "CreateSwapChainForHwnd failed";
        return false;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    ComPtr<IDXGISwapChain2> sc2;
    if (SUCCEEDED(swap_.As(&sc2))) {
        sc2->SetMaximumFrameLatency(1);
        waitable_ = sc2->GetFrameLatencyWaitableObject();
    }
    DXGI_SWAP_CHAIN_DESC1 got{};
    swap_->GetDesc1(&got);
    w_ = got.Width;
    h_ = got.Height;
    createBackBufferView();
    if (!createShaders(err)) return false;
    buildMatrix(false, false, colorMatrix_);
    DXGI_ADAPTER_DESC ad{};
    adapter->GetDesc(&ad);
    char name[128];
    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof name, nullptr, nullptr);
    log::info(std::string("GPU: ") + name + (tearing_ ? " (tearing supported)" : ""));
    return true;
}

void Renderer::shutdown() {
    if (waitable_) CloseHandle(waitable_), waitable_ = nullptr;
    rtv_.Reset();
    swap_.Reset();
    ctx_.Reset();
    device_.Reset();
}

void Renderer::createBackBufferView() {
    ComPtr<ID3D11Texture2D> bb;
    swap_->GetBuffer(0, IID_PPV_ARGS(&bb));
    device_->CreateRenderTargetView(bb.Get(), nullptr, &rtv_);
}

void Renderer::resize(uint32_t w, uint32_t h) {
    if (!swap_ || w == 0 || h == 0 || (w == w_ && h == h_)) return;
    std::lock_guard lk(video::d3dContextLock());
    rtv_.Reset();
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    swap_->GetDesc1(&sd);
    swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, sd.Flags);
    w_ = w;
    h_ = h;
    createBackBufferView();
}

bool Renderer::waitForNextFrame() {
    // Non-blocking: returns true only when the GPU has a free swap-chain slot.
    // A blocking wait here (the previous 100 ms timeout) stalled the UI thread and
    // starved mouse processing during firefights. With timeout 0 the UI thread
    // processes input at full rate and simply skips a render when the GPU is busy.
    if (!waitable_) return true;
    return WaitForSingleObjectEx(waitable_, 0, TRUE) == WAIT_OBJECT_0;
}

bool Renderer::createShaders(std::string& err) {
    auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
        ComPtr<ID3DBlob> errors;
        HRESULT hr = D3DCompile(kShader, std::strlen(kShader), "wasd", nullptr, nullptr, entry, target,
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &errors);
        if (FAILED(hr)) {
            err = std::string("shader compile failed: ") + (errors ? static_cast<const char*>(errors->GetBufferPointer()) : entry);
            return false;
        }
        return true;
    };
    ComPtr<ID3DBlob> vs, ps1, ps2;
    if (!compile("vs_main", "vs_5_0", vs) || !compile("ps_nv12", "ps_5_0", ps1) || !compile("ps_i420", "ps_5_0", ps2))
        return false;
    device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_);
    device_->CreatePixelShader(ps1->GetBufferPointer(), ps1->GetBufferSize(), nullptr, &psNv12_);
    device_->CreatePixelShader(ps2->GetBufferPointer(), ps2->GetBufferSize(), nullptr, &psI420_);
    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&s, &sampler_);
    D3D11_BUFFER_DESC b{};
    b.ByteWidth = 48;
    b.Usage = D3D11_USAGE_DEFAULT;
    b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    device_->CreateBuffer(&b, nullptr, &cb_);
    D3D11_RASTERIZER_DESC r{};
    r.FillMode = D3D11_FILL_SOLID;
    r.CullMode = D3D11_CULL_NONE;
    device_->CreateRasterizerState(&r, &raster_);
    return vs_ && psNv12_ && psI420_ && sampler_ && cb_;
}

bool Renderer::ensureTextures(uint32_t w, uint32_t h, bool nv12) {
    if (w == videoW_ && h == videoH_ && nv12 == nv12_ && (nv12 ? texNv12_ != nullptr : texPlanes_[0] != nullptr)) return true;
    texNv12_.Reset();
    srvY_.Reset();
    srvUV_.Reset();
    staging_.Reset();
    for (int i = 0; i < 3; ++i) texPlanes_[i].Reset(), srvPlanes_[i].Reset();
    D3D11_TEXTURE2D_DESC d{};
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (nv12) {
        d.Width = w;
        d.Height = h;
        d.Format = DXGI_FORMAT_NV12;
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &texNv12_))) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipLevels = 1;
        v.Format = DXGI_FORMAT_R8_UNORM;
        device_->CreateShaderResourceView(texNv12_.Get(), &v, &srvY_);
        v.Format = DXGI_FORMAT_R8G8_UNORM;
        device_->CreateShaderResourceView(texNv12_.Get(), &v, &srvUV_);
    } else {
        d.Format = DXGI_FORMAT_R8_UNORM;
        for (int i = 0; i < 3; ++i) {
            d.Width = i ? (w + 1) / 2 : w;
            d.Height = i ? (h + 1) / 2 : h;
            if (FAILED(device_->CreateTexture2D(&d, nullptr, &texPlanes_[i]))) return false;
            device_->CreateShaderResourceView(texPlanes_[i].Get(), nullptr, &srvPlanes_[i]);
        }
    }
    videoW_ = w;
    videoH_ = h;
    nv12_ = nv12;
    return true;
}

bool Renderer::upload(const video::DecodedFrame& df) {
    const AVFrame* f = df.frame.get();
    if (!f || f->width <= 0 || f->height <= 0) return false;
    const bool bt709 = f->colorspace == AVCOL_SPC_BT709;
    const bool full = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P;
    buildMatrix(bt709, full, colorMatrix_);
    bt709_ = bt709;
    full_ = full;
    std::lock_guard lk(video::d3dContextLock());
    if (f->format == AV_PIX_FMT_D3D11) {
        auto* src = reinterpret_cast<ID3D11Texture2D*>(f->data[0]);
        const UINT index = static_cast<UINT>(reinterpret_cast<intptr_t>(f->data[1]));
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        if (sd.Format != DXGI_FORMAT_NV12) return false;  // 10-bit (P010) not supported yet
        const uint32_t w = static_cast<uint32_t>(f->width) & ~1u, h = static_cast<uint32_t>(f->height) & ~1u;
        if (!ensureTextures(w, h, true)) return false;
        D3D11_BOX box{0, 0, 0, w, h, 1};
        ctx_->CopySubresourceRegion(texNv12_.Get(), 0, 0, 0, 0, src, index, &box);
        return true;
    }
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) return false;
    const uint32_t w = static_cast<uint32_t>(f->width), h = static_cast<uint32_t>(f->height);
    if (!ensureTextures(w, h, false)) return false;
    for (int i = 0; i < 3; ++i)
        ctx_->UpdateSubresource(texPlanes_[i].Get(), 0, nullptr, f->data[i], static_cast<UINT>(f->linesize[i]), 0);
    return true;
}

void Renderer::beginFrame() {
    std::lock_guard lk(video::d3dContextLock());
    const float bg[4] = {30.0f / 255.0f, 30.0f / 255.0f, 30.0f / 255.0f, 1.0f};  // NEXPLAY #1E1E1E
    ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    ctx_->ClearRenderTargetView(rtv_.Get(), bg);
}

void Renderer::drawVideo(const Rect& r) {
    if (!hasVideo() || r.w <= 0 || r.h <= 0) return;
    std::lock_guard lk(video::d3dContextLock());
    D3D11_VIEWPORT vp{r.x, r.y, r.w, r.h, 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetState(raster_.Get());
    ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx_->UpdateSubresource(cb_.Get(), 0, nullptr, colorMatrix_, 0, 0);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    if (nv12_) {
        ID3D11ShaderResourceView* v[2] = {srvY_.Get(), srvUV_.Get()};
        ctx_->PSSetShaderResources(0, 2, v);
        ctx_->PSSetShader(psNv12_.Get(), nullptr, 0);
    } else {
        ID3D11ShaderResourceView* v[3] = {srvPlanes_[0].Get(), srvPlanes_[1].Get(), srvPlanes_[2].Get()};
        ctx_->PSSetShaderResources(0, 3, v);
        ctx_->PSSetShader(psI420_.Get(), nullptr, 0);
    }
    ctx_->Draw(3, 0);
    ID3D11ShaderResourceView* none[3] = {};
    ctx_->PSSetShaderResources(0, 3, none);
    D3D11_VIEWPORT full{0, 0, static_cast<float>(w_), static_cast<float>(h_), 0, 1};
    ctx_->RSSetViewports(1, &full);
}

void Renderer::present(bool vsync) {
    std::lock_guard lk(video::d3dContextLock());
    const UINT flags = (!vsync && tearing_) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    swap_->Present(vsync ? 1 : 0, flags);
}

std::optional<float> Renderer::sampleLuma(Vec2 n) {
    if (!hasVideo()) return std::nullopt;
    constexpr uint32_t kBox = 16;
    std::lock_guard lk(video::d3dContextLock());
    D3D11_TEXTURE2D_DESC d{};
    d.Width = d.Height = kBox;
    d.MipLevels = d.ArraySize = 1;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.Format = nv12_ ? DXGI_FORMAT_NV12 : DXGI_FORMAT_R8_UNORM;
    if (!staging_) {
        if (FAILED(device_->CreateTexture2D(&d, nullptr, &staging_))) return std::nullopt;
    }
    if (videoW_ < kBox || videoH_ < kBox) return std::nullopt;
    auto clampU = [](float v, uint32_t maxV) {
        int64_t x = static_cast<int64_t>(v);
        return static_cast<uint32_t>(x < 0 ? 0 : (x > maxV ? maxV : x));
    };
    const uint32_t x = clampU(n.x * videoW_ - kBox / 2, videoW_ - kBox) & ~1u;
    const uint32_t y = clampU(n.y * videoH_ - kBox / 2, videoH_ - kBox) & ~1u;
    D3D11_BOX box{x, y, 0, x + kBox, y + kBox, 1};
    ID3D11Texture2D* src = nv12_ ? texNv12_.Get() : texPlanes_[0].Get();
    ctx_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, src, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m))) return std::nullopt;
    uint64_t sum = 0;
    for (uint32_t row = 0; row < kBox; ++row) {
        const auto* p = static_cast<const uint8_t*>(m.pData) + row * m.RowPitch;
        for (uint32_t col = 0; col < kBox; ++col) sum += p[col];
    }
    ctx_->Unmap(staging_.Get(), 0);
    return static_cast<float>(sum) / (kBox * kBox);
}

// ---- asynchronous region readback (color zones) ----

bool Renderer::requestRegion(int slot, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!hasVideo() || slot < 0 || slot >= kRegionSlots) return false;
    x &= ~1u, y &= ~1u, w = (w + 1) & ~1u, h = (h + 1) & ~1u;
    if (w < 2 || h < 2 || x >= videoW_ || y >= videoH_) return false;
    w = std::min(w, (videoW_ - x) & ~1u), h = std::min(h, (videoH_ - y) & ~1u);
    if (w < 2 || h < 2 || uint64_t(w) * h > (8ull << 20)) return false;  // ~8Mpx ≈ 24MB RGB24 readback
    RegionSlot& s = regions_[slot];
    std::lock_guard lk(video::d3dContextLock());
    if (s.w != w || s.h != h || s.nv12 != nv12_) {
        for (auto& t : s.tex) t.Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.MipLevels = d.ArraySize = 1;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        const int planes = nv12_ ? 1 : 3;
        for (int i = 0; i < planes; ++i) {
            d.Width = i == 0 ? w : w / 2;
            d.Height = i == 0 ? h : h / 2;
            d.Format = nv12_ ? DXGI_FORMAT_NV12 : DXGI_FORMAT_R8_UNORM;
            if (FAILED(device_->CreateTexture2D(&d, nullptr, &s.tex[i]))) return false;
        }
        s.w = w, s.h = h, s.nv12 = nv12_;
    }
    if (nv12_) {
        D3D11_BOX box{x, y, 0, x + w, y + h, 1};
        ctx_->CopySubresourceRegion(s.tex[0].Get(), 0, 0, 0, 0, texNv12_.Get(), 0, &box);
    } else {
        D3D11_BOX box{x, y, 0, x + w, y + h, 1};
        ctx_->CopySubresourceRegion(s.tex[0].Get(), 0, 0, 0, 0, texPlanes_[0].Get(), 0, &box);
        D3D11_BOX half{x / 2, y / 2, 0, (x + w) / 2, (y + h) / 2, 1};
        ctx_->CopySubresourceRegion(s.tex[1].Get(), 0, 0, 0, 0, texPlanes_[1].Get(), 0, &half);
        ctx_->CopySubresourceRegion(s.tex[2].Get(), 0, 0, 0, 0, texPlanes_[2].Get(), 0, &half);
    }
    s.x = x, s.y = y, s.pending = true;
    return true;
}

bool Renderer::pollRegion(int slot, RegionPixels& out) {
    if (slot < 0 || slot >= kRegionSlots) return false;
    RegionSlot& s = regions_[slot];
    if (!s.pending) return false;
    std::lock_guard lk(video::d3dContextLock());
    const int planes = s.nv12 ? 1 : 3;
    D3D11_MAPPED_SUBRESOURCE m[3]{};
    for (int i = 0; i < planes; ++i) {
        const HRESULT hr = ctx_->Map(s.tex[i].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m[i]);
        if (FAILED(hr)) {
            for (int k = 0; k < i; ++k) ctx_->Unmap(s.tex[k].Get(), 0);
            return false;  // DXGI_ERROR_WAS_STILL_DRAWING: try again next frame
        }
    }
    out.x = s.x, out.y = s.y, out.w = s.w, out.h = s.h;
    out.rgb.resize(size_t(s.w) * s.h * 3);
    // Same YUV -> RGB conversion the display shader uses (BT.601/709, limited/full range).
    const float kr = bt709_ ? 1.5748f : 1.402f, kgb = bt709_ ? 0.1873f : 0.344136f, kgr = bt709_ ? 0.4681f : 0.714136f,
                kb = bt709_ ? 1.8556f : 1.772f;
    const float ys = full_ ? 1.0f : 255.0f / 219.0f, yo = full_ ? 0.0f : 16.0f, cs = full_ ? 1.0f : 255.0f / 224.0f;
    auto clamp8 = [](float v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f); };
    for (uint32_t row = 0; row < s.h; ++row) {
        const uint8_t* yr = static_cast<const uint8_t*>(m[0].pData) + size_t(row) * m[0].RowPitch;
        const uint8_t *ur, *vr;
        size_t step;
        if (s.nv12) {
            ur = static_cast<const uint8_t*>(m[0].pData) + size_t(m[0].RowPitch) * s.h + size_t(row / 2) * m[0].RowPitch;
            vr = ur + 1;
            step = 2;
        } else {
            ur = static_cast<const uint8_t*>(m[1].pData) + size_t(row / 2) * m[1].RowPitch;
            vr = static_cast<const uint8_t*>(m[2].pData) + size_t(row / 2) * m[2].RowPitch;
            step = 1;
        }
        uint8_t* o = out.rgb.data() + size_t(row) * s.w * 3;
        for (uint32_t col = 0; col < s.w; ++col) {
            const float Y = (yr[col] - yo) * ys, U = (ur[(col / 2) * step] - 128.0f) * cs, V = (vr[(col / 2) * step] - 128.0f) * cs;
            o[col * 3 + 0] = clamp8(Y + kr * V);
            o[col * 3 + 1] = clamp8(Y - kgb * U - kgr * V);
            o[col * 3 + 2] = clamp8(Y + kb * U);
        }
    }
    for (int i = 0; i < planes; ++i) ctx_->Unmap(s.tex[i].Get(), 0);
    s.pending = false;
    return true;
}

}  // namespace wasd::render
