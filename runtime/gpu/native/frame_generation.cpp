// DLSS Frame Generation through NVIDIA Streamline (manual hooking): only the
// DXGI factory is upgraded to a Streamline proxy, so the swap chain Xenia's
// presenter creates is the Streamline one; everything else keeps using the
// native D3D12 interfaces. sl.interposer.dll is delay-loaded after its
// signature has been checked.
#include <sl_security.h>
#include "frame_generation.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_helpers.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace gpu::native::fg
{
namespace
{
constexpr char kProjectId[] = "c3f1a2b4-7d5e-4f60-9a1b-2e8d4c6f0b17";
const sl::ViewportHandle kViewport{0};

std::mutex g_lock;
Status g_status;
bool g_loaded = false, g_deviceReady = false;
std::atomic<int> g_frames{0};
std::atomic<bool> g_menuOpen{false};
uint32_t g_frameIndex = 0;

// Renderer-side frame (GPU command thread).
sl::FrameToken* g_renderToken = nullptr;
bool g_renderTagged = false;
// Last frame handed to the presenter, and the one being painted.
sl::FrameToken* g_readyToken = nullptr;
bool g_readyTagged = false;
uint32_t g_readyIndex = 0;
sl::FrameToken* g_paintToken = nullptr;
bool g_paintTagged = false;
uint32_t g_paintIndex = 0, g_lastPresented = UINT32_MAX;
bool g_paintPresentMarked = false;
// Options last sent to Streamline.
bool g_appliedOn = false;
int g_appliedFrames = -1;
uint32_t g_presents = 0;

void SetReason(const char* text)
{
    std::lock_guard lock(g_lock);
    snprintf(g_status.reason, sizeof(g_status.reason), "%s", text);
}

void Log(sl::LogType type, const char* message)
{
    static const bool verbose = getenv("FD_STREAMLINE_VERBOSE") != nullptr;
    if (type == sl::LogType::eInfo && !verbose) return;
    fprintf(stderr, "streamline: %s", message);
    const size_t length = strlen(message);
    if (!length || message[length - 1] != '\n') fputc('\n', stderr);
}

void Transpose(const float* in, sl::float4x4& out)
{
    for (int r = 0; r < 4; ++r)
    {
        out[r].x = in[0 * 4 + r];
        out[r].y = in[1 * 4 + r];
        out[r].z = in[2 * 4 + r];
        out[r].w = in[3 * 4 + r];
    }
}

void Identity(sl::float4x4& m)
{
    for (int r = 0; r < 4; ++r) m[r] = sl::float4(float(r == 0), float(r == 1), float(r == 2), float(r == 3));
}

bool Invert(const sl::float4x4& m, sl::float4x4& out)
{
    const float* a = &m[0].x;
    float inv[16];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (!(std::abs(det) > 1e-12f)) return false;
    float* o = &out[0].x;
    for (int i = 0; i < 16; ++i) o[i] = inv[i] / det;
    return true;
}

std::wstring ModuleDirectory()
{
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring directory(path, length);
    const size_t slash = directory.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : directory.substr(0, slash);
}

// Options for the frame about to be presented.
void ApplyOptions(bool on)
{
    int maxFrames;
    {
        std::lock_guard lock(g_lock);
        maxFrames = g_status.maxFrames;
    }
    const int frames = (std::clamp)(g_frames.load(), 1, maxFrames > 0 ? (std::min)(maxFrames, 3) : 1);
    if (on == g_appliedOn && (!on || frames == g_appliedFrames)) return;
    sl::DLSSGOptions options{};
    options.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    options.numFramesToGenerate = uint32_t(frames);
    options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    // Diagnostics: show only the generated frames (artifact inspection).
    static const bool generatedOnly = getenv("FD_FG_SHOW_GENERATED") != nullptr;
    if (generatedOnly) options.flags |= sl::DLSSGFlags::eShowOnlyInterpolatedFrame;
    if (SL_FAILED(result, slDLSSGSetOptions(kViewport, options)))
    {
        fprintf(stderr, "frame generation: slDLSSGSetOptions failed (%s)\n", sl::getResultAsStr(result));
        return;
    }
    if (on != g_appliedOn) fprintf(stderr, "frame generation: %s (%dx)\n", on ? "on" : "off", on ? frames + 1 : 1);
    g_appliedOn = on;
    g_appliedFrames = frames;
}
}

int ConfiguredFrames(int configured)
{
    if (const char* value = getenv("FD_FRAME_GENERATION"); value && *value) configured = atoi(value);
    return (std::clamp)(configured, 0, 3);
}

bool Loaded() { return g_loaded; }

bool Startup(int frames)
{
    g_frames = (std::clamp)(frames, 0, 3);
    {
        std::lock_guard lock(g_lock);
        g_status.frames = g_frames;
    }
    if (g_loaded) return true;
    const std::wstring path = ModuleDirectory() + L"\\sl.interposer.dll";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        SetReason("sl.interposer.dll em falta junto do executavel");
        return false;
    }
    if (!sl::security::verifyEmbeddedSignature(path.c_str()))
    {
        SetReason("sl.interposer.dll sem assinatura valida da NVIDIA");
        return false;
    }
    if (!LoadLibraryW(path.c_str()))
    {
        SetReason("nao foi possivel carregar o Streamline");
        return false;
    }
    static const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = getenv("FD_STREAMLINE_VERBOSE") ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    pref.logMessageCallback = Log;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = uint32_t(std::size(features));
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "1.0";
    pref.projectId = kProjectId;
    pref.renderAPI = sl::RenderAPI::eD3D12;
    if (SL_FAILED(result, slInit(pref, sl::kSDKVersion)))
    {
        char text[160];
        snprintf(text, sizeof(text), "Streamline nao iniciou (%s)", sl::getResultAsStr(result));
        SetReason(text);
        return false;
    }
    g_loaded = true;
    fprintf(stderr, "frame generation: Streamline loaded\n");
    return true;
}

void FactoryCreated(IDXGIFactory2** factory)
{
    if (!g_loaded || !factory || !*factory) return;
    void* proxy = *factory;
    if (SL_FAILED(result, slUpgradeInterface(&proxy)))
    {
        fprintf(stderr, "frame generation: slUpgradeInterface(factory) failed (%s)\n", sl::getResultAsStr(result));
        return;
    }
    *factory = static_cast<IDXGIFactory2*>(proxy);
}

void DeviceCreated(ID3D12Device* device)
{
    if (!g_loaded || !device) return;
    if (SL_FAILED(result, slSetD3DDevice(device)))
    {
        char text[160];
        snprintf(text, sizeof(text), "slSetD3DDevice falhou (%s)", sl::getResultAsStr(result));
        SetReason(text);
        return;
    }
    const LUID luid = device->GetAdapterLuid();
    sl::AdapterInfo adapter{};
    adapter.deviceLUID = (uint8_t*)&luid;
    adapter.deviceLUIDSizeInBytes = sizeof(LUID);
    if (SL_FAILED(result, slIsFeatureSupported(sl::kFeatureDLSS_G, adapter)))
    {
        char text[160];
        snprintf(text, sizeof(text), "DLSS Frame Generation indisponivel neste sistema (%s)", sl::getResultAsStr(result));
        SetReason(text);
        std::lock_guard lock(g_lock);
        g_status.available = 0;
        return;
    }
    sl::ReflexOptions reflex{};
    reflex.mode = getenv("FD_FG_REFLEX_OFF") ? sl::ReflexMode::eOff : sl::ReflexMode::eLowLatency;
    if (SL_FAILED(result, slReflexSetOptions(reflex)))
        fprintf(stderr, "frame generation: slReflexSetOptions failed (%s)\n", sl::getResultAsStr(result));
    sl::DLSSGState state{};
    if (SL_SUCCEEDED(result, slDLSSGGetState(kViewport, state, nullptr)))
    {
        std::lock_guard lock(g_lock);
        g_status.maxFrames = int(state.numFramesToGenerateMax);
    }
    g_deviceReady = true;
    std::lock_guard lock(g_lock);
    g_status.available = 1;
    g_status.reason[0] = 0;
    fprintf(stderr, "frame generation: DLSS-G ready (up to %d generated frames)\n", g_status.maxFrames);
}

bool WantsInputs() { return g_deviceReady && g_frames.load() > 0; }

void* GameFrameBegin()
{
    if (!g_deviceReady) return nullptr;
    const uint32_t index = ++g_frameIndex;
    sl::FrameToken* token = nullptr;
    if (SL_FAILED(result, slGetNewFrameToken(token, &index)) || !token) return nullptr;
    slReflexSleep(*token);
    slPCLSetMarker(sl::PCLMarker::eSimulationStart, *token);
    return token;
}

void GameFrameEnd(void* token)
{
    if (g_deviceReady && token) slPCLSetMarker(sl::PCLMarker::eSimulationEnd, *static_cast<sl::FrameToken*>(token));
}

void RenderBegin(void* token)
{
    g_renderToken = static_cast<sl::FrameToken*>(token);
    g_renderTagged = false;
    if (g_deviceReady && g_renderToken) slPCLSetMarker(sl::PCLMarker::eRenderSubmitStart, *g_renderToken);
}

void Tag(const FrameInputs& in)
{
    if (!g_deviceReady || !g_renderToken || !in.list || !in.depth || !in.motion || !WantsInputs()) return;
    sl::Constants c{};
    Transpose(in.clipToPrevClip, c.clipToPrevClip);
    if (!in.valid || !Invert(c.clipToPrevClip, c.prevClipToClip))
    {
        Identity(c.clipToPrevClip);
        Identity(c.prevClipToClip);
    }
    // The game only exposes the combined world-to-clip matrix; the camera
    // motion is in the motion vectors and clipToPrevClip.
    Identity(c.cameraViewToClip);
    Identity(c.clipToCameraView);
    Identity(c.clipToLensClip);
    c.jitterOffset = sl::float2(in.jitterX, in.jitterY);
    c.mvecScale = sl::float2(1.0f / float(in.width), 1.0f / float(in.height));
    c.cameraPinholeOffset = sl::float2(0, 0);
    c.cameraPos = sl::float3(0, 0, 0);
    c.cameraUp = sl::float3(0, 1, 0);
    c.cameraRight = sl::float3(1, 0, 0);
    c.cameraFwd = sl::float3(0, 0, 1);
    c.cameraNear = 1.0f;
    c.cameraFar = 10000.0f;
    c.cameraFOV = 0.9f;
    c.cameraAspectRatio = float(in.width) / float(in.height);
    c.depthInverted = sl::Boolean::eTrue;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = in.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.orthographicProjection = sl::Boolean::eFalse;
    c.motionVectorsDilated = sl::Boolean::eFalse;
    c.motionVectorsJittered = sl::Boolean::eFalse;
    if (SL_FAILED(result, slSetConstants(c, *g_renderToken, kViewport)))
    {
        fprintf(stderr, "frame generation: slSetConstants failed (%s)\n", sl::getResultAsStr(result));
        return;
    }
    sl::Resource depth(sl::ResourceType::eTex2d, in.depth, nullptr, nullptr, D3D12_RESOURCE_STATE_RENDER_TARGET);
    sl::Resource motion(sl::ResourceType::eTex2d, in.motion, nullptr, nullptr, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const sl::Extent extent{0, 0, in.width, in.height};
    sl::ResourceTag tags[] = {
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &extent),
        sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &extent),
    };
    if (SL_FAILED(result, slSetTagForFrame(*g_renderToken, kViewport, tags, uint32_t(std::size(tags)), in.list)))
    {
        fprintf(stderr, "frame generation: slSetTagForFrame failed (%s)\n", sl::getResultAsStr(result));
        return;
    }
    g_renderTagged = true;
    std::lock_guard lock(g_lock);
    ++g_status.taggedFrames;
}

void Delivered()
{
    if (!g_deviceReady || !g_renderToken) return;
    slPCLSetMarker(sl::PCLMarker::eRenderSubmitEnd, *g_renderToken);
    std::lock_guard lock(g_lock);
    g_readyToken = g_renderToken;
    g_readyTagged = g_renderTagged;
    g_readyIndex = uint32_t(*g_renderToken);
}

void GuestOutputPainted(ID3D12GraphicsCommandList* list, ID3D12Resource* backBuffer, int32_t x, int32_t y, uint32_t width, uint32_t height)
{
    if (!g_deviceReady) return;
    {
        std::lock_guard lock(g_lock);
        g_paintToken = g_readyToken;
        g_paintTagged = g_readyTagged && width && height;
        g_paintIndex = g_readyIndex;
    }
    if (!g_paintToken || !g_paintTagged || !list || !backBuffer) return;
    // The back buffer before the PC overlay (hudless) and the guest output
    // rectangle inside it (the motion vectors / depth cover only that part).
    const sl::Extent rect{uint32_t((std::max)(y, 0)), uint32_t((std::max)(x, 0)), width, height};
    sl::Resource hudless(sl::ResourceType::eTex2d, backBuffer, nullptr, nullptr, D3D12_RESOURCE_STATE_RENDER_TARGET);
    sl::ResourceTag tags[] = {
        sl::ResourceTag(nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle{}, &rect),
        sl::ResourceTag(&hudless, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow, &rect),
    };
    if (SL_FAILED(result, slSetTagForFrame(*g_paintToken, kViewport, tags, uint32_t(std::size(tags)), list)))
    {
        fprintf(stderr, "frame generation: back buffer tags failed (%s)\n", sl::getResultAsStr(result));
        g_paintTagged = false;
    }
}

bool BeforePresent()
{
    if (!g_deviceReady) return true;
    g_paintPresentMarked = false;
    const bool fresh = g_paintToken && g_paintIndex != g_lastPresented;
    const bool wanted = g_frames.load() > 0 && !g_menuOpen.load() && g_paintTagged;
    // A repaint of a frame already presented with frame generation on would
    // reach the pacer twice; drop it (nothing new to show). When generation is
    // no longer wanted (panel opened), repaints go through with it off.
    if (!fresh && wanted && g_appliedOn) return false;
    ApplyOptions(wanted && fresh);
    if (fresh)
    {
        slPCLSetMarker(sl::PCLMarker::ePresentStart, *g_paintToken);
        g_paintPresentMarked = true;
    }
    return true;
}

void AfterPresent()
{
    if (!g_deviceReady) return;
    if (g_paintPresentMarked)
    {
        slPCLSetMarker(sl::PCLMarker::ePresentEnd, *g_paintToken);
        g_lastPresented = g_paintIndex;
    }
    static auto windowStart = std::chrono::steady_clock::now();
    static uint32_t windowPresents = 0, windowGenerated = 0;
    ++windowPresents;
    if (++g_presents % 30 == 0)
    {
        sl::DLSSGState state{};
        if (SL_SUCCEEDED(result, slDLSSGGetState(kViewport, state, nullptr)))
        {
            std::lock_guard lock(g_lock);
            g_status.maxFrames = int(state.numFramesToGenerateMax);
            g_status.presentedPerFrame = state.numFramesActuallyPresented;
            g_status.statusFlags = uint32_t(state.status);
            windowGenerated += state.numFramesActuallyPresented;
            if (g_presents % 900 == 0 || (state.status != sl::DLSSGStatus::eOk && g_presents % 150 == 0))
                fprintf(stderr, "frame generation: %s, %u frames per present, status 0x%X, %llu tagged\n",
                    g_appliedOn ? "on" : "off", state.numFramesActuallyPresented, uint32_t(state.status),
                    (unsigned long long)g_status.taggedFrames);
        }
    }
    // Reflex latency reports: simulation start -> GPU render end of real frames.
    if (g_presents % 600 == 0)
    {
        static sl::ReflexState reflex{};
        if (SL_SUCCEEDED(result, slReflexGetState(reflex)) && reflex.latencyReportAvailable)
        {
            double total = 0, worst = 0, queue = 0;
            uint32_t count = 0;
            for (const auto& report : reflex.frameReport)
                if (report.simStartTime && report.gpuRenderEndTime > report.simStartTime && report.gpuRenderEndTime - report.simStartTime < 1000000)
                {
                    const double ms = (report.gpuRenderEndTime - report.simStartTime) / 1000.0;
                    total += ms; worst = (std::max)(worst, ms); ++count;
                    if (report.gpuRenderStartTime > report.presentEndTime && report.presentEndTime)
                        queue += (report.gpuRenderStartTime - report.presentEndTime) / 1000.0;
                }
            if (count)
                fprintf(stderr, "reflex latency: simulation start -> GPU end %.1f ms average, %.1f ms worst; present -> GPU start %.1f ms (%u frames)\n",
                    total / count, worst, queue / count, count);
        }
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - windowStart).count();
    if (seconds >= 5)
    {
        uint32_t perPresent;
        {
            std::lock_guard lock(g_lock);
            perPresent = g_status.presentedPerFrame ? g_status.presentedPerFrame : 1;
        }
        fprintf(stderr, "frame generation: %.1f game frames/s -> ~%.0f displayed/s (%s)\n", windowPresents / seconds,
            windowPresents / seconds * (g_appliedOn ? perPresent : 1), g_appliedOn ? "on" : "off");
        windowStart = now;
        windowPresents = windowGenerated = 0;
    }
    std::lock_guard lock(g_lock);
    g_status.active = g_appliedOn;
}

void SetFrames(int frames)
{
    g_frames = (std::clamp)(frames, 0, 3);
    std::lock_guard lock(g_lock);
    g_status.frames = g_frames;
}

void SetMenuOpen(bool open) { g_menuOpen = open; }

Status GetStatus()
{
    std::lock_guard lock(g_lock);
    Status status = g_status;
    status.frames = g_frames;
    if (!g_loaded && status.available < 0) status.available = -1;
    return status;
}
}
