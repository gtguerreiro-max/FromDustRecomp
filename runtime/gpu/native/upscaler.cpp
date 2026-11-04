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

#include <algorithm>
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

UpscalerMode ConfiguredUpscalerMode(UpscalerMode configured)
{
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
    return true;
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
