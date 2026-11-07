// NGX DLSS integration for the native renderer. The structure (NGX init with a
// project id, feature recreation on setup change, evaluate parameters, Halton
// jitter phases) follows ReGPU's dlss.cpp, MIT License, Copyright (c) 2026 Scott
// (https://github.com/zer0ducksgiven/ReGPU).
#include "upscaler.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#ifdef FD_DLSS_NR
#include <nvsdk_ngx_defs_dlssnr.h>
#endif

#include <algorithm>
#include <chrono>
#include <atomic>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace gpu::native
{
namespace
{
// NGX expects a GUID-shaped project id for custom engines; this one is ours.
constexpr char kProjectId[] = "c3f1a2b4-7d5e-4f60-9a1b-2e8d4c6f0b17";

float Halton(uint32_t index, uint32_t base)
{
    float f = 1, r = 0;
    for (; index; index /= base) { f /= float(base); r += f * float(index % base); }
    return r;
}
}

namespace
{
std::atomic<uint32_t> g_setting{0};
std::mutex g_statusLock;
UpscalerStatus g_status;
}

std::atomic<double> g_renderScale{1.0};
void SetRenderScaleSetting(double scale) { g_renderScale = (std::clamp)(scale, 1.0, 4.0); }
double RenderScaleSetting() { return g_renderScale.load(); }
double ConfiguredRenderScale(double configured)
{
    static const double forced = [] {
        const char* value = getenv("FD_NATIVE_SCALE");
        return value && *value ? atof(value) : 0.0;
    }();
    return forced > 0 ? (std::clamp)(forced, 0.25, 4.0) : configured;
}
namespace
{
std::atomic<uint32_t> g_anisotropy{1}, g_shadowScale{1};
uint32_t Forced(const char* name)
{
    const char* value = getenv(name);
    return value && *value ? uint32_t(atoi(value)) : 0;
}
}
void SetAnisotropySetting(uint32_t maxAnisotropy) { g_anisotropy = (std::clamp)(maxAnisotropy, 1u, 16u); }
uint32_t ConfiguredAnisotropy()
{
    static const uint32_t forced = Forced("FD_NATIVE_ANISOTROPY");
    return forced ? (std::clamp)(forced, 1u, 16u) : g_anisotropy.load();
}
void SetShadowScaleSetting(uint32_t scale) { g_shadowScale = (std::clamp)(scale, 1u, 4u); }
uint32_t ConfiguredShadowScale()
{
    static const uint32_t forced = Forced("FD_NATIVE_SHADOW_SCALE");
    return forced ? (std::clamp)(forced, 1u, 4u) : g_shadowScale.load();
}
namespace
{
std::atomic<bool> g_hdrActive{false};
std::atomic<float> g_hdrPaperWhite{200.0f}, g_hdrPeak{800.0f};
}
void SetHdrActive(bool active) { g_hdrActive = active; }
bool HdrActive() { return g_hdrActive.load(); }
void SetHdrLevels(float paperWhiteNits, float peakNits)
{
    g_hdrPaperWhite = (std::clamp)(paperWhiteNits, 80.0f, 500.0f);
    g_hdrPeak = (std::clamp)(peakNits, g_hdrPaperWhite.load(), 10000.0f);
}
void HdrLevels(float& paperWhiteNits, float& peakNits) { paperWhiteNits = g_hdrPaperWhite.load(); peakNits = g_hdrPeak.load(); }
namespace
{
std::atomic<int> g_hdrDisplay{-1};
std::atomic<float> g_hdrDisplayNits{0.0f};
}
void SetHdrDisplay(int state, float maxNits) { g_hdrDisplay = state; g_hdrDisplayNits = maxNits; }
namespace
{
std::atomic<double (*)()> g_worldRate{nullptr};
std::atomic<bool> g_worldInterpolation{true};
}
void SetWorldRateSource(double (*rate)()) { g_worldRate = rate; }
void SetWorldInterpolationSetting(bool on) { g_worldInterpolation = on; }
double WorldInterpolationRate()
{
    static const bool off = getenv("FD_NO_WORLD_INTERP") != nullptr;
    const auto rate = g_worldRate.load();
    return off || !g_worldInterpolation.load() || !rate ? 0.0 : rate();
}
namespace
{
std::atomic<bool> g_waterStepScale{true};
std::atomic<float> g_waterStepGain{1.0f};
}
void SetWaterStepDiagnostic(bool scale, float gain) { g_waterStepScale = scale; g_waterStepGain = gain; }
void WaterStepDiagnostic(bool& scale, float& gain) { scale = g_waterStepScale.load(); gain = g_waterStepGain.load(); }
double WorldRate()
{
    const auto rate = g_worldRate.load();
    return rate ? rate() : 0.0;
}
void HdrDisplay(int& state, float& maxNits) { state = g_hdrDisplay.load(); maxNits = g_hdrDisplayNits.load(); }
void SetUpscalerSetting(UpscalerMode mode) { g_setting = uint32_t(mode); }
UpscalerMode UpscalerSetting() { return UpscalerMode(g_setting.load()); }
UpscalerStatus GetUpscalerStatus() { std::lock_guard lock(g_statusLock); return g_status; }
void PublishUpscalerStatus(const UpscalerStatus& status) { std::lock_guard lock(g_statusLock); g_status = status; }

UpscalerMode ConfiguredUpscalerMode(UpscalerMode configured)
{
    // FD_UPSCALER_TOGGLE=<seconds>: diagnostic, alternates the configured mode
    // and off every that many seconds (tests switching live, as from F1).
    static const double toggle = [] { const char* v = getenv("FD_UPSCALER_TOGGLE"); return v ? atof(v) : 0.0; }();
    if (toggle > 0)
    {
        static const auto start = std::chrono::steady_clock::now();
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (int64_t(t / toggle) % 2) return UpscalerMode::Off;
    }
    const char* value = getenv("FD_NATIVE_UPSCALER");
    if (!value || !*value) return configured;
    const std::string v(value);
    if (v == "off") return UpscalerMode::Off;
    if (v == "dlaa") return UpscalerMode::Dlaa;
    if (v == "quality") return UpscalerMode::Quality;
    if (v == "balanced") return UpscalerMode::Balanced;
    if (v == "performance") return UpscalerMode::Performance;
    if (v == "ultra") return UpscalerMode::UltraPerformance;
    return configured;
}

double UpscalerRenderScale(UpscalerMode mode)
{
    switch (mode)
    {
    case UpscalerMode::Quality: return 1.0 / 1.5;
    case UpscalerMode::Balanced: return 1.0 / 1.724;
    case UpscalerMode::Performance: return 0.5;
    case UpscalerMode::UltraPerformance: return 1.0 / 3.0;
    default: return 1.0;
    }
}

const char* UpscalerModeName(UpscalerMode mode)
{
    switch (mode)
    {
    case UpscalerMode::Dlaa: return "DLAA";
    case UpscalerMode::Quality: return "DLSS Quality";
    case UpscalerMode::Balanced: return "DLSS Balanced";
    case UpscalerMode::Performance: return "DLSS Performance";
    case UpscalerMode::UltraPerformance: return "DLSS Ultra Performance";
    default: return "Off";
    }
}

void UpscalerJitter(uint64_t index, double renderScale, float& x, float& y)
{
    const double ratio = 1.0 / (std::max)(renderScale, 0.01);
    const uint32_t phases = std::clamp(uint32_t(std::ceil(8.0 * ratio * ratio)), 8u, 64u);
    const uint32_t i = uint32_t(index % phases) + 1;
    x = Halton(i, 2) - 0.5f;
    y = Halton(i, 3) - 0.5f;
}

Upscaler::~Upscaler()
{
    Release();
    ReleaseNeural();
    if (available) NVSDK_NGX_D3D12_Shutdown1(device);
}

void Upscaler::Release()
{
    if (feature) NVSDK_NGX_D3D12_ReleaseFeature(feature);
    feature = nullptr;
    current = {};
}

bool Upscaler::Initialize(ID3D12Device* d)
{
    if (tried) return available;
    tried = true;
    device = d;
    wchar_t path[MAX_PATH] = L".";
    GetTempPathW(MAX_PATH, path);
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", path, device);
    if (NVSDK_NGX_FAILED(r))
    {
        fprintf(stderr, "native upscaler: NGX init failed (%08X): needs an NVIDIA RTX GPU/driver and nvngx_dlss.dll\n", unsigned(r));
        return false;
    }
    r = NVSDK_NGX_D3D12_GetCapabilityParameters(&params);
    int supported = 0, needsUpdate = 0;
    if (NVSDK_NGX_SUCCEED(r))
    {
        NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_Available, &supported);
        NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsUpdate);
    }
    if (!supported)
    {
        fprintf(stderr, "native upscaler: DLSS not available%s\n", needsUpdate ? " (driver update required)" : "");
        NVSDK_NGX_D3D12_Shutdown1(device);
        return false;
    }
    available = true;
    fprintf(stderr, "native upscaler: DLSS ready\n");
#ifdef FD_DLSS_NR
    // DLSS 5 Neural Rendering (feature 18): availability for this application.
    {
        int nrAvailable = 0, nrNeedsDriver = 0, nrInit = 0;
        unsigned int nrMajor = 0, nrMinor = 0;
        NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_DLSSNR_Available, &nrAvailable);
        NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_DLSSNR_NeedsUpdatedDriver, &nrNeedsDriver);
        NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_DLSSNR_MinDriverVersionMajor, &nrMajor);
        NVSDK_NGX_Parameter_GetUI(params, NVSDK_NGX_Parameter_DLSSNR_MinDriverVersionMinor, &nrMinor);
        NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_DLSSNR_FeatureInitResult, &nrInit);
        neuralAvailable = nrAvailable != 0;
        fprintf(stderr, "native upscaler: DLSS neural rendering available %d, needs driver %d (min %u.%u), feature init result %08X\n",
                nrAvailable, nrNeedsDriver, nrMajor, nrMinor, unsigned(nrInit));
    }
#endif
    return true;
}

namespace
{
std::mutex g_neuralLock;
NeuralSettings g_neuralSettings;
NeuralStatus g_neuralStatus;
}
void SetNeuralSettings(const NeuralSettings& settings) { std::lock_guard lock(g_neuralLock); g_neuralSettings = settings; }
NeuralSettings GetNeuralSettings()
{
    std::lock_guard lock(g_neuralLock);
    NeuralSettings settings = g_neuralSettings;
    // FD_DLSS_NR=1 / 0 forces it on / off (tests).
    if (const char* forced = getenv("FD_DLSS_NR"); forced && *forced) settings.enabled = atoi(forced) != 0;
    // FD_DLSS_NR_PRESET=<0..7> (diagnostic): the network preset (0 default, 1 A, 2 B ...).
    if (const char* preset = getenv("FD_DLSS_NR_PRESET"); preset && *preset) settings.preset = atoi(preset);
    settings.passes = std::clamp(settings.passes, 1u, kMaxNeuralPasses);
    // FD_DLSS_NR_SPLIT=1 / 2 (diagnostic): see NeuralSettings::split.
    if (const char* split = getenv("FD_DLSS_NR_SPLIT"); split && *split) settings.split = atoi(split);
    // FD_DLSS_NR_TOGGLE=<s> (diagnostic): on / off every s seconds, to test live switching.
    static const int toggle = getenv("FD_DLSS_NR_TOGGLE") ? atoi(getenv("FD_DLSS_NR_TOGGLE")) : 0;
    if (toggle > 0)
    {
        static const auto start = std::chrono::steady_clock::now();
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
        settings.enabled = (seconds / toggle) % 2 == 0;
    }
    return settings;
}
NeuralStatus GetNeuralStatus() { std::lock_guard lock(g_neuralLock); return g_neuralStatus; }
void PublishNeuralStatus(const NeuralStatus& status) { std::lock_guard lock(g_neuralLock); g_neuralStatus = status; }

void Upscaler::ReleaseNeural()
{
#ifdef FD_DLSS_NR
    for (NeuralPass& p : neural)
        if (p.feature) NVSDK_NGX_D3D12_ReleaseFeature(p.feature);
#endif
    for (NeuralPass& p : neural) p = NeuralPass();
    neuralFailed = false;
}

bool Upscaler::EvaluateNeural(ID3D12GraphicsCommandList* list, const NeuralFrame& f, uint32_t pass)
{
#ifdef FD_DLSS_NR
    if (!available || !neuralAvailable || neuralFailed || pass >= kMaxNeuralPasses || !f.color || !f.output || !f.depth || !f.motion) return false;
    NeuralPass& state = neural[pass];
    ::NVSDK_NGX_Handle*& neuralFeature = state.feature;
    if (!neuralFeature || state.width != f.width || state.height != f.height || state.preset != f.settings.preset)
    {
        if (neuralFeature) NVSDK_NGX_D3D12_ReleaseFeature(neuralFeature);
        neuralFeature = nullptr;
        NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSSNR_Width, f.width);
        NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSSNR_Height, f.height);
        NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_DLSSNR_Hint_Render_Preset, f.settings.preset);
        const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_CreateFeature(list, NVSDK_NGX_Feature_DLSSNR, params, &neuralFeature);
        if (NVSDK_NGX_FAILED(r))
        {
            fprintf(stderr, "native upscaler: DLSS neural rendering create failed (%08X) for %ux%u\n", unsigned(r), f.width, f.height);
            neuralFeature = nullptr;
            neuralFailed = true;
            return false;
        }
        state.width = f.width; state.height = f.height; state.preset = f.settings.preset;
        fprintf(stderr, "native upscaler: DLSS neural rendering %ux%u (render %ux%u, preset %d, pass %u)\n", f.width, f.height,
                f.renderWidth, f.renderHeight, f.settings.preset, pass + 1);
    }
    NVSDK_NGX_Parameter_SetD3d12Resource(params, NVSDK_NGX_Parameter_DLSSNR_Color, f.color);
    NVSDK_NGX_Parameter_SetD3d12Resource(params, NVSDK_NGX_Parameter_DLSSNR_Output, f.output);
    NVSDK_NGX_Parameter_SetD3d12Resource(params, NVSDK_NGX_Parameter_DLSSNR_Backbuffer, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(params, NVSDK_NGX_Parameter_DLSSNR_MVec, f.motion);
    NVSDK_NGX_Parameter_SetD3d12Resource(params, NVSDK_NGX_Parameter_DLSSNR_Depth, f.depth);
    NVSDK_NGX_Parameter_SetD3d12Resource(params, NVSDK_NGX_Parameter_DLSSNR_BidirectionalDistortionField, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(params, NVSDK_NGX_Parameter_DLSSNR_ControlMask, f.mask);
    NVSDK_NGX_Parameter_SetD3d12Resource(params, NVSDK_NGX_Parameter_DLSSNR_UI, nullptr);
    NVSDK_NGX_Parameter_SetD3d12Resource(params, NVSDK_NGX_Parameter_DLSSNR_UIAlpha, nullptr);
    NVSDK_NGX_Parameter_SetF(params, NVSDK_NGX_Parameter_DLSSNR_LocalToneStrength, f.settings.tone);
    NVSDK_NGX_Parameter_SetF(params, NVSDK_NGX_Parameter_DLSSNR_LocalStructureStrength, f.settings.structure);
    NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSSNR_Style, f.settings.model);
    // The skin auto mask, combined with the (white) control mask by "min", as
    // RTX Remix does; FD_DLSS_NR_NO_AUTOMASK=1 (diagnostic) uses the control mask alone.
    static const bool autoMask = getenv("FD_DLSS_NR_NO_AUTOMASK") == nullptr;
    NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_DLSSNR_UseAutoMask, autoMask ? 1 : 0);
    NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_DLSSNR_MaskBlendMode,
                             int(autoMask ? NVSDK_NGX_DLSSNR_Mask_Blend_Mode_Min : NVSDK_NGX_DLSSNR_Mask_Blend_Mode_None));
    // -1: skin follows the local structure strength (the model's own default).
    NVSDK_NGX_Parameter_SetF(params, NVSDK_NGX_Parameter_DLSSNR_SkinStructureStrength, -1.0f);
    NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_DLSSNR_Reset, f.reset ? 1 : 0);
    NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_DLSSNR_DepthInverted, f.depthInverted ? 1 : 0);
    NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_DLSSNR_Enabled, 1);
    NVSDK_NGX_Parameter_SetI(params, NVSDK_NGX_Parameter_DLSSNR_UICorrection, 0);
    NVSDK_NGX_Parameter_SetF(params, NVSDK_NGX_Parameter_DLSSNR_Intensity, f.settings.intensity);
    NVSDK_NGX_Parameter_SetF(params, NVSDK_NGX_Parameter_DLSSNR_MVecScaleX, 1.0f);
    NVSDK_NGX_Parameter_SetF(params, NVSDK_NGX_Parameter_DLSSNR_MVecScaleY, 1.0f);
    auto rect = [&](const char* x, const char* y, const char* w, const char* h, uint32_t width, uint32_t height) {
        NVSDK_NGX_Parameter_SetUI(params, x, 0); NVSDK_NGX_Parameter_SetUI(params, y, 0);
        NVSDK_NGX_Parameter_SetUI(params, w, width); NVSDK_NGX_Parameter_SetUI(params, h, height);
    };
    rect(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseX, NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseY,
         NVSDK_NGX_Parameter_DLSSNR_ColorSubrectWidth, NVSDK_NGX_Parameter_DLSSNR_ColorSubrectHeight, f.width, f.height);
    rect(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseX, NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseY,
         NVSDK_NGX_Parameter_DLSSNR_OutputSubrectWidth, NVSDK_NGX_Parameter_DLSSNR_OutputSubrectHeight, f.width, f.height);
    rect(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseX, NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseY,
         NVSDK_NGX_Parameter_DLSSNR_MVecSubrectWidth, NVSDK_NGX_Parameter_DLSSNR_MVecSubrectHeight, f.renderWidth, f.renderHeight);
    rect(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseX, NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseY,
         NVSDK_NGX_Parameter_DLSSNR_DepthSubrectWidth, NVSDK_NGX_Parameter_DLSSNR_DepthSubrectHeight, f.renderWidth, f.renderHeight);
    rect(NVSDK_NGX_Parameter_DLSSNR_ControlMaskSubrectBaseX, NVSDK_NGX_Parameter_DLSSNR_ControlMaskSubrectBaseY,
         NVSDK_NGX_Parameter_DLSSNR_ControlMaskSubrectWidth, NVSDK_NGX_Parameter_DLSSNR_ControlMaskSubrectHeight, f.renderWidth, f.renderHeight);
    NVSDK_NGX_Parameter_SetF(params, NVSDK_NGX_Parameter_Jitter_Offset_X, f.jitterX);
    NVSDK_NGX_Parameter_SetF(params, NVSDK_NGX_Parameter_Jitter_Offset_Y, f.jitterY);
    const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_EvaluateFeature(list, neuralFeature, params, nullptr);
    if (NVSDK_NGX_FAILED(r))
    {
        static int logged = 0;
        if (logged++ < 3) fprintf(stderr, "native upscaler: DLSS neural rendering evaluate failed (%08X)\n", unsigned(r));
        return false;
    }
    return true;
#else
    (void)list; (void)f; (void)pass;
    return false;
#endif
}

bool Upscaler::Evaluate(ID3D12GraphicsCommandList* list, const UpscalerSetup& setup, const UpscalerFrame& frame)
{
    if (!available || setup.mode == UpscalerMode::Off) return false;
    if (!feature || !(setup == current))
    {
        Release();
        NVSDK_NGX_PerfQuality_Value quality = NVSDK_NGX_PerfQuality_Value_DLAA;
        switch (setup.mode)
        {
        case UpscalerMode::Quality: quality = NVSDK_NGX_PerfQuality_Value_MaxQuality; break;
        case UpscalerMode::Balanced: quality = NVSDK_NGX_PerfQuality_Value_Balanced; break;
        case UpscalerMode::Performance: quality = NVSDK_NGX_PerfQuality_Value_MaxPerf; break;
        case UpscalerMode::UltraPerformance: quality = NVSDK_NGX_PerfQuality_Value_UltraPerformance; break;
        default: break;
        }
        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature.InWidth = setup.renderWidth;
        create.Feature.InHeight = setup.renderHeight;
        create.Feature.InTargetWidth = setup.outputWidth;
        create.Feature.InTargetHeight = setup.outputHeight;
        create.Feature.InPerfQualityValue = quality;
        create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
            (setup.depthInverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0) |
            (setup.hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0);
        const NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(list, 1, 1, &feature, params, &create);
        if (NVSDK_NGX_FAILED(r))
        {
            fprintf(stderr, "native upscaler: DLSS create failed (%08X) for %ux%u -> %ux%u\n", unsigned(r),
                setup.renderWidth, setup.renderHeight, setup.outputWidth, setup.outputHeight);
            feature = nullptr;
            available = false;
            return false;
        }
        current = setup;
        fprintf(stderr, "native upscaler: %s %ux%u -> %ux%u%s\n", UpscalerModeName(setup.mode), setup.renderWidth,
            setup.renderHeight, setup.outputWidth, setup.outputHeight, setup.hdr ? " HDR" : "");
    }
    NVSDK_NGX_D3D12_DLSS_Eval_Params eval{};
    eval.Feature.pInColor = frame.color;
    eval.Feature.pInOutput = frame.output;
    eval.pInDepth = frame.depth;
    eval.pInMotionVectors = frame.motion;
    eval.InJitterOffsetX = frame.jitterX;
    eval.InJitterOffsetY = frame.jitterY;
    eval.InRenderSubrectDimensions = {setup.renderWidth, setup.renderHeight};
    eval.InReset = frame.reset ? 1 : 0;
    eval.InMVScaleX = 1.0f;
    eval.InMVScaleY = 1.0f;
    eval.InFrameTimeDeltaInMsec = frame.frameTimeMs;
    const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(list, feature, params, &eval);
    if (NVSDK_NGX_FAILED(r))
    {
        static int logged = 0;
        if (logged++ < 3) fprintf(stderr, "native upscaler: DLSS evaluate failed (%08X)\n", unsigned(r));
        return false;
    }
    return true;
}
}
