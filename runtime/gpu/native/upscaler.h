#pragma once
// NVIDIA DLSS (DLAA at native scale, Super Resolution below it) through the
// NGX SDK. The native renderer hands over the scene colour, depth and motion
// vectors at render size and gets the anti-aliased / upscaled scene back.
// Calling the standard NGX DLSS entry points also lets third-party NGX
// injectors (for example OptiScaler) attach to the game as they do to PC titles.
#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

namespace gpu::native
{
enum class UpscalerMode : uint32_t
{
    Off = 0,
    Dlaa,
    Quality,
    Balanced,
    Performance,
    UltraPerformance,
};

// The user's setting (F1 menu), applied from the next frame.
void SetUpscalerSetting(UpscalerMode mode);
UpscalerMode UpscalerSetting();
// FD_NATIVE_UPSCALER=off|dlaa|quality|balanced|performance|ultra overrides the
// configured mode (diagnostics and bounded sessions).
UpscalerMode ConfiguredUpscalerMode(UpscalerMode configured);
// Live state for the settings UI: availability (-1 not tried, 0 no, 1 yes),
// the mode last applied and the number of evaluated frames.
struct UpscalerStatus
{
    int available = -1;
    UpscalerMode active = UpscalerMode::Off;
    uint64_t evaluatedFrames = 0;
    bool sceneFound = false, cameraFound = false;
    uint32_t renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
};
UpscalerStatus GetUpscalerStatus();
void PublishUpscalerStatus(const UpscalerStatus& status);
// Render-size fraction of the output for a mode (1 for DLAA / off).
double UpscalerRenderScale(UpscalerMode mode);
// Internal resolution of the screen-size targets (output of DLSS Super
// Resolution), applied from the next frame. FD_NATIVE_SCALE=<factor> overrides.
void SetRenderScaleSetting(double scale);
double RenderScaleSetting();
double ConfiguredRenderScale(double configured);
const char* UpscalerModeName(UpscalerMode mode);

struct UpscalerSetup
{
    uint32_t renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
    UpscalerMode mode = UpscalerMode::Off;
    bool hdr = false, depthInverted = true;
    bool operator==(const UpscalerSetup&) const = default;
};

struct UpscalerFrame
{
    ID3D12Resource* color = nullptr;   // NON_PIXEL_SHADER_RESOURCE
    ID3D12Resource* depth = nullptr;   // NON_PIXEL_SHADER_RESOURCE, R32_FLOAT
    ID3D12Resource* motion = nullptr;  // NON_PIXEL_SHADER_RESOURCE, RG16F, render pixels, current -> previous
    ID3D12Resource* output = nullptr;  // UNORDERED_ACCESS
    float jitterX = 0, jitterY = 0;    // render pixels
    float frameTimeMs = 16.7f;
    bool reset = false;
};

class Upscaler
{
public:
    ~Upscaler();
    // Initializes NGX once per device. Returns false when DLSS is unavailable
    // (no RTX GPU / driver, or nvngx_dlss.dll missing); the reason is logged once.
    bool Initialize(ID3D12Device* device);
    bool Available() const { return available; }
    bool Tried() const { return tried; }
    // Records the DLSS evaluation into `list`, (re)creating the feature when the
    // setup changes. The caller restores its own pipeline / descriptor state.
    bool Evaluate(ID3D12GraphicsCommandList* list, const UpscalerSetup& setup, const UpscalerFrame& frame);

private:
    void Release();
    bool tried = false, available = false;
    ID3D12Device* device = nullptr;
    ::NVSDK_NGX_Parameter* params = nullptr;
    ::NVSDK_NGX_Handle* feature = nullptr;
    UpscalerSetup current{};
};

// Halton(2,3) sub-pixel jitter in [-0.5, 0.5) for frame `index`, with
// 8 * (output / render)^2 phases (at least 8, at most 64), as DLSS recommends.
void UpscalerJitter(uint64_t index, double renderScale, float& x, float& y);
}
