// Direct3D 11 presenter: flip-model swap chain with frame-latency waitable object,
// optional tearing, and NV12 (D3D11VA) / YUV420P (software) to RGB conversion in a shader.
#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/coords.h"
#include "video/decoder.h"

namespace wasd::render {

using Microsoft::WRL::ComPtr;

class Renderer {
public:
    bool init(HWND hwnd, std::string& err);
    void shutdown();
    void resize(uint32_t w, uint32_t h);

    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return ctx_.Get(); }

    // Returns true if the GPU can accept a new frame (non-blocking). When false,
    // the caller should skip rendering this iteration to avoid stalling input.
    bool waitForNextFrame();
    // Copies the decoded frame into renderer-owned textures; the frame can be freed after.
    bool upload(const video::DecodedFrame& f);
    bool hasVideo() const { return videoW_ > 0; }
    uint32_t videoWidth() const { return videoW_; }
    uint32_t videoHeight() const { return videoH_; }

    void beginFrame();                 // binds and clears the back buffer
    void drawVideo(const Rect& viewport);
    void present(bool vsync);

    // Average luma (0..255) of a small box around a normalized point in the last
    // uploaded frame. Stalls the GPU; use only while a latency probe runs.
    std::optional<float> sampleLuma(Vec2 normalized);

    // Color zones: queue a GPU copy of a pixel box, then read it back as RGB24 on a later frame
    // without stalling (poll returns false until the copy is ready).
    static constexpr int kRegionSlots = 36;  // zones + images + picks + graph probe + detect + preview + watch
    struct RegionPixels {
        uint32_t x = 0, y = 0, w = 0, h = 0;
        std::vector<uint8_t> rgb;
    };
    bool requestRegion(int slot, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    bool pollRegion(int slot, RegionPixels& out);

    bool tearingSupported() const { return tearing_; }
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }

private:
    bool createShaders(std::string& err);
    bool ensureTextures(uint32_t w, uint32_t h, bool nv12);
    void createBackBufferView();

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    HANDLE waitable_ = nullptr;
    bool tearing_ = false;
    uint32_t w_ = 0, h_ = 0;

    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> psNv12_, psI420_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11RasterizerState> raster_;

    bool nv12_ = true;
    uint32_t videoW_ = 0, videoH_ = 0;
    ComPtr<ID3D11Texture2D> texNv12_;
    ComPtr<ID3D11ShaderResourceView> srvY_, srvUV_;
    ComPtr<ID3D11Texture2D> texPlanes_[3];
    ComPtr<ID3D11ShaderResourceView> srvPlanes_[3];
    ComPtr<ID3D11Texture2D> staging_;
    float colorMatrix_[12] = {};
    bool bt709_ = false, full_ = false;
    struct RegionSlot {
        ComPtr<ID3D11Texture2D> tex[3];
        uint32_t x = 0, y = 0, w = 0, h = 0;
        bool nv12 = true, pending = false;
    };
    RegionSlot regions_[kRegionSlots];
};

}  // namespace wasd::render
