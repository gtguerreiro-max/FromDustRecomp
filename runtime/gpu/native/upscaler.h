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
// Image quality overrides of the native renderer, applied from the next frame.
// Anisotropic filtering: maximum anisotropy for mipmapped textures (1 = the
// game's own filtering; FD_NATIVE_ANISOTROPY=<n> overrides).
void SetAnisotropySetting(uint32_t maxAnisotropy);
uint32_t ConfiguredAnisotropy();
// Shadow map resolution, a multiple of the game's 512x512 cascades (1, 2, 4;
// FD_NATIVE_SHADOW_SCALE=<n> overrides, also for replays).
void SetShadowScaleSetting(uint32_t scale);
uint32_t ConfiguredShadowScale();
// HDR output: scRGB through the presenter's FP16 swap chain. Active is decided
// once, before the presenter exists (the swap chain format); paper white and
// peak brightness (nits) apply live.
void SetHdrActive(bool active);
bool HdrActive();
void SetHdrLevels(float paperWhiteNits, float peakNits);
void HdrLevels(float& paperWhiteNits, float& peakNits);
// The window's display at start-up: 1 Windows HDR on, 0 off, -1 not checked;
// its reported peak luminance (nits, 0 when unknown).
void SetHdrDisplay(int state, float maxNits);
// World interpolation (F1 > Smooth world motion): the fixed-rate world's step
// rate comes from the game loop (0: the world steps every frame, nothing to
// interpolate). FD_NO_WORLD_INTERP=1 turns it off.
void SetWorldRateSource(double (*rate)());
void SetWorldInterpolationSetting(bool on);
double WorldInterpolationRate();
// The fixed-rate world's step rate whatever the interpolation setting (0: the
// world steps every frame).
double WorldRate();
// Water UV advection step (diagnostic, FD_DLSS_NR_SWEEP "waterfix=" / "watergain="):
// scaling to the frame rate on or off, and a gain on the step.
void SetWaterStepDiagnostic(bool scale, float gain);
void WaterStepDiagnostic(bool& scale, float& gain);
void HdrDisplay(int& state, float& maxNits);
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

// DLSS 5 3D-Guided Neural Rendering (NGX feature 18, nvngx_dlssnr.dll), on the
// finished scene before the HUD. Settings apply live (F1 > Graphics).
constexpr uint32_t kMaxNeuralPasses = 3;
struct NeuralSettings
{
    bool enabled = false;
    uint32_t model = 2;       // appearance model A / B / C (C by default)
    float intensity = 1.0f, structure = 1.0f, tone = 1.0f; // the community default (RTX Remix: 0.7 / 0.3)
    int preset = 2;           // 2: preset B (lightweight network), 0: default
    uint32_t passes = 1;      // 1..kMaxNeuralPasses sequential passes, each on the previous one's output
    int split = 0;            // diagnostic comparison: 1 right half only, 2 middle twice (before | after)
    bool operator==(const NeuralSettings&) const = default;
};
void SetNeuralSettings(const NeuralSettings& settings);
NeuralSettings GetNeuralSettings();
struct NeuralStatus
{
    int available = -1;       // -1 not checked yet, 0 no, 1 yes
    bool active = false;
    uint64_t frames = 0;
    uint32_t width = 0, height = 0;
    char reason[96] = {};
};
NeuralStatus GetNeuralStatus();
void PublishNeuralStatus(const NeuralStatus& status);
struct NeuralFrame
{
    ID3D12Resource* color = nullptr;   // NON_PIXEL_SHADER_RESOURCE, finished LDR scene
    ID3D12Resource* output = nullptr;  // UNORDERED_ACCESS, same size
    ID3D12Resource* depth = nullptr;   // NON_PIXEL_SHADER_RESOURCE, render size
    ID3D12Resource* motion = nullptr;  // NON_PIXEL_SHADER_RESOURCE, render size, render pixels
    ID3D12Resource* mask = nullptr;    // NON_PIXEL_SHADER_RESOURCE, control mask (white: full effect)
    uint32_t width = 0, height = 0, renderWidth = 0, renderHeight = 0;
    float jitterX = 0, jitterY = 0;
    bool reset = false, depthInverted = true;
    NeuralSettings settings;
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
    // DLSS 5 Neural Rendering (NGX feature 18) reported available for this application.
    bool NeuralAvailable() const { return neuralAvailable; }
    // Records the DLSS evaluation into `list`, (re)creating the feature when the
    // setup changes. The caller restores its own pipeline / descriptor state.
    bool Evaluate(ID3D12GraphicsCommandList* list, const UpscalerSetup& setup, const UpscalerFrame& frame);
    // DLSS 5 Neural Rendering on `frame` (creating the feature for its size).
    // `pass` < kMaxNeuralPasses: each pass has its own feature (its own history).
    bool EvaluateNeural(ID3D12GraphicsCommandList* list, const NeuralFrame& frame, uint32_t pass = 0);
    // Frees the neural rendering feature (its VRAM) once it is turned off; the
    // GPU must be done with it. A later evaluation creates it again.
    void ReleaseNeural();

private:
    void Release();
    bool tried = false, available = false, neuralAvailable = false;
    ID3D12Device* device = nullptr;
    ::NVSDK_NGX_Parameter* params = nullptr;
    ::NVSDK_NGX_Handle* feature = nullptr;
    struct NeuralPass
    {
        ::NVSDK_NGX_Handle* feature = nullptr;
        uint32_t width = 0, height = 0;
        int preset = -1;
        // The tuning the feature was created with (the model latches it at creation).
        uint32_t model = ~0u;
        float intensity = -1, structure = -1, tone = -1;
    } neural[kMaxNeuralPasses];
    bool neuralFailed = false;
    UpscalerSetup current{};
};

// Halton(2,3) sub-pixel jitter in [-0.5, 0.5) for frame `index`, with
// 8 * (output / render)^2 phases (at least 8, at most 64), as DLSS recommends.
void UpscalerJitter(uint64_t index, double renderScale, float& x, float& y);
}
