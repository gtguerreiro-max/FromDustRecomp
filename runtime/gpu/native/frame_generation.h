#pragma once
// NVIDIA DLSS Frame Generation (DLSS-G / multi frame generation) through the
// public Streamline SDK. The game renders and presents at its own 30 FPS; the
// Streamline swap chain proxy adds 1-3 generated frames between real ones from
// the scene depth and camera motion vectors the native renderer already makes
// for DLAA, and the back buffer before the PC overlay (hudless).
//
// Streamline is loaded only when the setting is on at start-up (it hooks the
// DXGI factory before the swap chain exists); the multiplier changes live.
#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct IDXGIFactory2;

namespace gpu::native::fg
{
// 0 off, 1..3 generated frames per rendered frame (2x..4x).
// FD_FRAME_GENERATION=0..3 overrides the configured value.
int ConfiguredFrames(int configured);
// Loads and initializes Streamline (call before the D3D12 device / DXGI
// factory exist). Returns false, with a reason in the status, when unavailable.
bool Startup(int frames);
bool Loaded();
// Presentation hooks (Xenia D3D12 provider / presenter).
void FactoryCreated(IDXGIFactory2** factory);
void DeviceCreated(ID3D12Device* device);
void GuestOutputPainted(ID3D12GraphicsCommandList* list, ID3D12Resource* backBuffer, int32_t x, int32_t y, uint32_t width, uint32_t height);
bool BeforePresent();
void AfterPresent();

// Renderer, once per continuous frame: RenderBegin before recording, Tag after
// the motion pass (resources in the render target state), Delivered after the
// frame has been handed to the presenter.
// Reflex / PCL frame boundaries. The game thread opens a frame (token,
// slReflexSleep, simulation start) right after handing the previous one over
// at VdSwap, so the sleep delays its input sampling; it closes the
// simulation at the next VdSwap. The render worker then renders that token.
void* GameFrameBegin();
void GameFrameEnd(void* token);
void RenderBegin(void* token);
bool WantsInputs();
struct FrameInputs
{
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* depth = nullptr;   // R32F, reversed (1 = near)
    ID3D12Resource* motion = nullptr;  // RG16F, render pixels, current -> previous
    uint32_t width = 0, height = 0;
    float clipToPrevClip[16]{};        // column-vector convention (p' = M p)
    float jitterX = 0, jitterY = 0;
    bool reset = false, valid = false;
};
void Tag(const FrameInputs& inputs);
void Delivered();

// Settings panel.
void SetFrames(int frames);
void SetMenuOpen(bool open);
struct Status
{
    int available = -1;          // -1 not loaded, 0 unavailable, 1 ready
    int frames = 0;              // requested generated frames
    int maxFrames = 0;           // supported generated frames (0 unknown)
    bool active = false;         // frame generation on for the last present
    uint32_t presentedPerFrame = 0;
    uint32_t statusFlags = 0;    // sl::DLSSGStatus
    uint64_t taggedFrames = 0;
    char reason[160] = {};
};
Status GetStatus();
}
