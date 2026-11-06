// Shared native renderer; saved-frame adapter retained for fidelity regression.
#include "renderer.h"
#include "shader_catalog.h"
#include "frame.h"
#include "replay_frame.h"
#include "viewport.h"
#include "upscaler.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#include <wrl/client.h>

#include "plume_render_interface.h"
#include "plume_d3d12.h"

#if __has_include("../../../third_party/renderdoc_dl/RenderDoc_1.47_64/renderdoc_app.h")
#include "../../../third_party/renderdoc_dl/RenderDoc_1.47_64/renderdoc_app.h"
#define FD_REPLAY_RENDERDOC 1
#endif

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace plume
{
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
}

using namespace plume;
using namespace gpu::native;

namespace
{
#ifdef FD_REPLAY_RENDERDOC
RENDERDOC_API_1_1_2* StartReplayCapture()
{
    if (!getenv("FD_REPLAY_CAPTURE")) return nullptr;
    HMODULE module = GetModuleHandleA("renderdoc.dll");
    if (!module) return nullptr;
    const auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
    RENDERDOC_API_1_1_2* api = nullptr;
    if (!getApi || getApi(eRENDERDOC_API_Version_1_1_2, reinterpret_cast<void**>(&api)) != 1 || !api)
        return nullptr;
    api->StartFrameCapture(nullptr, nullptr);
    return api;
}
#endif

// FD_REPLAY_DEBUG=1: D3D12 debug layer, messages printed after pipeline creation.

bool EnableDebugLayer()
{
    if (!getenv("FD_REPLAY_DEBUG")) return true;
    static std::mutex mutex;
    static bool enabled=false;
    std::lock_guard lock(mutex);
    if(enabled)return true;
    ID3D12Debug* debug = nullptr;
    const HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    if (FAILED(hr))
    {
        fprintf(stderr, "native renderer: D3D12 debug layer unavailable (%08X); install Windows Graphics Tools\n", unsigned(hr));
        return false;
    }
    debug->EnableDebugLayer();
    debug->Release();
    enabled=true;
    return true;
}

ID3D12InfoQueue* AttachInfoQueue(RenderDevice* device)
{
    if (!getenv("FD_REPLAY_DEBUG")) return nullptr;
    ID3D12InfoQueue* infoQueue = nullptr;
    auto* d3d = static_cast<D3D12Device*>(device)->d3d;
    d3d->QueryInterface(IID_PPV_ARGS(&infoQueue));
    if (infoQueue)
    {
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
    }
    return infoQueue;
}

void PrintDebugMessages(ID3D12InfoQueue* infoQueue, uint64_t& debugErrors, uint64_t& debugWarnings, const char* context)
{
    if (!infoQueue) return;
    for (UINT64 i = 0, n = infoQueue->GetNumStoredMessages(); i < n; ++i)
    {
        SIZE_T size = 0;
        infoQueue->GetMessage(i, nullptr, &size);
        std::vector<char> buffer(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        if (SUCCEEDED(infoQueue->GetMessage(i, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
        {
            fprintf(stderr, "[d3d12 %s] %s\n", context, message->pDescription);
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) ++debugErrors;
            else ++debugWarnings;
        }
    }
    infoQueue->ClearStoredMessages();
}


std::vector<uint8_t> ReadFile(const std::string& path)
{
    std::vector<uint8_t> data;
    if (FILE* f = fopen(path.c_str(), "rb"))
    {
        fseek(f, 0, SEEK_END);
        data.resize(size_t(ftell(f)));
        fseek(f, 0, SEEK_SET);
        fread(data.data(), 1, data.size(), f);
        fclose(f);
    }
    return data;
}

std::unique_ptr<RenderTexture> CreateTexture(RenderDevice* device, const RenderTextureDesc& description)
{
    auto texture = device->createTexture(description);
    if (!texture || !static_cast<D3D12Texture*>(texture.get())->d3d)
        throw std::runtime_error("D3D12 texture creation failed");
    return texture;
}

float AsFloat(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// Shared constants layout of the patched XenosRecomp (shader_common.h).
constexpr uint32_t kSharedTextureIndex = 0;   // + dimension * 128 + slot * 4
constexpr uint32_t kSharedSamplerIndex = 384; // + slot * 4
constexpr uint32_t kSharedVertexFetch = 1024; // 32 uint4
constexpr uint32_t kSharedFetchScale = 1536; // 32 floats
constexpr uint32_t kSharedColorScale = 1664; // 4 floats
constexpr uint32_t kSharedAllBooleans = 1680; // complete 256-bit Xenos bank
constexpr uint32_t kSharedSize = 2048;

// Xenos/D3D9 comparison functions (RB_DEPTHCONTROL zfunc): never, less, equal,
// less-equal, greater, not-equal, greater-equal, always.
RenderComparisonFunction Compare(uint32_t f)
{
    static const RenderComparisonFunction table[] = {
        RenderComparisonFunction::NEVER, RenderComparisonFunction::LESS, RenderComparisonFunction::EQUAL,
        RenderComparisonFunction::LESS_EQUAL, RenderComparisonFunction::GREATER, RenderComparisonFunction::NOT_EQUAL,
        RenderComparisonFunction::GREATER_EQUAL, RenderComparisonFunction::ALWAYS};
    return table[f & 7];
}

// xenos::BlendFactor / BlendOp -> plume (RB_BLENDCONTROL fields).
RenderBlend BlendFactor(uint32_t f)
{
    switch (f)
    {
    case 0: return RenderBlend::ZERO;
    case 1: return RenderBlend::ONE;
    case 4: return RenderBlend::SRC_COLOR;
    case 5: return RenderBlend::INV_SRC_COLOR;
    case 6: return RenderBlend::SRC_ALPHA;
    case 7: return RenderBlend::INV_SRC_ALPHA;
    case 8: return RenderBlend::DEST_COLOR;
    case 9: return RenderBlend::INV_DEST_COLOR;
    case 10: return RenderBlend::DEST_ALPHA;
    case 11: return RenderBlend::INV_DEST_ALPHA;
    case 12: case 14: return RenderBlend::BLEND_FACTOR;
    case 13: case 15: return RenderBlend::INV_BLEND_FACTOR;
    case 16: return RenderBlend::SRC_ALPHA_SAT;
    default: return RenderBlend::ONE;
    }
}

RenderBlendOperation BlendOp(uint32_t op)
{
    static const RenderBlendOperation table[] = {RenderBlendOperation::ADD, RenderBlendOperation::SUBTRACT,
        RenderBlendOperation::MIN, RenderBlendOperation::MAX, RenderBlendOperation::REV_SUBTRACT};
    return op < 5 ? table[op] : RenderBlendOperation::ADD;
}

// RB_BLENDCONTROL0 + RB_COLOR_MASK (RT0 bits 0-3). Blending is off when it is a
// plain copy (ONE, ZERO, ADD for colour and alpha), as in Xenia.
RenderBlendDesc Blend(uint32_t control, uint32_t mask)
{
    RenderBlendDesc b;
    b.srcBlend = BlendFactor(control & 0x1F);
    b.blendOp = BlendOp((control >> 5) & 7);
    b.dstBlend = BlendFactor((control >> 8) & 0x1F);
    b.srcBlendAlpha = BlendFactor((control >> 16) & 0x1F);
    b.blendOpAlpha = BlendOp((control >> 21) & 7);
    b.dstBlendAlpha = BlendFactor((control >> 24) & 0x1F);
    b.blendEnabled = (control & 0x1F1F1FFF) != 0x00010001;
    b.renderTargetWriteMask = uint8_t(mask & 0xF);
    return b;
}

// xenos::ColorRenderTargetFormat (RB_COLOR_INFO bits 16-19) -> host format.
// Xenos fixed16 targets have a [-32, 32] range. SNORM uses
// hardware fixed-point quantization, with reciprocal scales at writes/fetches.
RenderFormat ColorFormat(uint32_t colorInfo,bool fixedFloat32)
{
    switch ((colorInfo >> 16) & 0xF)
    {
    case 5: return fixedFloat32 ? RenderFormat::R32G32B32A32_FLOAT : RenderFormat::R16G16B16A16_SNORM;
    case 4: return fixedFloat32 ? RenderFormat::R32G32_FLOAT : RenderFormat::R16G16_SNORM;
    case 2: case 10: case 3: case 12: case 7: return RenderFormat::R16G16B16A16_FLOAT; // no 10-bit format in plume
    case 6: return RenderFormat::R16G16_FLOAT;
    case 14: return RenderFormat::R32_FLOAT;
    case 15: return RenderFormat::R32G32_FLOAT;
    default: return RenderFormat::R8G8B8A8_UNORM;
    }
}

bool FixedSnorm(RenderFormat format)
{
    return format == RenderFormat::R16G16_SNORM || format == RenderFormat::R16G16B16A16_SNORM;
}

uint32_t FormatBytes(RenderFormat f)
{
    switch (f)
    {
    case RenderFormat::R16G16B16A16_FLOAT: case RenderFormat::R16G16B16A16_SNORM:
    case RenderFormat::R32G32_FLOAT: return 8;
    case RenderFormat::R32G32B32A32_FLOAT: return 16;
    default: return 4;
    }
}

float HalfToFloat(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
    float v = exponent == 0 ? std::ldexp(float(mantissa), -24)
        : exponent == 31 ? 65504.0f : std::ldexp(float(mantissa | 0x400), int(exponent) - 25);
    return sign ? -v : v;
}

// One pixel of a target as RGBA8 for the PNG (values clamped to 0-1).
void ToRgba8(RenderFormat f, const uint8_t* src, uint8_t* dst)
{
    auto unit = [](float v) { return uint8_t(std::lround((std::clamp)(v, 0.0f, 1.0f) * 255.0f)); };
    uint16_t h[4];
    float x[2];
    switch (f)
    {
    case RenderFormat::R16G16_SNORM:
    case RenderFormat::R16G16B16A16_SNORM:
    {
        int16_t v[4] = {};
        const int count = f == RenderFormat::R16G16_SNORM ? 2 : 4;
        memcpy(v, src, count * 2);
        dst[0] = dst[1] = dst[2] = 0;
        dst[3] = 255;
        for (int c = 0; c < count; ++c) dst[c] = unit((std::max)(-1.0f, float(v[c]) / 32767.0f) * 32.0f);
        break;
    }
    case RenderFormat::R32G32B32A32_FLOAT:
        for (int c = 0; c < 4; ++c) { float v; memcpy(&v, src + 4 * c, 4); dst[c] = unit(v); }
        break;
    case RenderFormat::R16G16B16A16_FLOAT:
        memcpy(h, src, 8);
        for (int c = 0; c < 4; ++c) dst[c] = unit(HalfToFloat(h[c]));
        break;
    case RenderFormat::R16G16_FLOAT:
        memcpy(h, src, 4);
        dst[0] = unit(HalfToFloat(h[0])), dst[1] = unit(HalfToFloat(h[1])), dst[2] = 0, dst[3] = 255;
        break;
    case RenderFormat::R32_FLOAT:
        memcpy(x, src, 4);
        dst[0] = dst[1] = dst[2] = unit(x[0]), dst[3] = 255;
        break;
    case RenderFormat::R32G32_FLOAT:
        memcpy(x, src, 8);
        dst[0] = unit(x[0]), dst[1] = unit(x[1]), dst[2] = 0, dst[3] = 255;
        break;
    default:
        memcpy(dst, src, 4);
    }
}

// RB_COLOR_INFO of RT k (0-3).
uint32_t ColorInfo(const uint32_t* regs, uint32_t k)
{
    static const int index[] = {RB_COLOR_INFO, RB_COLOR1_INFO, RB_COLOR2_INFO, RB_COLOR3_INFO};
    return regs[index[k]];
}

uint32_t BlendControl(const uint32_t* regs, uint32_t k)
{
    static const int index[] = {RB_BLENDCONTROL0, RB_BLENDCONTROL1, RB_BLENDCONTROL2, RB_BLENDCONTROL3};
    return regs[index[k]];
}

// Fetch constant swizzle (4 x 3 bits: 0-3 XYZW, 4 zero, 5 one) as a view mapping.
RenderComponentMapping SwizzleMapping(uint32_t w)
{
    static const RenderSwizzle channel[] = {RenderSwizzle::R, RenderSwizzle::G, RenderSwizzle::B,
        RenderSwizzle::A, RenderSwizzle::ZERO, RenderSwizzle::ONE, RenderSwizzle::ZERO, RenderSwizzle::ZERO};
    return RenderComponentMapping(channel[w & 7], channel[(w >> 3) & 7], channel[(w >> 6) & 7], channel[(w >> 9) & 7]);
}

constexpr uint32_t kIdentitySwizzle = 0 | 1 << 3 | 2 << 6 | 3 << 9;

// One color render target (MRT slot) of a target configuration.
struct Surface
{
    RenderFormat format = RenderFormat::R8G8B8A8_UNORM;
    std::unique_ptr<RenderTexture> texture;
    std::unique_ptr<RenderTexture> multisample; // colour attachment; texture retains its resolved view
    std::unique_ptr<RenderBuffer> readback;
    uint32_t rowPitch = 0;
    uint32_t descriptor = 0;      // index in the Texture2D heap (resolved reads)
    std::map<uint32_t, std::pair<uint32_t, std::unique_ptr<RenderTextureView>>> views; // swizzle -> heap index, view
    bool shaderRead = false;      // texture currently in SHADER_READ
    bool isDepth = false;         // depth surface: written as D32_FLOAT, read as R32_FLOAT
    bool explicitlyCleared = false;
    uint32_t samples=1;
    std::unique_ptr<RenderTextureView> plainView;
};

// A render target configuration of the frame (EDRAM color/depth bases + pitch).
struct Target
{
    uint32_t width = 0, height = 0, count = 1, draws = 0; // host size
    uint32_t guestWidth = 0, guestHeight = 0;             // guest (EDRAM) size
    double scale = 1;                                     // host / guest (internal resolution)
    bool hadContents=false;
    uint64_t lastWrite=0;
    std::shared_ptr<Surface> rt[4];
    Surface* ds = nullptr;                      // shared EDRAM depth surface
    std::unique_ptr<RenderTextureView> dsv;    // D32_FLOAT view for the framebuffer
    std::unique_ptr<RenderFramebuffer> framebuffer;
};

using SurfaceKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
using TargetKey = std::tuple<uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t>; // ..., scale * 1000
using DepthKey = std::tuple<uint32_t,uint32_t,uint32_t,uint32_t,uint32_t>;
using ColorKey = std::tuple<uint32_t,uint32_t,uint32_t,uint32_t>;
struct Snapshot
{
    RenderFormat format=RenderFormat::UNKNOWN;
    uint32_t width=0,height=0,mipCount=1;
    Surface surface;
    std::unique_ptr<RenderFramebuffer> framebuffer;
};
struct Packed
{
    uint32_t width=0,height=0;
    Surface surface;
    std::unique_ptr<RenderTextureView> depthView;
    std::unique_ptr<RenderFramebuffer> framebuffer;
};
struct FrameCache
{
    struct StaticTexture
    {
        std::shared_ptr<RenderTexture> texture,cube,volume;
        std::shared_ptr<RenderTextureView> cubeView;
        std::tuple<uint32_t,uint32_t,uint32_t,RenderFormat> shape{}; // texture: width, height, mips, format
    };
    std::map<uint64_t,StaticTexture> staticTextures;
    std::shared_ptr<RenderTexture> blackCube;
    std::shared_ptr<RenderTextureView> blackCubeView;
    std::unique_ptr<RenderPipelineLayout> layout;
    std::map<DepthKey,Surface> depthSurfaces;
    std::map<ColorKey,std::shared_ptr<Surface>> colorSurfaces;
    std::map<TargetKey,Target> targets;
    std::map<uint32_t,Surface*> resolved,resolvedDepth;
    std::map<uint32_t,bool> resolvedSwap;
    std::map<uint32_t,int32_t> resolvedBias;
    std::map<uint32_t,std::unique_ptr<Snapshot>> snapshots,depthSnapshots;
    std::map<uint32_t,std::unique_ptr<Packed>> packed;
    std::map<std::string,std::pair<std::unique_ptr<RenderShader>,std::unique_ptr<RenderPipeline>>> pipelines;
    std::map<std::string,std::unique_ptr<RenderShader>> pixelShaders;
    uint64_t writeSerial=0;
    std::unique_ptr<RenderShader> packVs,packPs,floatPs;
    std::unique_ptr<RenderPipeline> packPipeline,floatPipeline;
    std::unique_ptr<RenderBuffer> vertexBuffer,indexBuffer,constantBuffer,streamBuffer,packConstants;
    uint64_t vertexCapacity=0,indexCapacity=0,constantCapacity=0,streamCapacity=0;
    // Continuous frames: textures of retired versions with the same shape
    // are reused for new versions (the water changes every frame), and the
    // texture staging buffer persists. Every frame completes on the GPU
    // before the next one starts, so both are idle when reused.
    std::multimap<std::tuple<uint32_t,uint32_t,uint32_t,RenderFormat>,std::shared_ptr<RenderTexture>> freeTextures;
    std::unique_ptr<RenderBuffer> textureStaging;
    size_t textureStagingCapacity=0;
    // HDR: the final composite's target right after the colour grading pass,
    // before the HUD (present_hdr.hlsl compares the final image with it).
    std::unique_ptr<RenderTexture> hudless;
    uint32_t hudlessWidth=0,hudlessHeight=0;
    RenderFormat hudlessFormat=RenderFormat::UNKNOWN;
};

// Row-major 4x4 inverse (cofactor expansion); false when singular.
bool Invert4x4(const float* m,float* out)
{
    double inv[16],a[16];
    for(int i=0;i<16;++i)a[i]=m[i];
    inv[0]=a[5]*a[10]*a[15]-a[5]*a[11]*a[14]-a[9]*a[6]*a[15]+a[9]*a[7]*a[14]+a[13]*a[6]*a[11]-a[13]*a[7]*a[10];
    inv[4]=-a[4]*a[10]*a[15]+a[4]*a[11]*a[14]+a[8]*a[6]*a[15]-a[8]*a[7]*a[14]-a[12]*a[6]*a[11]+a[12]*a[7]*a[10];
    inv[8]=a[4]*a[9]*a[15]-a[4]*a[11]*a[13]-a[8]*a[5]*a[15]+a[8]*a[7]*a[13]+a[12]*a[5]*a[11]-a[12]*a[7]*a[9];
    inv[12]=-a[4]*a[9]*a[14]+a[4]*a[10]*a[13]+a[8]*a[5]*a[14]-a[8]*a[6]*a[13]-a[12]*a[5]*a[10]+a[12]*a[6]*a[9];
    inv[1]=-a[1]*a[10]*a[15]+a[1]*a[11]*a[14]+a[9]*a[2]*a[15]-a[9]*a[3]*a[14]-a[13]*a[2]*a[11]+a[13]*a[3]*a[10];
    inv[5]=a[0]*a[10]*a[15]-a[0]*a[11]*a[14]-a[8]*a[2]*a[15]+a[8]*a[3]*a[14]+a[12]*a[2]*a[11]-a[12]*a[3]*a[10];
    inv[9]=-a[0]*a[9]*a[15]+a[0]*a[11]*a[13]+a[8]*a[1]*a[15]-a[8]*a[3]*a[13]-a[12]*a[1]*a[11]+a[12]*a[3]*a[9];
    inv[13]=a[0]*a[9]*a[14]-a[0]*a[10]*a[13]-a[8]*a[1]*a[14]+a[8]*a[2]*a[13]+a[12]*a[1]*a[10]-a[12]*a[2]*a[9];
    inv[2]=a[1]*a[6]*a[15]-a[1]*a[7]*a[14]-a[5]*a[2]*a[15]+a[5]*a[3]*a[14]+a[13]*a[2]*a[7]-a[13]*a[3]*a[6];
    inv[6]=-a[0]*a[6]*a[15]+a[0]*a[7]*a[14]+a[4]*a[2]*a[15]-a[4]*a[3]*a[14]-a[12]*a[2]*a[7]+a[12]*a[3]*a[6];
    inv[10]=a[0]*a[5]*a[15]-a[0]*a[7]*a[13]-a[4]*a[1]*a[15]+a[4]*a[3]*a[13]+a[12]*a[1]*a[7]-a[12]*a[3]*a[5];
    inv[14]=-a[0]*a[5]*a[14]+a[0]*a[6]*a[13]+a[4]*a[1]*a[14]-a[4]*a[2]*a[13]-a[12]*a[1]*a[6]+a[12]*a[2]*a[5];
    inv[3]=-a[1]*a[6]*a[11]+a[1]*a[7]*a[10]+a[5]*a[2]*a[11]-a[5]*a[3]*a[10]-a[9]*a[2]*a[7]+a[9]*a[3]*a[6];
    inv[7]=a[0]*a[6]*a[11]-a[0]*a[7]*a[10]-a[4]*a[2]*a[11]+a[4]*a[3]*a[10]+a[8]*a[2]*a[7]-a[8]*a[3]*a[6];
    inv[11]=-a[0]*a[5]*a[11]+a[0]*a[7]*a[9]+a[4]*a[1]*a[11]-a[4]*a[3]*a[9]-a[8]*a[1]*a[7]+a[8]*a[3]*a[5];
    inv[15]=a[0]*a[5]*a[10]-a[0]*a[6]*a[9]-a[4]*a[1]*a[10]+a[4]*a[2]*a[9]+a[8]*a[1]*a[6]-a[8]*a[2]*a[5];
    const double det=a[0]*inv[0]+a[1]*inv[4]+a[2]*inv[8]+a[3]*inv[12];
    if(!(std::fabs(det)>1e-30))return false;
    for(int i=0;i<16;++i)out[i]=float(inv[i]/det);
    return true;
}
void Multiply4x4(const float* a,const float* b,float* out)
{
    for(int r=0;r<4;++r)for(int c=0;c<4;++c)
    {
        double v=0;for(int k=0;k<4;++k)v+=double(a[r*4+k])*b[k*4+c];
        out[r*4+c]=float(v);
    }
}

// Diagnostic environment switches on per-draw / per-copy paths, read once.
const char* CachedEnv(const char* name)
{
    static std::mutex lock;
    static std::map<std::string, const char*> values;
    std::lock_guard guard(lock);
    auto [at, inserted] = values.try_emplace(name, nullptr);
    if (inserted) at->second = getenv(name);
    return at->second;
}

RenderMultisampling Multisampling(uint32_t surface,bool noMsaa,bool xenosPattern)
{
    const uint32_t count = noMsaa ? 1u : 1u << ((surface >> 16) & 3);
    RenderMultisampling samples(count);
    // Match the reference backend's default host sample pattern. The Xenos
    // programmable pattern is an explicit diagnostic in both renderers.
    if (!xenosPattern) return samples;
    if (count == 2)
    {
        // Xenia's native 2x host mapping is guest sample XOR 1.
        samples.sampleLocationsEnabled = true;
        samples.sampleLocations[0] = {4, -4};
        samples.sampleLocations[1] = {-4, 4};
    }
    else if (count == 4)
    {
        samples.sampleLocationsEnabled = true;
        samples.sampleLocations[0] = {-2, -2};
        samples.sampleLocations[1] = {2, 2};
        samples.sampleLocations[2] = {-6, 6};
        samples.sampleLocations[3] = {6, -6};
    }
    return samples;
}
}

#include "depth_vs.h"
#include "depth_ps.h"
#include "depth_float.h"
#include "present.h"
#include "present_hdr.h"
#include "motion_vs.h"
#include "motion_ps.h"
#include "frame_generation.h"

struct gpu::native::Renderer::Impl
{
    std::unique_ptr<ShaderCatalog> shaderCatalog;
    std::unique_ptr<RenderInterface> api;
    std::unique_ptr<RenderDevice> device;
    std::unique_ptr<RenderCommandQueue> queue;
    std::unique_ptr<RenderCommandList> list;
    // Texture uploads: their own list, so the frame's list can be recorded
    // while the GPU copies (the queue orders the two; the frame's fence wait
    // covers both before the next frame resets them).
    std::unique_ptr<RenderCommandList> uploadList;
    std::unique_ptr<RenderCommandFence> fence;
    std::unique_ptr<FrameCache> frameCache;
    double profileMs[5]{};
    double profileResMs[5]{};
    double profileDrawMs[4]{};
    double profileCopyMs[2]{}; // vertex + index copies, constant copies
    uint32_t profileFrames=0;
    Microsoft::WRL::ComPtr<ID3D12Device> externalDevice;
    D3D12Texture* outputTexture=nullptr;
    uint32_t outputWidth=0,outputHeight=0;
    D3D12Texture* outputHudless=nullptr; // this frame's scene before the HUD (HDR), or null
    // Continuous frames: the GPU finishes a frame while the worker presents it
    // and prepares the next one; the CPU wait for it moves to the start of the
    // next RenderFrame (FD_NATIVE_FRAME_WAIT=1 waits at the end, as before).
    // What the frame's commands reference stays alive here until that wait.
    struct InFlight
    {
        std::unique_ptr<RenderDescriptorSet> sets[5];
        std::map<uint64_t, std::pair<uint32_t, std::unique_ptr<RenderSampler>>> samplers;
        std::vector<std::shared_ptr<RenderTexture>> textures, cubeTextures, volumeTextures;
        std::vector<std::shared_ptr<RenderTextureView>> cubeViews;
        std::vector<std::unique_ptr<RenderTextureView>> volumeViews;
        std::map<uint64_t, std::pair<uint32_t, std::unique_ptr<RenderTextureView>>> tableViews;
        std::unique_ptr<RenderBuffer> staging, cubeStaging, volumeStaging;
        std::vector<std::unique_ptr<Packed>> retiredPacked;
        std::vector<std::unique_ptr<Snapshot>> retiredDepthSnapshots, retiredSnapshots;
        std::unique_ptr<RenderShader> pixelShader;
    };
    std::unique_ptr<InFlight> inFlight;
    // The last submitted frame's fence value: the presenter's copy waits for
    // it on the GPU.
    ID3D12Fence* frameFence=nullptr;
    uint64_t frameFenceValue=0;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> presentHdrRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> presentHdrPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> presentRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> presentPipeline;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> presentHeap;
    // A ring of copy recordings: with VSync the presenter's queue runs the
    // copy behind queued flips, and waiting for the previous one to reuse a
    // single allocator blocked the render worker for most of a refresh.
    static constexpr uint32_t kPresentSlots=3;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> presentAllocators[kPresentSlots];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> presentLists[kPresentSlots];
    uint64_t presentSlotSerials[kPresentSlots]{};
    Microsoft::WRL::ComPtr<ID3D12Fence> presentFence;
    uint64_t presentSerial=0;
    bool PresentTo(ID3D12Resource* destination,ID3D12CommandQueue* destinationQueue);
    ID3D12InfoQueue* infoQueue = nullptr;
    uint64_t debugErrors = 0, debugWarnings = 0, completedFrames = 0, completedDraws = 0;
    // DLAA / DLSS on the completed scene (continuous frames only).
    struct UpscaleState
    {
        Upscaler ngx;
        std::unique_ptr<RenderTexture> motion, depth, output;
        Surface upscaled;                 // DLSS Super Resolution output (replaces the scene copy)
        uint32_t outWidth = 0, outHeight = 0;
        double lastRenderScale = 1, lastOutputScale = 1;
        uint32_t lastShadowScale = 1;
        std::map<std::string, int> shadowRoles; // shader -> 1 shadow caster, 2 VSM pass, 0 other
        std::map<std::string, bool> gradingShaders; // PS -> the game's final colour grading (samp_ColoremapR)
        std::unique_ptr<RenderFramebuffer> framebuffer;
        std::unique_ptr<RenderShader> vs, ps;
        std::unique_ptr<RenderPipeline> pipeline;
        std::unique_ptr<RenderBuffer> constants;
        uint32_t width = 0, height = 0;
        RenderFormat outputFormat = RenderFormat::UNKNOWN;
        float previousVP[16]{}, previousViewport[4]{};
        bool hasPrevious = false, reset = true;
        uint64_t frameIndex = 0, evaluated = 0;
        std::chrono::steady_clock::time_point last{};
        std::map<std::string, bool> cameraShaders; // VS name -> writes oPos with VS_DustConst[8..11]
    } upscale;

    ~Impl() { if (infoQueue) infoQueue->Release(); }
    bool Initialize()
    {
        if (device && queue && list && fence) return true;
        if (!EnableDebugLayer()) return false;
        auto newApi = CreateD3D12Interface();
        std::unique_ptr<RenderDevice> newDevice;
        if(newApi)newDevice=externalDevice?std::make_unique<D3D12Device>(static_cast<D3D12Interface*>(newApi.get()),"",externalDevice.Get()):newApi->createDevice();
        if (!newDevice) return false;
        auto* newInfoQueue = AttachInfoQueue(newDevice.get());
        if (getenv("FD_REPLAY_DEBUG"))
        {
            if (!newInfoQueue) return false;
            fprintf(stderr, "native renderer: D3D12 debug layer active\n");
        }
        auto newQueue = newDevice->createCommandQueue(RenderCommandListType::DIRECT);
        auto newList = newQueue ? newQueue->createCommandList() : nullptr;
        auto newUploadList = newQueue ? newQueue->createCommandList() : nullptr;
        auto newFence = newDevice->createCommandFence();
        if (!newQueue || !newList || !newUploadList || !newFence)
        {
            if (newInfoQueue) newInfoQueue->Release();
            return false;
        }
        api = std::move(newApi);
        device = std::move(newDevice);
        queue = std::move(newQueue);
        uploadList.reset();
        list = std::move(newList);
        uploadList = std::move(newUploadList);
        fence = std::move(newFence);
        infoQueue = newInfoQueue;
        return true;
    }
    int RenderFrame(const FrameInput& frame, const std::string& dir, FrameOutput* output);
    std::vector<uint8_t> ReadDrawShader(const std::string& dir, const std::string& name,
        ShaderStage stage, bool source = false)
    {
        if (!shaderCatalog)
            return ReadFile(dir + "/" + name + (stage == ShaderStage::Vertex ? ".vs" : ".ps") + (source ? ".hlsl" : ".dxil"));
        const auto* shader = shaderCatalog->Find(name, stage);
        if (!shader) throw std::runtime_error("Missing native catalog shader: " + name);
        if (source) return {shader->source.begin(), shader->source.end()};
        return shader->dxil;
    }
};

int gpu::native::Renderer::Impl::RenderFrame(const FrameInput& frame, const std::string& dir, FrameOutput* output)
{
    // The previous continuous frame must be complete before its buffers are
    // rewritten and what it referenced is released.
    if(inFlight){queue->waitForCommandFence(fence.get());inFlight.reset();}
    const bool profile=frame.continuous&&getenv("FD_NATIVE_PROFILE");
    const auto profileStart=std::chrono::steady_clock::now();
    // Diagnostic environment options are stable during one submission. Avoid
    // taking the CRT environment lock again for every shader texture slot.
    const bool noColorBias=getenv("FD_NO_COLOR_BIAS")!=nullptr;
    const char* staticDestOption=getenv("FD_STATIC_DEST");
    const uint32_t staticDest=staticDestOption?uint32_t(strtoul(staticDestOption,nullptr,16)):0;
    const char* tracePSOption=getenv("FD_TRACE_PS");
    const std::string tracePS=tracePSOption?","+std::string(tracePSOption)+",":"";
    const char* traceDrawOption=getenv("FD_TRACE_DRAWS");
    const size_t traceDrawThrough=traceDrawOption?size_t(atoi(traceDrawOption)):0;
    const bool fixedFloat32=getenv("FD_FIXED_FLOAT32")!=nullptr;
    const bool noMsaa=getenv("FD_NO_MSAA")!=nullptr,xenosPattern=getenv("FD_XENOS_SAMPLE_POSITIONS")!=nullptr;
    const auto colorFormat=[&](uint32_t info){return ColorFormat(info,fixedFloat32);};
    const auto multisampling=[&](uint32_t surface){return Multisampling(surface,noMsaa,xenosPattern);};
    debugErrors = 0;
    debugWarnings = 0;
    if (output) *output = {};
    outputTexture=nullptr;outputWidth=outputHeight=0;outputHudless=nullptr;
    const auto draws = frame.draws;
    if (dir.empty() && getenv("FD_REPLAY_TEST")) throw std::runtime_error("Shader-file diagnostic requires an asset directory");
    if (!shaderCatalog && dir.empty()) throw std::runtime_error("In-memory rendering requires a shader catalog");
    if (draws.empty()) throw std::runtime_error("Empty or invalid replay");
    for (const auto& draw : draws)
        if (!draw.clear && !draw.resolve &&
            ((!draw.indices && draw.indexBytes) || (!draw.vertices && draw.vertexBytes) ||
             !draw.constants || (!draw.ps.empty() && !draw.psConstants) || draw.primitive != 4))
            throw std::runtime_error("Incomplete native draw data");
    if (shaderCatalog)
        for (const auto& draw : draws)
        {
            if (draw.clear || draw.resolve) continue;
            if (!shaderCatalog->Find(draw.vs, ShaderStage::Vertex))
                throw std::runtime_error("Missing native catalog vertex shader: " + draw.vs);
            if (!draw.ps.empty() && !shaderCatalog->Find(draw.ps, ShaderStage::Pixel))
                throw std::runtime_error("Missing native catalog pixel shader: " + draw.ps);
        }
    for (const auto& draw : draws)
        if (!draw.clear && !draw.resolve && draw.indexCount > draw.indexBytes / (draw.index32 ? 4u : 2u))
        {
            fprintf(stderr, "fd_replay: insufficient index data for VS %s; repack the capture\n", draw.vs.c_str());
            return 1;
        }

    if (!Initialize())
    {
        fprintf(stderr, "native renderer: D3D12 initialization failed\n");
        return 1;
    }
    if(!frame.continuous||!frameCache)frameCache=std::make_unique<FrameCache>();
    auto& storage=*frameCache;
    // Root constant buffers as XenosRecomp's D3D12 shaders expect them:
    // b0 vertex constants, b1 pixel constants, b2 shared constants (space 4).
    const RenderRootDescriptorDesc roots[] = {
        {0, 4, RenderRootDescriptorType::CONSTANT_BUFFER},
        {1, 4, RenderRootDescriptorType::CONSTANT_BUFFER},
        {2, 4, RenderRootDescriptorType::CONSTANT_BUFFER},
        {0, 5, RenderRootDescriptorType::SHADER_RESOURCE}, // g_VertexStreams (ByteAddressBuffer)
    };
    // Bindless heaps of XenosRecomp shaders: Texture2D[] space0, Texture3D[]
    // space1, TextureCube[] space2, SamplerState[] space3 (plume: set = space).
    std::vector<uint8_t> textureFile(frame.textures.begin(), frame.textures.end());
    uint32_t textureCount = 0;
    if (textureFile.size() >= 4) memcpy(&textureCount, textureFile.data(), 4);
    const bool liveResources=!frame.resources.empty();
    std::vector<uint64_t> versions(frame.textureVersions.begin(),frame.textureVersions.end());
    if(liveResources){textureCount=uint32_t(frame.resources.size());versions.clear();for(const auto& resource:frame.resources){if(!resource)throw std::runtime_error("Null native texture resource");versions.push_back(resource->version);}}
    // RenderDescriptorRange(type, binding, count).
    constexpr uint32_t kMaxTargets = 2048; // heap slots after the table for target and swizzled views
    const uint32_t descriptorBase=frame.continuous?8192:textureCount;
    if(textureCount>descriptorBase)throw std::runtime_error("Native texture descriptor capacity exceeded");
    const RenderDescriptorRange textureRange(RenderDescriptorRangeType::TEXTURE, 0, descriptorBase + kMaxTargets);
    constexpr uint32_t kMaxSamplers = 256;
    const RenderDescriptorRange samplerRange(RenderDescriptorRangeType::SAMPLER, 0, kMaxSamplers);
    const RenderDescriptorSetDesc setDescs[] = {
        RenderDescriptorSetDesc(&textureRange, 1, true, descriptorBase + kMaxTargets),
        RenderDescriptorSetDesc(&textureRange, 1, true, descriptorBase + kMaxTargets),
        RenderDescriptorSetDesc(&textureRange, 1, true, descriptorBase + kMaxTargets),
        RenderDescriptorSetDesc(&samplerRange, 1, true, kMaxSamplers),
        RenderDescriptorSetDesc(&textureRange, 1, true, descriptorBase + kMaxTargets), // Texture2DMS depth, space4
    };
    RenderPipelineLayoutDesc layoutDesc;
    layoutDesc.rootDescriptorDescs = roots;
    layoutDesc.rootDescriptorDescsCount = 4;
    layoutDesc.descriptorSetDescs = setDescs;
    layoutDesc.descriptorSetDescsCount = 5;
    layoutDesc.allowInputLayout = true;
    auto& layout=storage.layout;
    if(!layout)layout=device->createPipelineLayout(layoutDesc);
    std::unique_ptr<RenderDescriptorSet> sets[5];
    for (int i = 0; i < 5; ++i) sets[i] = device->createDescriptorSet(setDescs[i]);
    const auto profileSets=std::chrono::steady_clock::now();
    auto sampler = device->createSampler(RenderSamplerDesc());
    sets[3]->setSampler(0, sampler.get());
    std::map<uint64_t, std::pair<uint32_t, std::unique_ptr<RenderSampler>>> samplers;
    // Anisotropic filtering override (F1 > Texture filtering): only for pixel
    // textures with a mip chain and linear minification. Look-up tables,
    // shadow maps and render target copies have one level and keep the
    // game's filter (anisotropy would average a LUT across the footprint).
    const uint32_t anisotropyOverride=frame.continuous||getenv("FD_NATIVE_ANISOTROPY")?ConfiguredAnisotropy():1;
    auto samplerFor = [&](const Draw& d, uint32_t slot) -> uint32_t {
        if (!d.hasSamplerStates || d.samplerStates[slot] == UINT32_MAX) return 0;
        const uint32_t state = d.samplerStates[slot];
        const uint32_t lod = d.textureFetches[slot][4], border = d.textureFetches[slot][5] & 3;
        const uint64_t key = uint64_t(state) << 32 | (d.hasFetchMetadata ? (lod & 0x003FFFFC) | (border << 22) : 0);
        auto found = samplers.find(key);
        if (found != samplers.end()) return found->second.first;
        if (samplers.size() + 1 >= kMaxSamplers) return 0;
        auto address = [](uint32_t mode) {
            switch (mode & 7)
            {
            case 0: return RenderTextureAddressMode::WRAP;
            case 1: return RenderTextureAddressMode::MIRROR;
            case 2: case 4: return RenderTextureAddressMode::CLAMP;
            case 6: return RenderTextureAddressMode::BORDER;
            default: return RenderTextureAddressMode::MIRROR_ONCE;
            }
        };
        RenderSamplerDesc desc;
        desc.addressU = address(state);
        desc.addressV = address(state >> 3);
        desc.addressW = address(state >> 6);
        desc.magFilter = ((state >> 9) & 3) == 0 ? RenderFilter::NEAREST : RenderFilter::LINEAR;
        desc.minFilter = ((state >> 11) & 3) == 0 ? RenderFilter::NEAREST : RenderFilter::LINEAR;
        desc.mipmapMode = ((state >> 13) & 3) == 1 ? RenderMipmapMode::LINEAR : RenderMipmapMode::NEAREST;
        const uint32_t aniso = (state >> 15) & 7;
        desc.anisotropyEnabled = aniso != 0;
        desc.maxAnisotropy = aniso ? 1u << (std::min)(aniso - 1, 4u) : 1u;
        if (d.hasFetchMetadata)
        {
            desc.minLOD = float((lod >> 2) & 15);
            desc.maxLOD = (std::max)(desc.minLOD, float((lod >> 6) & 15));
            desc.mipLODBias = float(int32_t(lod << 10) >> 22) / 32.0f;
            desc.borderColor = border == 1 ? RenderBorderColor::OPAQUE_WHITE :
                border == 0 ? RenderBorderColor::TRANSPARENT_BLACK : RenderBorderColor::OPAQUE_BLACK;
            if (anisotropyOverride > desc.maxAnisotropy && slot < 16 && desc.maxLOD > desc.minLOD &&
                desc.minFilter == RenderFilter::LINEAR)
            {
                desc.anisotropyEnabled = true;
                desc.maxAnisotropy = anisotropyOverride;
            }
        }
        const uint32_t index = uint32_t(samplers.size()) + 1;
        auto object = device->createSampler(desc);
        sets[3]->setSampler(index, object.get());
        samplers.emplace(key, std::make_pair(index, std::move(object)));
        return index;
    };
    const auto psCode = frame.debugPS;
    auto pixelShader = psCode.empty()?nullptr:device->createShader(psCode.data(), psCode.size(), "main", RenderShaderFormat::DXIL);

    // FD_REPLAY_TEST=1: try pipelines with the test shaders in <dir>/tests.
    if (getenv("FD_REPLAY_TEST"))
    {
        for (const char* name : {"a", "b", "c"})
        {
            const std::vector<uint8_t> code = ReadFile(dir + "/tests/" + name + ".vs.dxil");
            auto vs = device->createShader(code.data(), code.size(), "main", RenderShaderFormat::DXIL);
            RenderInputElement element("POSITION", 0, 0, RenderFormat::R32G32B32A32_FLOAT, 0, 0);
            RenderInputSlot slot(0, 16);
            RenderGraphicsPipelineDesc desc;
            desc.inputSlots = &slot;
            desc.inputSlotsCount = 1;
            desc.inputElements = &element;
            desc.inputElementsCount = 1;
            desc.pipelineLayout = layout.get();
            desc.vertexShader = vs.get();
            desc.pixelShader = pixelShader.get();
            desc.renderTargetFormat[0] = RenderFormat::R8G8B8A8_UNORM;
            desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
            desc.renderTargetCount = 1;
            desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
            auto pipeline = device->createGraphicsPipeline(desc);
            const bool ok = pipeline && static_cast<D3D12GraphicsPipeline*>(pipeline.get())->d3d;
            fprintf(stderr, "[test] %s: %s\n", name, ok ? "accepted" : "REJECTED");
        }
        return 0;
    }

    // All per-draw data in a few upload buffers.
    size_t vertexTotal = 16, indexTotal = 16;
    // Mixing 16- and 32-bit indices requires padding between draws: an odd
    // count of 16-bit indices must not leave the next R32_UINT view at +2.
    for (const Draw& d : draws)
    {
        vertexTotal += d.vertexBytes;
        indexTotal += (size_t(d.indexBytes) + 3) & ~size_t(3);
    }
    auto& vertexBuffer=storage.vertexBuffer;auto& indexBuffer=storage.indexBuffer;
    if(vertexTotal>storage.vertexCapacity){vertexBuffer=device->createBuffer(RenderBufferDesc::VertexBuffer(vertexTotal,RenderHeapType::UPLOAD));storage.vertexCapacity=vertexTotal;}
    if(indexTotal>storage.indexCapacity){indexBuffer=device->createBuffer(RenderBufferDesc::IndexBuffer(indexTotal,RenderHeapType::UPLOAD));storage.indexCapacity=indexTotal;}
    // Per draw: 4096 bytes vertex constants, 4096 pixel constants, kSharedSize shared.
    constexpr uint32_t kPerDraw = 4096 + 4096 + kSharedSize;
    auto& constantBuffer=storage.constantBuffer;
    const uint64_t constantBytes=kPerDraw*(draws.size()+1);
    if(constantBytes>storage.constantCapacity){constantBuffer=device->createBuffer(RenderBufferDesc::UploadBuffer(constantBytes,RenderBufferFlag::CONSTANT));storage.constantCapacity=constantBytes;}
    // Guest vertex streams read by pulled vertex fetches.
    const auto streamFile = frame.streams;
    auto& streamBuffer=storage.streamBuffer;
    const uint64_t streamBytes=(std::max)((std::max)(streamFile.size(),size_t(frame.streamSegmentBytes)),size_t(16));
    if(streamBytes>storage.streamCapacity){streamBuffer=device->createBuffer(RenderBufferDesc::UploadBuffer(streamBytes));storage.streamCapacity=streamBytes;}
    if (!streamFile.empty())
    {
        memcpy(streamBuffer->map(), streamFile.data(), streamFile.size());
        streamBuffer->unmap();
    }
    // Live frames: one copy from the shared converted streams (~20 MB in
    // play), on another thread while this one records the commands; joined
    // before submission. The future's destructor also waits on early returns.
    std::future<void> streamCopy;
    if (!frame.streamSegments.empty())
    {
        auto* map = static_cast<uint8_t*>(streamBuffer->map());
        streamCopy = std::async(std::launch::async, [map, segments = frame.streamSegments] {
            for (const auto& segment : segments) memcpy(map + segment.offset, segment.data, segment.size);
        });
    }
    const auto profileUploads=std::chrono::steady_clock::now();
    auto* vertexMap = static_cast<uint8_t*>(vertexBuffer->map());
    auto* indexMap = static_cast<uint8_t*>(indexBuffer->map());
    auto* constantMap = static_cast<uint8_t*>(constantBuffer->map());
    memset(constantMap, 0, kPerDraw); // pixel constants: zero for now

    // Textures: RGBA8, uploaded through one staging buffer.
    const bool versioned=frame.continuous&&versions.size()==textureCount;
    if(versioned)
    {
        std::vector<uint64_t> live(versions);std::sort(live.begin(),live.end());
        std::erase_if(storage.staticTextures,[&](auto& entry){
            if(std::binary_search(live.begin(),live.end(),entry.first))return false;
            auto& texture=entry.second.texture;
            if(texture&&texture.use_count()==1&&storage.freeTextures.size()<64)
            {
                storage.freeTextures.emplace(entry.second.shape,std::move(texture));
            }
            return true;
        });
    }
    std::vector<bool> reused(textureCount,false);
    std::vector<std::shared_ptr<RenderTexture>> textures;
    std::vector<RenderFormat> textureFormats; // per table texture
    std::unique_ptr<RenderBuffer> staging;
    {
        size_t at = 4, stagingSize = 0;
        struct TextureUpload { uint32_t index, mip, width, height; size_t from, to;const uint8_t* pixels; };
        std::vector<TextureUpload> uploads;
        std::vector<std::pair<uint32_t, uint32_t>> dimensions;
        std::vector<uint32_t> mipCounts(textureCount, 1);
        auto addUpload = [&](uint32_t index, uint32_t mip, uint32_t width, uint32_t height, size_t from,const uint8_t* pixels=nullptr) {
            uploads.push_back({index, mip, width, height, from, stagingSize,pixels});
            stagingSize += size_t((width * FormatBytes(textureFormats[index]) + 255) & ~255u) * height;
            stagingSize = (stagingSize + 511) & ~size_t(511);
        };
        for (uint32_t i = 0; i < textureCount; ++i)
        {
            if (!liveResources&&at + 8 > textureFile.size()) return 1;
            uint32_t w, h;
            if(liveResources){const auto& image=frame.resources[i]->image;w=image.width|(image.floatPixels?0x80000000u:0);h=image.height;}
            else {memcpy(&w, textureFile.data() + at, 4);memcpy(&h, textureFile.data() + at + 4, 4);}
            textureFormats.push_back((w >> 31) ? RenderFormat::R32G32B32A32_FLOAT : RenderFormat::R8G8B8A8_UNORM);
            w &= 0x7FFFFFFF;
            const size_t bytes = size_t(w) * h * FormatBytes(textureFormats.back());
            if (!w || !h || (liveResources?frame.resources[i]->image.pixels.size()!=bytes:at + 8 + bytes > textureFile.size())) return 1;
            dimensions.emplace_back(w, h);
            // FD_DUMP_TEXTURE=<table index>: debug, write that texture's base level.
            if(const char* dumpIndex=CachedEnv("FD_DUMP_TEXTURE");dumpIndex&&uint32_t(atoi(dumpIndex))==i&&textureFormats.back()==RenderFormat::R8G8B8A8_UNORM)
            {
                const uint8_t* pixels=liveResources?frame.resources[i]->image.pixels.data():textureFile.data()+at+8;
                stbi_write_png(("texture_"+std::to_string(i)+".png").c_str(),int(w),int(h),4,pixels,int(w*4));
            }
            if(versioned)
            {
                const auto found=storage.staticTextures.find(versions[i]);
                reused[i]=found!=storage.staticTextures.end()&&bool(found->second.texture);
            }
            if(!reused[i])addUpload(i, 0, w, h, at + 8,liveResources?frame.resources[i]->image.pixels.data():nullptr);
            if(liveResources)
            {
                mipCounts[i]=uint32_t(frame.resources[i]->mips.size())+1;
                if(!reused[i])for(uint32_t mip=1;mip<mipCounts[i];++mip){const auto& image=frame.resources[i]->mips[mip-1];addUpload(i,mip,image.width,image.height,0,image.pixels.data());}
            }
            at += 8 + bytes;
        }
        const auto mipFile = frame.mips;
        size_t mipAt = 4;
        uint32_t mipTextureCount = 0;
        if (mipFile.size() >= 4) memcpy(&mipTextureCount, mipFile.data(), 4);
        for (uint32_t entry = 0; entry < mipTextureCount; ++entry)
        {
            if (mipAt + 8 > mipFile.size()) return 1;
            uint32_t index, levels;
            memcpy(&index, mipFile.data() + mipAt, 4);
            memcpy(&levels, mipFile.data() + mipAt + 4, 4);
            mipAt += 8;
            if (index >= textureCount || levels > 15 || textureFormats[index] != RenderFormat::R8G8B8A8_UNORM) return 1;
            mipCounts[index] = levels + 1;
            for (uint32_t mip = 1; mip <= levels; ++mip)
            {
                if (mipAt + 8 > mipFile.size()) return 1;
                uint32_t w, h;
                memcpy(&w, mipFile.data() + mipAt, 4);
                memcpy(&h, mipFile.data() + mipAt + 4, 4);
                mipAt += 8;
                const size_t bytes = size_t(w) * h * 4;
                if (w != (std::max)(1u, dimensions[index].first >> mip) ||
                    h != (std::max)(1u, dimensions[index].second >> mip) || mipAt + bytes > mipFile.size()) return 1;
                if(!reused[index])
                {
                    const size_t from = textureFile.size();
                    textureFile.insert(textureFile.end(), mipFile.begin() + mipAt, mipFile.begin() + mipAt + bytes);
                    addUpload(index, mip, w, h, from);
                }
                mipAt += bytes;
            }
        }
        RenderBuffer* stagingBuffer=nullptr;
        if(versioned)
        {
            if(stagingSize>storage.textureStagingCapacity)
            {
                storage.textureStagingCapacity=(std::max)(stagingSize+stagingSize/4,size_t(1)<<20);
                storage.textureStaging=device->createBuffer(RenderBufferDesc::UploadBuffer(storage.textureStagingCapacity));
            }
            stagingBuffer=storage.textureStaging.get();
        }
        else{staging = device->createBuffer(RenderBufferDesc::UploadBuffer((std::max)(stagingSize, size_t(16))));stagingBuffer=staging.get();}
        auto* map = uploads.empty()?nullptr:static_cast<uint8_t*>(stagingBuffer->map());
        for (const auto& u : uploads)
        {
            const uint32_t texel = FormatBytes(textureFormats[u.index]), pitch = (u.width * texel + 255) & ~255u;
            for (uint32_t y = 0; y < u.height; ++y)
                memcpy(map + u.to + size_t(y) * pitch, (u.pixels?u.pixels:textureFile.data()+u.from) + size_t(y) * u.width * texel, u.width * texel);
        }
        if(map)stagingBuffer->unmap();
        // FD_UPLOAD_WAIT=1 restores the CPU wait for the texture copies.
        static const bool uploadWait = CachedEnv("FD_UPLOAD_WAIT") != nullptr;
        auto& upload = uploadWait ? list : uploadList;
        if(!uploads.empty())upload->begin();
        for (uint32_t i = 0; i < textureCount; ++i)
        {
            if(reused[i])textures.push_back(storage.staticTextures.at(versions[i]).texture);
            else
            {
                std::shared_ptr<RenderTexture> texture;
                if(versioned)
                    if(auto free=storage.freeTextures.find(std::tuple{dimensions[i].first,dimensions[i].second,mipCounts[i],textureFormats[i]});free!=storage.freeTextures.end())
                    {texture=std::move(free->second);storage.freeTextures.erase(free);}
                if(!texture)texture=CreateTexture(device.get(), RenderTextureDesc::Texture2D(dimensions[i].first, dimensions[i].second,mipCounts[i], textureFormats[i]));
                textures.push_back(std::move(texture));
                upload->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(textures.back().get(), RenderTextureLayout::COPY_DEST));
                if(versioned){auto& cached=storage.staticTextures[versions[i]];cached.texture=textures.back();cached.shape={dimensions[i].first,dimensions[i].second,mipCounts[i],textureFormats[i]};}
            }
        }
        for (const auto& u : uploads)
        {
            const auto format = textureFormats[u.index];
            const uint32_t texel = FormatBytes(format);
            upload->copyTextureRegion(RenderTextureCopyLocation::Subresource(textures[u.index].get(), u.mip),
                RenderTextureCopyLocation::PlacedFootprint(stagingBuffer, format, u.width, u.height, 1,
                                                          ((u.width * texel + 255) & ~255u) / texel, u.to));
        }
        for (uint32_t i = 0; i < textureCount; ++i)
        {
            if(!uploads.empty())upload->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(textures[i].get(), RenderTextureLayout::SHADER_READ));
            sets[0]->setTexture(i, textures[i].get(), RenderTextureLayout::SHADER_READ);
        }
        if(!frame.continuous||CachedEnv("FD_REPLAY_TRACE"))printf("fd_replay: %u captured mip chains uploaded\n", mipTextureCount);
        // Nothing new (every texture reused): no upload submission to wait for.
        if(!uploads.empty())
        {
            upload->end();
            const RenderCommandList* uploadCommands = upload.get();
            queue->executeCommandLists(&uploadCommands, 1, nullptr, 0, nullptr, 0, uploadWait ? fence.get() : nullptr);
            if (uploadWait) queue->waitForCommandFence(fence.get());
        }
    }
    const auto profileTextures=std::chrono::steady_clock::now();

    // Cube maps use six array slices and genuine cube SRVs in space2. Ambient
    // lighting reads these independently of the Texture2D descriptor heap.
    std::vector<std::shared_ptr<RenderTexture>> cubeTextures;
    std::vector<std::shared_ptr<RenderTextureView>> cubeViews;
    std::vector<RenderTexture*> cubesByIndex(textureCount, nullptr);
    std::unique_ptr<RenderBuffer> cubeStaging;
    {
        std::vector<uint8_t> file(frame.cubes.begin(), frame.cubes.end());
        if(liveResources)
        {
            file.resize(4,0);uint32_t count=0;
            for(uint32_t i=0;i<textureCount;++i)
            {
                const auto& resource=*frame.resources[i];if(resource.cube.empty())continue;
                auto& cached=storage.staticTextures[versions[i]];
                if(cached.cube){cubesByIndex[i]=cached.cube.get();continue;}
                for(uint32_t value:{i,resource.image.width,resource.image.height}){const auto* first=reinterpret_cast<const uint8_t*>(&value);file.insert(file.end(),first,first+4);}
                file.insert(file.end(),resource.cube.begin(),resource.cube.end());++count;
            }
            memcpy(file.data(),&count,4);
        }
        struct CubeUpload { uint32_t index, width, height; size_t from, to, faceStride; };
        std::vector<CubeUpload> uploads;
        size_t at = 4, total = 0;
        uint32_t count = 0;
        if (file.size() >= 4) memcpy(&count, file.data(), 4);
        for (uint32_t i = 0; i < count; ++i)
        {
            if (at + 12 > file.size()) return 1;
            uint32_t info[3];
            memcpy(info, file.data() + at, 12);
            at += 12;
            const size_t bytes = size_t(info[1]) * info[2] * 4 * 6;
            if (info[0] >= textureCount || !info[1] || !info[2] || at + bytes > file.size()) return 1;
            const size_t faceStride = (size_t((info[1] * 4 + 255) & ~255u) * info[2] + 511) & ~size_t(511);
            if(versioned&&storage.staticTextures[versions[info[0]]].cube)
                cubesByIndex[info[0]]=storage.staticTextures[versions[info[0]]].cube.get();
            else {uploads.push_back({info[0], info[1], info[2], at, total, faceStride});total += faceStride * 6;}
            at += bytes;
        }
        // Valid black fallback for cube heap entries that no shader should use.
        const size_t black = file.size();
        for (uint32_t face = 0; face < 6; ++face) file.insert(file.end(), {0, 0, 0, 255});
        if(!storage.blackCube){uploads.insert(uploads.begin(), {UINT32_MAX, 1, 1, black, total, 512});total += 512 * 6;}
        else for(uint32_t i=0;i<(std::max)(textureCount,1u);++i)sets[2]->setTexture(i,storage.blackCube.get(),RenderTextureLayout::SHADER_READ,storage.blackCubeView.get());
        if(!uploads.empty())cubeStaging = device->createBuffer(RenderBufferDesc::UploadBuffer((std::max)(total,size_t(16))));
        auto* map = uploads.empty()?nullptr:static_cast<uint8_t*>(cubeStaging->map());
        for (const auto& u : uploads)
            for (uint32_t face = 0; face < 6; ++face)
                for (uint32_t y = 0; y < u.height; ++y)
                    memcpy(map + u.to + face * u.faceStride + size_t(y) * ((u.width * 4 + 255) & ~255u),
                        file.data() + u.from + (size_t(face) * u.height + y) * u.width * 4, u.width * 4);
        if(map)cubeStaging->unmap();
        if(!uploads.empty())list->begin();
        for (const auto& u : uploads)
        {
            std::shared_ptr<RenderTexture> texture = CreateTexture(device.get(), RenderTextureDesc::Texture(RenderTextureDimension::TEXTURE_2D,
                u.width, u.height, 1, 1, 6, RenderFormat::R8G8B8A8_UNORM));
            std::shared_ptr<RenderTextureView> view = texture->createTextureView(RenderTextureViewDesc::TextureCube(RenderFormat::R8G8B8A8_UNORM));
            list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(texture.get(), RenderTextureLayout::COPY_DEST));
            for (uint32_t face = 0; face < 6; ++face)
                list->copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.get(), 0, face),
                    RenderTextureCopyLocation::PlacedFootprint(cubeStaging.get(), RenderFormat::R8G8B8A8_UNORM,
                        u.width, u.height, 1, ((u.width * 4 + 255) & ~255u) / 4, u.to + face * u.faceStride));
            list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(texture.get(), RenderTextureLayout::SHADER_READ));
            if (u.index == UINT32_MAX)
            {
                storage.blackCube=texture;storage.blackCubeView=view;
                for (uint32_t i = 0; i < (std::max)(textureCount, 1u); ++i)
                    sets[2]->setTexture(i, texture.get(), RenderTextureLayout::SHADER_READ, view.get());
            }
            else
            {
                cubesByIndex[u.index] = texture.get();
                sets[2]->setTexture(u.index, texture.get(), RenderTextureLayout::SHADER_READ, view.get());
                if(versioned){auto& cached=storage.staticTextures[versions[u.index]];cached.cube=texture;cached.cubeView=view;}
            }
            cubeTextures.push_back(std::move(texture));
            cubeViews.push_back(std::move(view));
        }
        if(!uploads.empty())
        {
            list->end();
            const RenderCommandList* upload = list.get();
            queue->executeCommandLists(&upload, 1, nullptr, 0, nullptr, 0, fence.get());
            queue->waitForCommandFence(fence.get());
        }
        if(versioned)for(uint32_t i=0;i<textureCount;++i)
            if(cubesByIndex[i]){auto& cached=storage.staticTextures[versions[i]];sets[2]->setTexture(i,cached.cube.get(),RenderTextureLayout::SHADER_READ,cached.cubeView.get());}
        if(!frame.continuous||CachedEnv("FD_REPLAY_TRACE"))printf("fd_replay: %u cube maps uploaded\n", count);
    }
    const auto profileCubes=std::chrono::steady_clock::now();
    uint32_t nextCubeView = descriptorBase;
    std::map<uint64_t, uint32_t> cubeSwizzles;

    // Genuine volume images for tfetch3D, independent of the 2D slice-zero
    // table retained by old replay readers. Slice rows obey D3D12 footprints.
    std::vector<RenderTexture*> volumesByIndex(textureCount,nullptr);
    std::vector<std::shared_ptr<RenderTexture>> volumeTextures;
    std::vector<std::unique_ptr<RenderTextureView>> volumeViews;
    std::unique_ptr<RenderBuffer> volumeStaging;
    {
        struct VolumeUpload{uint32_t index,width,height,depth;RenderFormat format;const uint8_t* pixels;size_t offset;};
        std::vector<VolumeUpload> uploads;size_t total=0;
        auto addVolume=[&](uint32_t index,uint32_t w,uint32_t h,uint32_t depth,bool floating,std::span<const uint8_t> pixels)
        {
            const uint32_t texel=floating?16:4;
            if(index>=textureCount||!w||!h||!depth||w>2048||h>2048||depth>1024||pixels.size()!=uint64_t(w)*h*depth*texel)
                throw std::runtime_error("Invalid native volume image");
            if(volumesByIndex[index])throw std::runtime_error("Duplicate native volume index");
            std::shared_ptr<RenderTexture> texture;
            if(versioned)texture=storage.staticTextures[versions[index]].volume;
            if(!texture)
            {
                const auto format=floating?RenderFormat::R32G32B32A32_FLOAT:RenderFormat::R8G8B8A8_UNORM;
                texture=CreateTexture(device.get(),RenderTextureDesc::Texture3D(w,h,depth,1,format));
                total=(total+511)&~size_t(511);uploads.push_back({index,w,h,depth,format,pixels.data(),total});
                total+=size_t((w*texel+255)&~255u)*h*depth;
                if(versioned)storage.staticTextures[versions[index]].volume=texture;
            }
            volumesByIndex[index]=texture.get();volumeTextures.push_back(std::move(texture));
            sets[1]->setTexture(index,volumesByIndex[index],RenderTextureLayout::SHADER_READ);
        };
        if(liveResources)
            for(uint32_t i=0;i<textureCount;++i){const auto& resource=*frame.resources[i];if(resource.volumeDepth)addVolume(i,resource.image.width,resource.image.height,resource.volumeDepth,resource.image.floatPixels,resource.volume);}
        else if(!frame.volumes.empty())
        {
            size_t at=0;auto word=[&](){if(at+4>frame.volumes.size())throw std::runtime_error("Truncated native volume table");uint32_t value;memcpy(&value,frame.volumes.data()+at,4);at+=4;return value;};
            const uint32_t count=word();if(count>textureCount)throw std::runtime_error("Invalid native volume count");
            for(uint32_t i=0;i<count;++i)
            {
                const uint32_t index=word(),width=word(),height=word(),depth=word();const bool floating=width>>31;
                const uint64_t bytes=uint64_t(width&0x7FFFFFFF)*height*depth*(floating?16:4);
                if(bytes>frame.volumes.size()-at)throw std::runtime_error("Truncated native volume pixels");
                addVolume(index,width&0x7FFFFFFF,height,depth,floating,frame.volumes.subspan(at,size_t(bytes)));at+=size_t(bytes);
            }
            if(at!=frame.volumes.size())throw std::runtime_error("Unexpected native volume data");
        }
        if(!uploads.empty())
        {
            volumeStaging=device->createBuffer(RenderBufferDesc::UploadBuffer(total));auto* map=static_cast<uint8_t*>(volumeStaging->map());
            list->begin();
            for(const auto& u:uploads)
            {
                const uint32_t texel=FormatBytes(u.format),row=u.width*texel,pitch=(row+255)&~255u;
                for(uint32_t z=0;z<u.depth;++z)for(uint32_t y=0;y<u.height;++y)
                    memcpy(map+u.offset+(size_t(z)*u.height+y)*pitch,u.pixels+(size_t(z)*u.height+y)*row,row);
                list->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(volumesByIndex[u.index],RenderTextureLayout::COPY_DEST));
                list->copyTextureRegion(RenderTextureCopyLocation::Subresource(volumesByIndex[u.index],0),RenderTextureCopyLocation::PlacedFootprint(volumeStaging.get(),u.format,u.width,u.height,u.depth,pitch/texel,u.offset));
                list->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(volumesByIndex[u.index],RenderTextureLayout::SHADER_READ));
            }
            volumeStaging->unmap();list->end();const RenderCommandList* upload=list.get();
            queue->executeCommandLists(&upload,1,nullptr,0,nullptr,0,fence.get());queue->waitForCommandFence(fence.get());
        }
    }
    uint32_t nextVolumeView=descriptorBase;
    std::map<uint64_t,uint32_t> volumeSwizzles;

    // Targets keyed by (surface info, color info, depth info), sized to the
    // largest viewport any draw of the frame uses with them.
    const auto profileResources=std::chrono::steady_clock::now();
    auto& depthSurfaces=storage.depthSurfaces;
    uint32_t depthSerial = 0;
    auto& targets=storage.targets;
    auto& resolved=storage.resolved;auto& resolvedDepth=storage.resolvedDepth;
    auto& resolvedSwap=storage.resolvedSwap;auto& resolvedBias=storage.resolvedBias;
    auto& snapshots=storage.snapshots;auto& depthSnapshots=storage.depthSnapshots;
    uint32_t nextView=descriptorBase;
    std::set<Surface*> boundSurfaces;
    auto bindSurface=[&](Surface& surface) {
        if(!surface.texture||!boundSurfaces.insert(&surface).second)return;
        surface.views.clear();
        if(nextView>=descriptorBase+kMaxTargets)throw std::runtime_error("Native surface descriptor capacity exceeded");
        surface.descriptor=nextView++;
        if(surface.format==RenderFormat::D32_FLOAT_S8_UINT&&!surface.plainView)
            surface.plainView=surface.texture->createTextureView(RenderTextureViewDesc::Texture2D(surface.format));
        sets[surface.samples>1?4:0]->setTexture(surface.descriptor,surface.texture.get(),RenderTextureLayout::SHADER_READ,surface.plainView.get());
    };
    for(auto& [key,target]:targets){target.draws=0;for(uint32_t k=0;k<target.count;++k){target.rt[k]->explicitlyCleared=false;bindSurface(*target.rt[k]);}}
    for(auto& [key,surface]:depthSurfaces)bindSurface(surface);
    for(auto& [base,snapshot]:snapshots)bindSurface(snapshot->surface);
    for(auto& [base,snapshot]:depthSnapshots)bindSurface(snapshot->surface);
    for(auto& [base,packed]:storage.packed)bindSurface(packed->surface);
    if(upscale.upscaled.texture)bindSurface(upscale.upscaled);
    std::map<uint32_t, const Draw*> pendingResolves; // destination -> its first resolve in the frame
    using ClearKey = std::pair<uint32_t, uint32_t>; // surface info, colour/depth info
    std::map<ClearKey, const Draw*> colorClears, depthClears;
    auto clearColorValue = [](const Draw& d, RenderFormat format) {
        const float divisor = FixedSnorm(format) ? 255.0f * 32.0f : 255.0f;
        return RenderColor(float((d.clearColor >> 16) & 255) / divisor,
            float((d.clearColor >> 8) & 255) / divisor, float(d.clearColor & 255) / divisor,
            float(d.clearColor >> 24) / divisor);
    };
    std::map<SurfaceKey, std::pair<uint32_t, uint32_t>> targetSizes;
    std::map<SurfaceKey, uint32_t> targetCounts;
    auto keyOf = [](const Draw& d) {
        return SurfaceKey(d.regs[RB_SURFACE_INFO], d.regs[RB_COLOR_INFO], d.regs[RB_COLOR1_INFO], d.regs[RB_COLOR2_INFO],
            d.regs[RB_COLOR3_INFO], d.regs[RB_DEPTH_INFO]);
    };
    // RT k is written when the pixel shader outputs SV_Target<k> and the color
    // mask has bits for it (Xenia's rule); count = highest written + 1.
    std::map<std::string, uint32_t> psTargets;
    auto writtenTargets = [&](const Draw& d) -> uint32_t {
        if (d.ps.empty()) return (d.regs[RB_COLOR_MASK] & 0xF) ? 1u : 0u;
        auto found = psTargets.find(d.ps);
        if (found == psTargets.end())
        {
            const std::vector<uint8_t> text = ReadDrawShader(dir, d.ps, ShaderStage::Pixel, true);
            const std::string hlsl(text.begin(), text.end());
            uint32_t mask = 0;
            for (uint32_t k = 0; k < 4; ++k)
                if (hlsl.find("SV_Target" + std::to_string(k)) != std::string::npos) mask |= 1u << k;
            found = psTargets.emplace(d.ps, mask).first;
        }
        uint32_t mask = 0;
        for (uint32_t k = 0; k < 4; ++k)
            if ((found->second >> k & 1) && ((d.regs[RB_COLOR_MASK] >> (4 * k)) & 0xF)) mask |= 1u << k;
        return mask;
    };
    for (const Draw& d : draws)
    {
        if (d.clear) continue;
        if (d.resolve)
        {
            // First resolve of each destination: reads before it in the frame see
            // the previous frame's copy, approximated by the live source target.
            pendingResolves.emplace(d.copyDest, &d);
            continue;
        }
        const uint32_t written = writtenTargets(d);
        uint32_t& count = targetCounts[keyOf(d)];
        for (uint32_t k = 0; k < 4; ++k)
            if (written >> k & 1) count = (std::max)(count, k + 1);
        auto& size = targetSizes[keyOf(d)];
        size.first = (std::max)(size.first, uint32_t(std::lround(2 * std::fabs(AsFloat(d.regs[VPORT_XSCALE])))));
        size.second = (std::max)(size.second, uint32_t(std::lround(2 * std::fabs(AsFloat(d.regs[VPORT_YSCALE])))));
    }
    auto targetExtent = [&](const Draw& d) {
        const uint32_t br=d.regs[PA_SC_WINDOW_SCISSOR_BR];
        const auto& size=targetSizes[keyOf(d)];
        return std::pair{(std::clamp)((std::max)(br&0x7FFF,size.first),1u,4096u),
            (std::clamp)((std::max)(br>>16&0x7FFF,size.second),1u,4096u)};
    };
    // DLAA / DLSS: find the completed scene. Its copy is the last full-size
    // colour resolve followed by a smaller (post-processing) pass; the scene
    // draws before it at that size receive the sub-pixel jitter. The camera's
    // world-to-clip matrix comes from the terrain shaders (VS_DustConst[8..11]).
    const UpscalerMode upscaleMode=frame.continuous&&shaderCatalog?ConfiguredUpscalerMode(UpscalerSetting()):UpscalerMode::Off;
    // DLSS Frame Generation needs the same depth / motion vectors (no jitter).
    const bool frameGen=frame.continuous&&shaderCatalog&&fg::WantsInputs();
    // Internal resolution: screen-size targets (the front buffer's size) are
    // rendered at outputScale; with DLSS Super Resolution the scene before its
    // copy renders at renderScale and DLSS brings it to outputScale.
    // Replays stay at 1x unless FD_NATIVE_SCALE asks otherwise (diagnostics).
    static const bool forcedScale=getenv("FD_NATIVE_SCALE")!=nullptr;
    const double outputScale=(frame.continuous||forcedScale)&&shaderCatalog?ConfiguredRenderScale(RenderScaleSetting()):1.0;
    const bool superResolution=upscaleMode!=UpscalerMode::Off&&upscaleMode!=UpscalerMode::Dlaa&&
        !(upscale.ngx.Tried()&&!upscale.ngx.Available());
    uint32_t fbW=0,fbH=0;
    size_t sceneResolve=SIZE_MAX;
    std::vector<uint8_t> jitterDraw;
    float jitterX=0,jitterY=0,sceneVP[16]{},sceneViewport[4]{},sceneDepth[2]{};
    bool sceneCamera=false;
    if(upscaleMode!=UpscalerMode::Off||frameGen||outputScale!=1.0)
    {
        uint32_t& maxW=fbW;uint32_t& maxH=fbH;
        // Scene size = the copy to the front buffer (the game's output size).
        for(const Draw& d:draws)
            if(d.resolve&&(d.copyControl&7)<4&&(frame.frontBufferBase?d.copyDest==frame.frontBufferBase:!d.copyX&&!d.copyY&&!d.copyMip)){maxW=d.copyWidth;maxH=d.copyHeight;}
        for(size_t i=0;i<draws.size();++i)
        {
            const Draw& r=draws[i];
            if(!r.resolve||(r.copyControl&7)>=4||r.copyWidth!=maxW||r.copyHeight!=maxH||r.copyX||r.copyY||r.copyMip)continue;
            size_t j=i+1;
            while(j<draws.size()&&(draws[j].clear||draws[j].resolve))++j;
            if(j==draws.size())continue;
            // The next pass's own viewport (targets can share EDRAM keys across sizes).
            const uint32_t w=uint32_t(std::lround(2*std::fabs(AsFloat(draws[j].regs[VPORT_XSCALE]))));
            const uint32_t h=uint32_t(std::lround(2*std::fabs(AsFloat(draws[j].regs[VPORT_YSCALE]))));
            if(w&&h&&w<maxW&&h<maxH)sceneResolve=i;
        }
        static uint64_t sceneLog=0;
        if(sceneLog++%600==0)fprintf(stderr,"native upscaler: %s, scene copy %s (%ux%u)\n",UpscalerModeName(upscaleMode),
            sceneResolve==SIZE_MAX?"not found":std::to_string(sceneResolve).c_str(),maxW,maxH);
        if(sceneResolve!=SIZE_MAX)
        {
            if(upscaleMode!=UpscalerMode::Off)UpscalerJitter(upscale.frameIndex,UpscalerRenderScale(upscaleMode),jitterX,jitterY);
            if(CachedEnv("FD_UPSCALER_NO_JITTER"))jitterX=jitterY=0;
            jitterDraw.assign(draws.size(),0);
            auto isCamera=[&](const std::string& vs){
                auto found=upscale.cameraShaders.find(vs);
                if(found!=upscale.cameraShaders.end())return found->second;
                const auto* shader=shaderCatalog->Find(vs,ShaderStage::Vertex);
                const std::string text=shader?std::string(shader->source.begin(),shader->source.end()):std::string();
                const bool camera=text.find("VS_DustConst(8)")!=std::string::npos&&text.find("VS_DustConst(11)")!=std::string::npos&&text.find("oPos")!=std::string::npos;
                return upscale.cameraShaders.emplace(vs,camera).first->second;
            };
            for(size_t i=0;i<sceneResolve;++i)
            {
                const Draw& d=draws[i];
                if(d.clear||d.resolve||d.indexCount<=6)continue;
                const uint32_t w=uint32_t(std::lround(2*std::fabs(AsFloat(d.regs[VPORT_XSCALE]))));
                const uint32_t h=uint32_t(std::lround(2*std::fabs(AsFloat(d.regs[VPORT_YSCALE]))));
                if(w!=maxW||h!=maxH)continue;
                jitterDraw[i]=1;
                if(!sceneCamera&&isCamera(d.vs))
                {
                    const float* c=reinterpret_cast<const float*>(d.constants)+28*4;
                    std::copy(c,c+16,sceneVP);
                    sceneViewport[0]=AsFloat(d.regs[VPORT_XSCALE]);sceneViewport[1]=AsFloat(d.regs[VPORT_XOFFSET]);
                    sceneViewport[2]=AsFloat(d.regs[VPORT_YSCALE]);sceneViewport[3]=AsFloat(d.regs[VPORT_YOFFSET]);
                    sceneDepth[0]=AsFloat(d.regs[VPORT_ZSCALE]);sceneDepth[1]=AsFloat(d.regs[VPORT_ZOFFSET]);
                    sceneCamera=sceneViewport[0]!=0&&sceneViewport[2]!=0&&sceneDepth[0]!=0;
                }
            }
        }
    }
    const double renderScale=superResolution&&sceneResolve!=SIZE_MAX?outputScale*UpscalerRenderScale(upscaleMode):outputScale;
    // Shadow maps above the game's resolution. The sun's shadows are variance
    // shadow maps in four 512x512 cascades: casters (vertex shaders with the
    // VS_Shadow_* constants, plus the terrain drawn into the same depth),
    // a pass turning the depth copy into moments and a blur (VS_VSM_Const /
    // samp_ShadowVSM), copied into a 512x2048 atlas the scene samples with
    // normalized coordinates (samp_ShadowZ). All of them render at
    // shadowScale, so the atlas copy is shadowScale times larger. The moments
    // and blur passes step by VS_VSM_Const (c21, one texel in UV): it is
    // divided by shadowScale, so they filter the same number of texels and
    // the edges get sharper (the game's UV steps would blur 4x4 texels).
    static const bool forcedShadowScale=getenv("FD_NATIVE_SHADOW_SCALE")!=nullptr;
    const uint32_t shadowScale=(frame.continuous||forcedShadowScale)&&shaderCatalog?ConfiguredShadowScale():1;
    std::vector<uint8_t> shadowDraw;
    if(shadowScale>1)
    {
        auto role=[&](const std::string& name,ShaderStage stage)->int{
            if(name.empty())return 0;
            auto found=upscale.shadowRoles.find(name+(stage==ShaderStage::Vertex?".vs":".ps"));
            if(found!=upscale.shadowRoles.end())return found->second;
            const auto* shader=shaderCatalog->Find(name,stage);
            const std::string text=shader?std::string(shader->source.begin(),shader->source.end()):std::string();
            const int value=stage==ShaderStage::Vertex&&text.find("VS_Shadow_")!=std::string::npos?1:
                text.find("VS_VSM_Const")!=std::string::npos||text.find("samp_ShadowVSM")!=std::string::npos?2:0;
            upscale.shadowRoles.emplace(name+(stage==ShaderStage::Vertex?".vs":".ps"),value);
            return value;
        };
        std::set<std::pair<uint32_t,uint32_t>> casterDepths;
        std::set<SurfaceKey> vsmPasses;
        for(const Draw& d:draws)
        {
            if(d.clear||d.resolve)continue;
            const int vs=role(d.vs,ShaderStage::Vertex),ps=role(d.ps,ShaderStage::Pixel);
            if(vs==1)casterDepths.insert({d.regs[RB_SURFACE_INFO],d.regs[RB_DEPTH_INFO]});
            if(vs==2||ps==2)vsmPasses.insert(keyOf(d));
        }
        if(!casterDepths.empty()||!vsmPasses.empty())
        {
            shadowDraw.assign(draws.size(),0);
            for(size_t i=0;i<draws.size();++i)
            {
                const Draw& d=draws[i];
                if(d.resolve)continue;
                // 1: rendered at shadowScale; 2: also a VSM filter pass (c21 scaled).
                shadowDraw[i]=vsmPasses.count(keyOf(d))||casterDepths.count({d.regs[RB_SURFACE_INFO],d.regs[RB_DEPTH_INFO]});
                if(shadowDraw[i]&&!d.clear&&role(d.vs,ShaderStage::Vertex)==2)shadowDraw[i]=2;
            }
        }
        if(CachedEnv("FD_TRACE_SHADOWS"))
        {
            static uint64_t shadowLog=0;
            if(!frame.continuous||++shadowLog%300==1)
            {
                fprintf(stderr,"native shadows: %ux, %zu caster depth(s), %zu VSM pass target(s), %zu of %zu draws scaled, %zu filter passes\n",shadowScale,
                    casterDepths.size(),vsmPasses.size(),draws.size()-size_t(std::count(shadowDraw.begin(),shadowDraw.end(),uint8_t(0))),draws.size(),
                    size_t(std::count(shadowDraw.begin(),shadowDraw.end(),uint8_t(2))));
                if(CachedEnv("FD_TRACE_SHADOWS")[0]=='2')
                {
                    std::set<std::string> seen;
                    for(size_t i=0;i<draws.size();++i)
                    {
                        const Draw& d=draws[i];
                        char line[256];
                        if(d.resolve)snprintf(line,sizeof(line),"  resolve src %u surface %08X color %08X depth %08X -> %08X (%u,%u %ux%u)",d.copyControl&7,d.copySurface,d.copyColor,d.copyDepth,d.copyDest,d.copyX,d.copyY,d.copyWidth,d.copyHeight);
                        else snprintf(line,sizeof(line),"  %s vs %s ps %s surface %08X color %08X depth %08X scaled %u",d.clear?"clear":"draw",d.vs.c_str(),d.ps.c_str(),
                            d.regs[RB_SURFACE_INFO],d.regs[RB_COLOR_INFO],d.regs[RB_DEPTH_INFO],shadowDraw.empty()?0u:shadowDraw[i]);
                        if(seen.insert(line).second)fprintf(stderr,"%5zu%s\n",i,line);
                        if(!shadowDraw.empty()&&shadowDraw[i]==2)
                        {
                            const float* c=reinterpret_cast<const float*>(d.constants)+21*4;
                            fprintf(stderr,"        c21 %g %g %g %g  c22 %g %g %g %g\n",c[0],c[1],c[2],c[3],c[4],c[5],c[6],c[7]);
                        }
                    }
                }
            }
        }
    }
    // HDR: the frame's last colour grading draw (the game's final composite:
    // scene, bloom and depth of field through the samp_ColoremapR / GB tables).
    // Its target is copied right after it, before the HUD is drawn over it.
    size_t gradingDraw=SIZE_MAX;
    if(frame.continuous&&HdrActive()&&shaderCatalog)
        for(size_t i=0;i<draws.size();++i)
        {
            const Draw& d=draws[i];
            if(d.clear||d.resolve||d.ps.empty())continue;
            auto found=upscale.gradingShaders.find(d.ps);
            if(found==upscale.gradingShaders.end())
            {
                const auto* shader=shaderCatalog->Find(d.ps,ShaderStage::Pixel);
                const std::string text=shader?std::string(shader->source.begin(),shader->source.end()):std::string();
                found=upscale.gradingShaders.emplace(d.ps,text.find("samp_ColoremapR")!=std::string::npos).first;
            }
            if(found->second)gradingDraw=i;
        }
    bool hudlessCopied=false;
    auto drawScale=[&](size_t index,uint32_t width,uint32_t height){
        if(!shadowDraw.empty()&&shadowDraw[index])return double(shadowScale);
        // Screen targets: the front buffer's size, or its width with a slightly
        // shorter letterbox (videos draw 1024x588 and copy 1024x600).
        if(!fbW||width!=fbW||height>fbH||height*10<fbH*9)return 1.0;
        return sceneResolve!=SIZE_MAX&&index>sceneResolve?outputScale:renderScale;
    };
    auto scaledSize=[](uint32_t value,double scale){return scale==1.0?value:(std::max)(1u,uint32_t(std::lround(value*scale)));};
    auto scaledRects=[](const Draw& c,double scale){
        std::vector<RenderRect> rects(c.clearRects.begin(),c.clearRects.end());
        if(scale!=1.0)for(auto& r:rects){r.left=int32_t(std::lround(r.left*scale));r.top=int32_t(std::lround(r.top*scale));
            r.right=int32_t(std::lround(r.right*scale));r.bottom=int32_t(std::lround(r.bottom*scale));}
        return rects;
    };
    // A changed internal resolution leaves targets of the old scale behind;
    // drop them (and what only they referenced) instead of growing forever.
    if(frame.continuous&&(renderScale!=upscale.lastRenderScale||outputScale!=upscale.lastOutputScale||shadowScale!=upscale.lastShadowScale))
    {
        for(auto it=targets.begin();it!=targets.end();)
        {
            const double s=it->second.scale;
            if(s!=1.0&&s!=renderScale&&s!=outputScale&&s!=double(shadowScale))it=targets.erase(it);else ++it;
        }
        for(auto it=storage.colorSurfaces.begin();it!=storage.colorSurfaces.end();)
            if(it->second.use_count()==1)it=storage.colorSurfaces.erase(it);else ++it;
        std::set<const Surface*> usedDepth;
        for(auto& [key,t]:targets)usedDepth.insert(t.ds);
        for(auto it=depthSurfaces.begin();it!=depthSurfaces.end();)
            if(!usedDepth.count(&it->second))it=depthSurfaces.erase(it);else ++it;
        resolved.clear();resolvedDepth.clear();
        upscale.lastRenderScale=renderScale;upscale.lastOutputScale=outputScale;
        if(shadowScale!=upscale.lastShadowScale)fprintf(stderr,"native renderer: shadow maps %ux\n",shadowScale);
        upscale.lastShadowScale=shadowScale;
        upscale.reset=true;
        if(outputScale!=1.0||renderScale!=1.0)fprintf(stderr,"native renderer: internal resolution %.3gx (scene %.3gx)\n",outputScale,renderScale);
    }
    auto targetFor = [&](const Draw& d) -> Target& {
        const auto surfaceKey=keyOf(d);
        const auto [width,height]=targetExtent(d);
        const double scale=drawScale(draws.IndexOf(d),width,height);
        const TargetKey key=std::tuple_cat(surfaceKey,std::tuple{width,height,(std::max)(targetCounts[surfaceKey],1u),uint32_t(std::lround(scale*1000))});
        Target& t = targets[key];
        if (t.framebuffer) return t;
        // Size: the larger of the window scissor and the largest viewport of
        // the target's draws (PA_CL_VPORT scale is half the size in pixels);
        // the scissor can be stale, e.g. the final 1024x600 composite after a
        // 512x300 pass. Screen-size targets are allocated at their scale.
        t.guestWidth=width;t.guestHeight=height;t.scale=scale;
        t.width=scaledSize(width,scale);t.height=scaledSize(height,scale);
        t.count = (std::max)(targetCounts[surfaceKey], 1u);
        const auto samples = multisampling(d.regs[RB_SURFACE_INFO]);
        auto coversTarget = [&](const Draw& clear) {
            return clear.clearRects.empty() || (clear.clearRects.size() == 1 &&
                clear.clearRects[0].left <= 0 && clear.clearRects[0].top <= 0 &&
                clear.clearRects[0].right >= int32_t(t.guestWidth) && clear.clearRects[0].bottom >= int32_t(t.guestHeight));
        };
        RenderColor initialColors[4];
        bool newColors[4]={};
        const RenderTexture* colors[4] = {};
        for (uint32_t k = 0; k < t.count; ++k)
        {
            if(frame.continuous)
            {
                auto& surface=storage.colorSurfaces[ColorKey(d.regs[RB_SURFACE_INFO],ColorInfo(d.regs,k),t.width,t.height)];
                if(!surface)surface=std::make_shared<Surface>();
                t.rt[k]=surface;
            }
            else t.rt[k]=std::make_shared<Surface>();
            Surface& rt = *t.rt[k];
            rt.format = colorFormat(ColorInfo(d.regs, k));
            initialColors[k] = RenderColor(0, 0, 0, CachedEnv("FD_CLEAR_ALPHA") ? float(atof(CachedEnv("FD_CLEAR_ALPHA"))) : 0);
            auto clear = colorClears.find({d.regs[RB_SURFACE_INFO], ColorInfo(d.regs, k)});
            if (clear != colorClears.end() && coversTarget(*clear->second)) initialColors[k] = clearColorValue(*clear->second, rt.format);
            newColors[k]=!rt.texture;
            if(newColors[k])
            {
            const auto optimizedClear = RenderClearValue::Color(initialColors[k], rt.format);
            rt.texture = CreateTexture(device.get(), RenderTextureDesc::ColorTarget(t.width, t.height, rt.format, RenderMultisampling(), &optimizedClear));
            if (samples.sampleCount > 1)
            {
                rt.multisample = CreateTexture(device.get(), RenderTextureDesc::ColorTarget(t.width, t.height, rt.format, samples, &optimizedClear));
                list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(rt.multisample.get(), RenderTextureLayout::COLOR_WRITE));
            }
            bindSurface(rt);
            rt.rowPitch = (t.width * FormatBytes(rt.format) + 255) & ~255u;
            if(output||!dir.empty())rt.readback = device->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(rt.rowPitch) * t.height));
            colors[k] = rt.multisample ? rt.multisample.get() : rt.texture.get();
            list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(rt.texture.get(), RenderTextureLayout::COLOR_WRITE));
            }
            colors[k] = rt.multisample ? rt.multisample.get() : rt.texture.get();
        }
        // Keep stencil as well as depth: water and refraction use stencil
        // masks to select the visible surface.
        // Depth storage belongs to its EDRAM address/pitch/size, independently
        // of the colour attachments bound alongside it (NativeXRenderer's
        // SurfaceKey follows the same storage identity).
        const DepthKey depthKey(d.regs[RB_SURFACE_INFO], d.regs[RB_DEPTH_INFO], t.width, t.height,
            CachedEnv("FD_SEPARATE_DEPTH") ? ++depthSerial : 0);
        t.ds = &depthSurfaces[depthKey];
        const bool newDepth = !t.ds->texture;
        if (newDepth)
        {
            t.ds->isDepth = true;
            t.ds->format = RenderFormat::D32_FLOAT_S8_UINT;
            const auto optimizedClear = RenderClearValue::Depth(RenderDepth(0), t.ds->format);
            t.ds->texture = CreateTexture(device.get(), RenderTextureDesc::DepthTarget(t.width, t.height, t.ds->format, samples, &optimizedClear));
            t.ds->samples=samples.sampleCount;
            bindSurface(*t.ds);
        }
        t.dsv = t.ds->texture->createTextureView(RenderTextureViewDesc::Texture2D(t.ds->format));
        if (newDepth) sets[samples.sampleCount > 1 ? 4 : 0]->setTexture(t.ds->descriptor, t.ds->texture.get(), RenderTextureLayout::SHADER_READ, t.dsv.get());
        RenderFramebufferDesc fb;
        fb.colorAttachments = colors;
        fb.colorAttachmentsCount = t.count;
        fb.depthAttachment = t.ds->texture.get();
        fb.depthAttachmentView = t.dsv.get();
        t.framebuffer = device->createFramebuffer(fb);
        list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(t.ds->texture.get(), RenderTextureLayout::DEPTH_WRITE));
        t.ds->shaderRead = false;
        list->setFramebuffer(t.framebuffer.get());
        for (uint32_t k = 0; k < t.count; ++k)
        {
            if(!newColors[k])continue;
            list->clearColor(k, initialColors[k]);
            auto clear = colorClears.find({d.regs[RB_SURFACE_INFO], ColorInfo(d.regs, k)});
            if (clear != colorClears.end())
            {
                const Draw& c = *clear->second;
                if (!coversTarget(c)){const auto rects=scaledRects(c,t.scale);list->clearColor(k, clearColorValue(c, t.rt[k]->format), rects.data(), uint32_t(rects.size()));}
                t.rt[k]->explicitlyCleared = true;
            }
        }
        // FD_PENDING_LIVE=1: reads before a destination's first copy this frame see
        // the live source target; by default they see the captured memory copy
        // (the previous frame's state, e.g. the simulation's height maps).
        if (CachedEnv("FD_PENDING_LIVE"))
        for (const auto& [dest, r] : pendingResolves)
            if (!resolved.count(dest) && std::get<0>(key) == r->copySurface && std::get<1>(key) == r->copyColor &&
                std::get<5>(key) == r->copyDepth && (r->copyControl & 7) < t.count)
            {
                resolved[dest] = t.rt[r->copyControl & 7].get();
                resolvedSwap[dest] = (r->copyDestInfo >> 24) & 1;
                resolvedBias[dest] = int32_t(r->copyDestInfo << 10) >> 26;
            }
        // An attachment change must preserve prior depth/stencil writes.
        if (newDepth)
        {
            // From Dust uses reversed depth (viewport Z scale -1, offset 1, GREATER_EQUAL).
            list->clearDepthStencil(true, true, 0.0f, 0);
            auto clear = depthClears.find({d.regs[RB_SURFACE_INFO], d.regs[RB_DEPTH_INFO]});
            if (clear != depthClears.end())
            {
                const Draw& c = *clear->second;
                const auto rects=scaledRects(c,t.scale);
                list->clearDepthStencil((c.clearFlags & 0x10) != 0, (c.clearFlags & 0x20) != 0,
                    c.clearDepth, c.clearStencil, rects.data(), uint32_t(rects.size()));
            }
        }
        t.hadContents=true;
        return t;
    };

    // Pipelines keyed by shader, vertex layout and fixed-function state.
    auto& pipelines=storage.pipelines;
    auto& pixelShaders=storage.pixelShaders;
    uint32_t failedPipelines = 0;
    auto pipelineFor = [&](const Draw& d) -> RenderPipeline* {
        const uint32_t depthControl = d.regs[RB_DEPTHCONTROL], mode = d.regs[PA_SU_SC_MODE_CNTL];
        const bool depthClip = d.hasContext && !(d.context[0x204] & 0x10000); // PA_CL_CLIP_CNTL.clip_disable
        std::string key = d.vs + "|" + d.ps + "|" + std::to_string(d.stride) + "|" + std::to_string(depthControl) + "|" + std::to_string(mode & 7) +
            "|" + std::to_string(d.regs[RB_BLENDCONTROL0]) + "|" + std::to_string(d.regs[RB_COLOR_MASK] & 0xF) +
            "|" + std::to_string(int(colorFormat(d.regs[RB_COLOR_INFO])));
        for (uint32_t k = 1; k < 4; ++k)
            key += "|" + std::to_string(ColorInfo(d.regs, k)) + "/" + std::to_string(BlendControl(d.regs, k));
        key += "|" + std::to_string(d.regs[RB_COLOR_MASK]) + "|" + std::to_string(writtenTargets(d));
        key += "|" + std::to_string(d.regs[RB_STENCILREFMASK]) + "|" + std::to_string(d.regs[RB_STENCILREFMASK_BF]);
        key += "|" + std::to_string(depthClip);
        key += "|" + std::to_string(multisampling(d.regs[RB_SURFACE_INFO]).sampleCount);
        for (const auto& s : d.semantics) key += "|" + s;
        auto it = pipelines.find(key);
        if (it != pipelines.end()) return it->second.second.get();
        auto& [vs, pipeline] = pipelines[key];
        RenderShader* ps = nullptr;
        if (!d.ps.empty())
        {
            auto found = pixelShaders.find(d.ps);
            if (found == pixelShaders.end())
            {
                const std::vector<uint8_t> psFile = ReadDrawShader(dir, d.ps, ShaderStage::Pixel);
                found = pixelShaders.emplace(d.ps, psFile.empty() ? nullptr
                    : device->createShader(psFile.data(), psFile.size(), "main", RenderShaderFormat::DXIL)).first;
                if (found->second) found->second->setName("PS " + d.ps);
            }
            ps = found->second.get();
        }
        const std::vector<uint8_t> code = ReadDrawShader(dir, d.vs, ShaderStage::Vertex);
        if (!code.empty())
        {
            vs = device->createShader(code.data(), code.size(), "main", RenderShaderFormat::DXIL);
            if (vs) vs->setName("VS " + d.vs);
        }
        if (!vs || (!d.ps.empty() && !ps))
        {
            fprintf(stderr, "fd_replay: missing or invalid shader for VS %s, PS %s\n", d.vs.c_str(), d.ps.c_str());
            ++failedPipelines;
            return nullptr;
        }
        std::vector<std::string> names;
        std::vector<RenderInputElement> elements;
        for (const auto& s : d.semantics) names.push_back(s.substr(0, s.find_first_of("0123456789")));
        for (size_t i = 0; i < d.semantics.size(); ++i)
            elements.emplace_back(names[i].c_str(), uint32_t(atoi(d.semantics[i].c_str() + names[i].size())), uint32_t(i),
                RenderFormat::R32G32B32A32_FLOAT, 0, uint32_t(i * 16));
        RenderInputSlot slot(0, d.stride);
        RenderGraphicsPipelineDesc desc;
        desc.inputSlots = &slot;
        desc.inputSlotsCount = 1;
        desc.inputElements = elements.data();
        desc.inputElementsCount = uint32_t(elements.size());
        desc.pipelineLayout = layout.get();
        desc.vertexShader = vs.get();
        // FD_REPLAY_DEBUG_PS=1 keeps the depth-colour debug shader.
        desc.pixelShader = CachedEnv("FD_REPLAY_DEBUG_PS") ? pixelShader.get() : ps;
        const uint32_t count = targetFor(d).count, written = writtenTargets(d);
        for (uint32_t k = 0; k < count; ++k)
        {
            desc.renderTargetFormat[k] = colorFormat(ColorInfo(d.regs, k));
            desc.renderTargetBlend[k] = Blend(BlendControl(d.regs, k), (written >> k & 1) ? d.regs[RB_COLOR_MASK] >> (4 * k) : 0);
        }
        desc.renderTargetCount = count;
        // RB_DEPTHCONTROL: z_enable bit 1, z_write bit 2, zfunc bits 4-6.
        desc.depthEnabled = (depthControl >> 1) & 1;
        desc.depthClipEnabled = depthClip;
        desc.multisampling = multisampling(d.regs[RB_SURFACE_INFO]);
        desc.depthWriteEnabled = (depthControl >> 2) & 1;
        desc.depthFunction = Compare(depthControl >> 4);
        desc.depthTargetFormat = RenderFormat::D32_FLOAT_S8_UINT;
        desc.stencilEnabled = d.hasStencilState && (depthControl & 1) && !CachedEnv("FD_NO_STENCIL");
        const uint32_t stencil = d.regs[RB_STENCILREFMASK];
        desc.stencilReference = stencil & 0xFF;
        desc.stencilReadMask = (stencil >> 8) & 0xFF;
        desc.stencilWriteMask = (stencil >> 16) & 0xFF;
        auto stencilFace = [&](uint32_t value) {
            RenderStencilFaceDesc face;
            face.compareFunction = Compare(value);
            face.failOp = RenderStencilOp(1 + ((value >> 3) & 7));
            face.passOp = RenderStencilOp(1 + ((value >> 6) & 7));
            face.depthFailOp = RenderStencilOp(1 + ((value >> 9) & 7));
            return face;
        };
        desc.stencilFrontFace = stencilFace(depthControl >> 8);
        desc.stencilBackFace = depthControl & 0x80 ? stencilFace(depthControl >> 20) : desc.stencilFrontFace;
        if (!(mode & 4)) std::swap(desc.stencilFrontFace, desc.stencilBackFace);
        // PA_SU_SC_MODE_CNTL: cull_front bit 0, cull_back bit 1, face bit 2 (1 =
        // clockwise front, the D3D12 default; 0 = counter-clockwise: swap sides).
        const bool cullFront = (mode & 4) ? (mode & 1) : (mode & 2), cullBack = (mode & 4) ? (mode & 2) : (mode & 1);
        desc.cullMode = cullBack ? RenderCullMode::BACK : cullFront ? RenderCullMode::FRONT : RenderCullMode::NONE;
        desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        if (vs) pipeline = device->createGraphicsPipeline(desc);
        if (pipeline) pipeline->setName("VS " + d.vs + ", PS " + d.ps);
        PrintDebugMessages(infoQueue, debugErrors, debugWarnings, d.vs.c_str());
        if (pipeline && !static_cast<D3D12GraphicsPipeline*>(pipeline.get())->d3d) { fprintf(stderr, "fd_replay: D3D12 rejected the pipeline for VS %s\n", d.vs.c_str()); pipeline.reset(); }
        if (!pipeline)
        {
            fprintf(stderr, "fd_replay: no pipeline for VS %s\n", d.vs.c_str());
            ++failedPipelines;
        }
        return pipeline.get();
    };

#ifdef FD_REPLAY_RENDERDOC
    // A live session captures only its explicitly requested diagnostic frame.
    // Starting a capture every frame would distort timing and fill the disk.
    auto* renderdoc = !frame.continuous||!dir.empty()?StartReplayCapture():nullptr;
#endif
    const auto profileSetup=std::chrono::steady_clock::now();
    list->begin();
    size_t vertexAt = 0, indexAt = 0;
    const Target* current = nullptr;
    // Every previous submission is fenced before reuse. Restore writable RTs
    // and readable resolve snapshots independently of any diagnostic readback.
    for(auto& [key,target]:targets)
        for(uint32_t k=0;k<target.count;++k)
        {
            auto& surface=*target.rt[k];
            list->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(surface.texture.get(),RenderTextureLayout::COLOR_WRITE));
            if(surface.multisample)list->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(surface.multisample.get(),RenderTextureLayout::COLOR_WRITE));
            surface.shaderRead=false;
        }
    for(auto& [key,surface]:depthSurfaces){list->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(surface.texture.get(),RenderTextureLayout::DEPTH_WRITE));surface.shaderRead=false;}
    auto readable=[&](Surface& surface){list->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(surface.texture.get(),RenderTextureLayout::SHADER_READ));surface.shaderRead=true;};
    for(auto& [base,snapshot]:snapshots)readable(snapshot->surface);
    for(auto& [base,snapshot]:depthSnapshots)readable(snapshot->surface);
    for(auto& [base,p]:storage.packed)readable(p->surface);

    // Depth resolved to k_8_8_8_8: a full-screen pass packs the depth surface
    // into the bytes the copy writes (tools/fd_replay/depth_pack.hlsl).
    const auto packVsCode = frame.depthVS.empty()?std::span<const uint8_t>(fd_native_depth_vs):frame.depthVS;
    const auto packPsCode = frame.depthPS.empty()?std::span<const uint8_t>(fd_native_depth_ps):frame.depthPS;
    const auto floatPsCode = frame.depthFloatPS.empty()?std::span<const uint8_t>(fd_native_depth_float):frame.depthFloatPS;
    auto& packVs=storage.packVs;auto& packPs=storage.packPs;auto& floatPs=storage.floatPs;
    auto& packPipeline=storage.packPipeline;auto& floatPipeline=storage.floatPipeline;
    if(!packPipeline)
    {
        packVs=device->createShader(packVsCode.data(),packVsCode.size(),"vs_main",RenderShaderFormat::DXIL);
        packPs=device->createShader(packPsCode.data(),packPsCode.size(),"ps_main",RenderShaderFormat::DXIL);
        floatPs=floatPsCode.empty()?nullptr:device->createShader(floatPsCode.data(),floatPsCode.size(),"float_main",RenderShaderFormat::DXIL);
        RenderGraphicsPipelineDesc desc;
        desc.pipelineLayout = layout.get();
        desc.vertexShader = packVs.get();
        desc.pixelShader = packPs.get();
        desc.renderTargetFormat[0] = RenderFormat::R8G8B8A8_UNORM;
        desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
        desc.renderTargetCount = 1;
        desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        packPipeline = device->createGraphicsPipeline(desc);
        if (floatPs)
        {
            desc.pixelShader = floatPs.get();
            desc.renderTargetFormat[0] = RenderFormat::R32_FLOAT;
            floatPipeline = device->createGraphicsPipeline(desc);
        }
        if(!frame.continuous||CachedEnv("FD_REPLAY_TRACE"))fprintf(stderr, "fd_replay: depth pack pipeline %s (vs %zu bytes, ps %zu bytes)\n", packPipeline && static_cast<D3D12GraphicsPipeline*>(packPipeline.get())->d3d ? "ok" : "FAILED", packVsCode.size(), packPsCode.size());
    }
    constexpr uint32_t kMaxPacks = 64;
    uint32_t packAt = 0;
    auto& packConstants=storage.packConstants;
    if(!packConstants)packConstants=device->createBuffer(RenderBufferDesc::UploadBuffer(256*kMaxPacks,RenderBufferFlag::CONSTANT));
    auto* packMap = static_cast<uint8_t*>(packConstants->map());
    const uint32_t packOrder = getenv("FD_DEPTH_PACK_ORDER") ? uint32_t(atoi(getenv("FD_DEPTH_PACK_ORDER"))) : 0;
    std::vector<std::unique_ptr<Packed>> retiredPacked;
    std::vector<std::unique_ptr<Snapshot>> retiredDepthSnapshots; // keep resized copies alive until execution completes
    std::vector<std::unique_ptr<Snapshot>> retiredSnapshots;
    struct Dump
    {
        bool depth = false;
        uint32_t dest = 0, order = 0, width = 0, height = 0, rowPitch = 0;
        RenderFormat format = RenderFormat::UNKNOWN;
        std::unique_ptr<RenderBuffer> buffer;
    };
    std::vector<Dump> dumps;
    uint32_t colorDumpCount = 0, depthDumpCount = 0;
    std::map<uint64_t, std::pair<uint32_t, std::unique_ptr<RenderTextureView>>> tableViews; // (texture, swizzle) -> view
    uint32_t aliasHits = 0;
    Surface* presentationSurface = nullptr;
    uint32_t presentationWidth = 0, presentationHeight = 0;
    auto resolveColor = [&](Surface& rt) {
        if (!rt.multisample) return;
        list->barriers(RenderBarrierStage::COPY, {
            RenderTextureBarrier(rt.multisample.get(), RenderTextureLayout::RESOLVE_SOURCE),
            RenderTextureBarrier(rt.texture.get(), RenderTextureLayout::RESOLVE_DEST)});
        list->resolveTexture(rt.texture.get(), rt.multisample.get());
        list->barriers(RenderBarrierStage::GRAPHICS, {
            RenderTextureBarrier(rt.multisample.get(), RenderTextureLayout::COLOR_WRITE),
            RenderTextureBarrier(rt.texture.get(), RenderTextureLayout::COLOR_WRITE)});
        rt.shaderRead = false;
    };
    // DLAA / DLSS on the scene copy: camera motion + linear depth pass, NGX
    // evaluation, then the result replaces the copy the post-processing reads.
    auto runUpscale=[&](const Draw& d,Target& t){
        auto snapIt=snapshots.find(d.copyDest);
        if(snapIt==snapshots.end()||!t.ds||!t.ds->texture)return;
        Snapshot& snap=*snapIt->second;
        auto& up=upscale;
        // Diagnostic frame dumps: scene before / after the upscaler and the
        // motion vectors (resolve_<n>_DLSS0000 / DLSS0001 / DLSSMV00).
        auto dumpTexture=[&](RenderTexture* texture,RenderFormat format,uint32_t width,uint32_t height,uint32_t tag,RenderTextureLayout restore){
            if(dir.empty())return;
            Dump dump;
            dump.dest=tag;dump.order=colorDumpCount++;dump.format=format;dump.width=width;dump.height=height;
            dump.rowPitch=(width*FormatBytes(format)+255)&~255u;
            dump.buffer=device->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(dump.rowPitch)*height));
            list->barriers(RenderBarrierStage::COPY,RenderTextureBarrier(texture,RenderTextureLayout::COPY_SOURCE));
            list->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(dump.buffer.get(),format,width,height,1,dump.rowPitch/FormatBytes(format)),
                RenderTextureCopyLocation::Subresource(texture));
            list->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(texture,restore));
            dumps.push_back(std::move(dump));
        };
        auto* d3dDevice=externalDevice?externalDevice.Get():static_cast<D3D12Device*>(device.get())->d3d;
        const bool dlaa=upscaleMode!=UpscalerMode::Off&&up.ngx.Initialize(d3dDevice);
        if(!dlaa&&!frameGen)return;
        const uint32_t w=snap.width,h=snap.height;
        // Super Resolution: the scene renders at renderScale, DLSS writes it at outputScale.
        const uint32_t outW=superResolution?scaledSize(fbW,outputScale):w,outH=superResolution?scaledSize(fbH,outputScale):h;
        if(!up.pipeline)
        {
            up.vs=device->createShader(fd_native_motion_vs,sizeof(fd_native_motion_vs),"vs_main",RenderShaderFormat::DXIL);
            up.ps=device->createShader(fd_native_motion_ps,sizeof(fd_native_motion_ps),"ps_main",RenderShaderFormat::DXIL);
            RenderGraphicsPipelineDesc desc;
            desc.pipelineLayout=layout.get();
            desc.vertexShader=up.vs.get();
            desc.pixelShader=up.ps.get();
            desc.renderTargetFormat[0]=RenderFormat::R16G16_FLOAT;
            desc.renderTargetFormat[1]=RenderFormat::R32_FLOAT;
            desc.renderTargetBlend[0]=desc.renderTargetBlend[1]=RenderBlendDesc::Copy();
            desc.renderTargetCount=2;
            desc.primitiveTopology=RenderPrimitiveTopology::TRIANGLE_LIST;
            up.pipeline=device->createGraphicsPipeline(desc);
            up.constants=device->createBuffer(RenderBufferDesc::UploadBuffer(512,RenderBufferFlag::CONSTANT));
            if(!up.pipeline||!static_cast<D3D12GraphicsPipeline*>(up.pipeline.get())->d3d){fprintf(stderr,"native upscaler: motion pipeline failed\n");up.pipeline.reset();return;}
        }
        if(up.width!=w||up.height!=h||up.outputFormat!=snap.format||up.outWidth!=outW||up.outHeight!=outH)
        {
            up.motion=CreateTexture(device.get(),RenderTextureDesc::ColorTarget(w,h,RenderFormat::R16G16_FLOAT));
            up.depth=CreateTexture(device.get(),RenderTextureDesc::ColorTarget(w,h,RenderFormat::R32_FLOAT));
            up.output=CreateTexture(device.get(),RenderTextureDesc::Texture2D(outW,outH,1,snap.format,RenderTextureFlag::UNORDERED_ACCESS));
            up.outWidth=outW;up.outHeight=outH;
            up.upscaled=Surface();
            if(outW!=w||outH!=h)
            {
                up.upscaled.format=snap.format;
                up.upscaled.texture=CreateTexture(device.get(),RenderTextureDesc::Texture2D(outW,outH,1,snap.format));
                boundSurfaces.erase(&up.upscaled);
                bindSurface(up.upscaled);
            }
            const RenderTexture* colors[2]={up.motion.get(),up.depth.get()};
            RenderFramebufferDesc fb;fb.colorAttachments=colors;fb.colorAttachmentsCount=2;
            list->barriers(RenderBarrierStage::GRAPHICS,{RenderTextureBarrier(up.motion.get(),RenderTextureLayout::COLOR_WRITE),RenderTextureBarrier(up.depth.get(),RenderTextureLayout::COLOR_WRITE)});
            up.framebuffer=device->createFramebuffer(fb);
            up.width=w;up.height=h;up.outputFormat=snap.format;up.reset=true;
        }
        // Motion constants: current clip (ndc, w = 1) -> previous clip.
        struct MotionConstants
        {
            float currentToPrevious[16];
            float viewport[4],previousViewport[4];
            float depthTransform[2];
            uint32_t depthIndex,sampleCount,sampleIndex,valid;
        } c{};
        float inverse[16];
        const bool valid=sceneCamera&&up.hasPrevious&&Invert4x4(sceneVP,inverse);
        if(valid)Multiply4x4(up.previousVP,inverse,c.currentToPrevious);
        const float* previousViewport=up.hasPrevious?up.previousViewport:sceneViewport;
        std::copy(sceneViewport,sceneViewport+4,c.viewport);
        std::copy(previousViewport,previousViewport+4,c.previousViewport);
        // Guest viewport -> host pixels of the (scaled) scene.
        for(float& v:c.viewport)v*=float(renderScale);
        for(float& v:c.previousViewport)v*=float(renderScale);
        c.depthTransform[0]=sceneDepth[0]?sceneDepth[0]:1.0f;c.depthTransform[1]=sceneDepth[1];
        c.depthIndex=t.ds->descriptor;c.sampleCount=t.ds->samples;c.sampleIndex=0;c.valid=valid;
        memcpy(up.constants->map(),&c,sizeof(c));up.constants->unmap();
        list->barriers(RenderBarrierStage::GRAPHICS,{
            RenderTextureBarrier(t.ds->texture.get(),RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(up.motion.get(),RenderTextureLayout::COLOR_WRITE),
            RenderTextureBarrier(up.depth.get(),RenderTextureLayout::COLOR_WRITE)});
        list->setFramebuffer(up.framebuffer.get());
        list->setGraphicsPipelineLayout(layout.get());
        for(uint32_t set=0;set<5;++set)list->setGraphicsDescriptorSet(sets[set].get(),set);
        list->setPipeline(up.pipeline.get());
        list->setGraphicsRootDescriptor(RenderBufferReference(up.constants.get(),0),2);
        list->setViewports(RenderViewport(0,0,float(w),float(h)));
        list->setScissors(RenderRect(0,0,w,h));
        list->drawInstanced(3,1,0,0);
        list->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(t.ds->texture.get(),RenderTextureLayout::DEPTH_WRITE));
        t.ds->shaderRead=false;
        auto* commandList=static_cast<D3D12CommandList*>(list.get());
        if(frameGen)
        {
            // Streamline copies the depth and motion vectors now (they are
            // rewritten by the next frame before this one is presented).
            fg::FrameInputs inputs;
            inputs.list=commandList->d3d;
            inputs.depth=static_cast<D3D12Texture*>(up.depth.get())->d3d;
            inputs.motion=static_cast<D3D12Texture*>(up.motion.get())->d3d;
            inputs.width=w;inputs.height=h;
            std::copy(c.currentToPrevious,c.currentToPrevious+16,inputs.clipToPrevClip);
            inputs.valid=valid;inputs.reset=up.reset||!valid;
            inputs.jitterX=jitterX;inputs.jitterY=jitterY;
            fg::Tag(inputs);
            commandList->notifyDescriptorHeapWasChangedExternally();
            commandList->activeTopology=D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
            commandList->activeGraphicsPipeline=nullptr;
            commandList->activeGraphicsPipelineLayout=nullptr;
            commandList->activeComputePipelineLayout=nullptr;
            commandList->targetFramebuffer=nullptr;
            current=nullptr;
            if(!dlaa)up.reset=false;
        }
        if(!dlaa)
        {
            if(sceneCamera)
            {
                std::copy(sceneVP,sceneVP+16,up.previousVP);
                std::copy(sceneViewport,sceneViewport+4,up.previousViewport);
                up.hasPrevious=true;
            }
            return;
        }
        dumpTexture(snap.surface.texture.get(),snap.format,w,h,0xD1550000,RenderTextureLayout::SHADER_READ);
        dumpTexture(up.motion.get(),RenderFormat::R16G16_FLOAT,w,h,0xD155AA00,RenderTextureLayout::COLOR_WRITE);
        // NGX reads in compute and writes the output as a UAV.
        list->barriers(RenderBarrierStage::COMPUTE,{
            RenderTextureBarrier(snap.surface.texture.get(),RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(up.motion.get(),RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(up.depth.get(),RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(up.output.get(),RenderTextureLayout::GENERAL)});
        UpscalerSetup setup;
        setup.renderWidth=w;setup.renderHeight=h;setup.outputWidth=outW;setup.outputHeight=outH;
        setup.mode=upscaleMode;
        setup.hdr=snap.format==RenderFormat::R16G16B16A16_FLOAT||snap.format==RenderFormat::R32G32B32A32_FLOAT;
        UpscalerFrame in;
        in.color=static_cast<D3D12Texture*>(snap.surface.texture.get())->d3d;
        in.depth=static_cast<D3D12Texture*>(up.depth.get())->d3d;
        in.motion=static_cast<D3D12Texture*>(up.motion.get())->d3d;
        in.output=static_cast<D3D12Texture*>(up.output.get())->d3d;
        static const float jitterSign=getenv("FD_UPSCALER_JITTER_FLIP")?-1.0f:1.0f;
        in.jitterX=jitterX*jitterSign;in.jitterY=jitterY*jitterSign;
        const auto now=std::chrono::steady_clock::now();
        in.frameTimeMs=up.last.time_since_epoch().count()?float(std::chrono::duration<double,std::milli>(now-up.last).count()):16.7f;
        up.last=now;
        in.reset=up.reset||!valid;
        const bool evaluated=up.ngx.Evaluate(commandList->d3d,setup,in);
        // NGX changes the descriptor heaps, root signature, pipeline and topology.
        commandList->notifyDescriptorHeapWasChangedExternally();
        commandList->activeTopology=D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        commandList->activeGraphicsPipeline=nullptr;
        commandList->activeGraphicsPipelineLayout=nullptr;
        commandList->activeComputePipelineLayout=nullptr;
        commandList->targetFramebuffer=nullptr;
        current=nullptr;
        if(evaluated&&!CachedEnv("FD_UPSCALER_BYPASS"))
        {
            // DLAA writes back into the scene copy; Super Resolution into its
            // own output-size surface, which the post-processing then reads.
            RenderTexture* destination=up.upscaled.texture?up.upscaled.texture.get():snap.surface.texture.get();
            list->barriers(RenderBarrierStage::COPY,{
                RenderTextureBarrier(up.output.get(),RenderTextureLayout::COPY_SOURCE),
                RenderTextureBarrier(destination,RenderTextureLayout::COPY_DEST)});
            list->copyTextureRegion(RenderTextureCopyLocation::Subresource(destination,0),
                RenderTextureCopyLocation::Subresource(up.output.get(),0));
            if(up.upscaled.texture)
            {
                list->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(destination,RenderTextureLayout::SHADER_READ));
                up.upscaled.shaderRead=true;
                resolved[d.copyDest]=&up.upscaled;
            }
            if(++up.evaluated%600==1)fprintf(stderr,"native upscaler: %s applied (%llu frames, camera %d, jitter %.3f %.3f)\n",UpscalerModeName(upscaleMode),(unsigned long long)up.evaluated,int(sceneCamera),jitterX,jitterY);
            up.reset=false;
        }
        list->barriers(RenderBarrierStage::GRAPHICS,{
            RenderTextureBarrier(snap.surface.texture.get(),RenderTextureLayout::SHADER_READ),
            RenderTextureBarrier(up.motion.get(),RenderTextureLayout::COLOR_WRITE),
            RenderTextureBarrier(up.depth.get(),RenderTextureLayout::COLOR_WRITE)});
        snap.surface.shaderRead=true;
        dumpTexture(snap.surface.texture.get(),snap.format,w,h,0xD1550001,RenderTextureLayout::SHADER_READ);
        if(sceneCamera)
        {
            std::copy(sceneVP,sceneVP+16,up.previousVP);
            std::copy(sceneViewport,sceneViewport+4,up.previousViewport);
            up.hasPrevious=true;
        }
    };
    // FD_STOP_AFTER=<record>: debug, replay only up to that record.
    const size_t stopAfter = getenv("FD_STOP_AFTER") ? size_t(atoi(getenv("FD_STOP_AFTER"))) : draws.size();
    for (size_t i = 0; i < draws.size() && i <= stopAfter; ++i)
    {
        const Draw& d = draws[i];
        const auto profileDrawStart=profile?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
        struct RecordTimer
        {
            bool enabled;double& total;std::chrono::steady_clock::time_point start;
            ~RecordTimer(){if(enabled)total+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();}
        } recordTimer{profile,profileDrawMs[d.clear?0:d.resolve?1:2],profileDrawStart};
        if (d.clear)
        {
            if (CachedEnv("FD_NO_CLEARS")) continue;
            // A clear followed immediately by Resolve has no draw to allocate
            // its target (the title screen's neutral refraction buffer).
            if ((d.clearFlags & 0xF)||(frame.continuous&&(d.clearFlags&0x30))) targetFor(d);
            for (uint32_t k = 0; k < 4; ++k)
                if (d.clearFlags >> k & 1)
                    colorClears[{d.regs[RB_SURFACE_INFO], ColorInfo(d.regs, k)}] = &d;
            if (d.clearFlags & 0x30)
                depthClears[{d.regs[RB_SURFACE_INFO], d.regs[RB_DEPTH_INFO]}] = &d;
            // Other colour attachments may give the same EDRAM depth surface
            // several host target configurations. A clear affects all of them.
            std::set<Surface*> clearedColors,clearedDepths;
            for (auto& [key, t] : targets)
            {
                if (std::get<0>(key) != d.regs[RB_SURFACE_INFO]) continue;
                // All attachments must have writable layouts before binding,
                // including shared depth previously sampled by another pass.
                for (uint32_t k = 0; k < t.count; ++k)
                {
                    auto& rt = *t.rt[k];
                    list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(rt.texture.get(), RenderTextureLayout::COLOR_WRITE));
                    if (rt.multisample)
                        list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(rt.multisample.get(), RenderTextureLayout::COLOR_WRITE));
                    rt.shaderRead = false;
                }
                list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(t.ds->texture.get(), RenderTextureLayout::DEPTH_WRITE));
                t.ds->shaderRead = false;
                list->setFramebuffer(t.framebuffer.get());
                bool wrote=false;
                for (uint32_t k = 0; k < t.count; ++k)
                    for (uint32_t c = 0; c < 4; ++c)
                        if ((d.clearFlags >> c & 1) && ColorInfo(d.regs, c) ==
                            (k == 0 ? std::get<1>(key) : k == 1 ? std::get<2>(key) : k == 2 ? std::get<3>(key) : std::get<4>(key))&&clearedColors.insert(t.rt[k].get()).second)
                        {
                            auto& rt = *t.rt[k];
                            rt.explicitlyCleared = true;
                            wrote=true;
                            const auto rects=scaledRects(d,t.scale);
                            list->clearColor(k, clearColorValue(d, rt.format), rects.data(), uint32_t(rects.size()));
                        }
                if ((d.clearFlags & 0x30) && std::get<5>(key) == d.regs[RB_DEPTH_INFO]&&clearedDepths.insert(t.ds).second)
                {
                    wrote=true;
                    const auto rects=scaledRects(d,t.scale);
                    list->clearDepthStencil((d.clearFlags & 0x10) != 0, (d.clearFlags & 0x20) != 0,
                        d.clearDepth, d.clearStencil, rects.data(), uint32_t(rects.size()));
                }
                if(wrote)t.lastWrite=++storage.writeSerial;
            }
            current = nullptr;
            continue;
        }
        if (d.resolve)
        {
            // Copy source: the target configured when the copy runs. Only
            // color copies (copy_src_select 0-3) are aliased for now.
            const uint32_t source = d.copyControl & 7; // copy_src_select: 0-3 color RT, 4 depth
            if (source < 4) presentationSurface = nullptr;
            // A depth copy addresses depth storage independently of the colour
            // attachments. XDK copy setup may leave RB_COLOR_INFO pointing at
            // an unrelated (or never allocated) colour surface.
            // Colour source: the exact configuration, else the one with the same
            // surface and colour (EDRAM base and format; depth may differ, e.g.
            // when the draws' state comes from Xenia and the copy's from our
            // command parsing), else the same colour info; most draws wins.
            const Target* best = nullptr;
            for (int pass = 0; pass < 3 && !best; ++pass)
                for (auto& [key, t] : targets)
                {
                    const bool match = source == 4
                        ? pass == 0 && std::get<0>(key) == d.copySurface && std::get<5>(key) == d.copyDepth
                        : pass == 0 ? std::get<0>(key) == d.copySurface && std::get<1>(key) == d.copyColor && (frame.continuous||std::get<5>(key) == d.copyDepth)
                        : pass == 1 ? std::get<0>(key) == d.copySurface && std::get<1>(key) == d.copyColor
                        : std::get<1>(key) == d.copyColor;
                    auto relevance=[&](const Target& candidate) {
                        uint64_t value=candidate.draws;
                        if(frame.continuous)
                        {
                            // EDRAM configurations with different extents may
                            // share an address across scene changes. The most
                            // recent writer owns the copy, including an older
                            // frame when no writes occurred in this one.
                            value=candidate.lastWrite;
                        }
                        return value;
                    };
                    if (match && (source == 4 || t.draws || (source < t.count && t.rt[source]->explicitlyCleared)||
                        (frame.continuous&&t.hadContents)) && (!best || relevance(t)>relevance(*best))) best = &t;
                }
            for (auto& [key, t] : targets)
                if (&t == best)
                {
                    if (source < t.count) resolveColor(*t.rt[source]);
                    const uint32_t depthSamples = multisampling(std::get<0>(key)).sampleCount;
                    const uint32_t guestSample = (d.copyControl >> 4) & 7;
                    const uint32_t depthSample = depthSamples == 2 ? ((guestSample & 1) ^ 1) : guestSample & (depthSamples - 1);
                    // RB_COPY_DEST_INFO bit 24: the copy swaps red and blue in memory.
                    resolvedSwap[d.copyDest] = (d.copyDestInfo >> 24) & 1;
                    // copy_dest_exp_bias (bits 16-21, signed): memory = value * 2^bias.
                    resolvedBias[d.copyDest] = int32_t(d.copyDestInfo << 10) >> 26;
                    if (CachedEnv("FD_REPLAY_TRACE"))
                        fprintf(stderr, "resolve %08X info %08X src %u bias %d count %u\n", d.copyDest, d.copyDestInfo,
                            source, resolvedBias[d.copyDest], t.count);
                    // Copy size in host pixels (scaled targets copy their scaled contents).
                    const uint32_t copyW = (std::min)(scaledSize((std::clamp)(d.copyWidth, 1u, t.guestWidth), t.scale), t.width);
                    const uint32_t copyH = (std::min)(scaledSize((std::clamp)(d.copyHeight, 1u, t.guestHeight), t.scale), t.height);
                    if (source < t.count)
                    {
                        presentationSurface = t.rt[source].get();
                        presentationWidth = copyW;
                        presentationHeight = copyH;
                    }
                    if (source == 4 && t.ds->texture && floatPipeline && packAt < kMaxPacks && nextView < descriptorBase + kMaxTargets)
                    {
                        // A depth resolve may later be fetched as D24(F)S8 or as
                        // raw RGBA bytes. Preserve a float snapshot as well as
                        // the byte-packed view below; later depth writes must
                        // not change the copy already visible to shaders.
                        auto& snap = depthSnapshots[d.copyDest];
                        if (!snap || snap->width != copyW || snap->height != copyH)
                        {
                            if (snap) retiredDepthSnapshots.push_back(std::move(snap));
                            snap = std::make_unique<Snapshot>();
                            snap->format = snap->surface.format = RenderFormat::R32_FLOAT;
                            snap->width = copyW;
                            snap->height = copyH;
                            snap->surface.isDepth = true;
                            snap->surface.texture = CreateTexture(device.get(), RenderTextureDesc::ColorTarget(copyW, copyH, snap->format));
                            const RenderTexture* color = snap->surface.texture.get();
                            RenderFramebufferDesc fb;
                            fb.colorAttachments = &color;
                            fb.colorAttachmentsCount = 1;
                            snap->framebuffer = device->createFramebuffer(fb);
                            snap->surface.descriptor = nextView++;
                            sets[0]->setTexture(snap->surface.descriptor, snap->surface.texture.get(), RenderTextureLayout::SHADER_READ);
                        }
                        const uint32_t constants[5] = {t.ds->descriptor, (d.copyDepth >> 16) & 1, 0, depthSamples, depthSample};
                        memcpy(packMap + 256 * packAt, constants, sizeof(constants));
                        list->barriers(RenderBarrierStage::GRAPHICS, {
                            RenderTextureBarrier(t.ds->texture.get(), RenderTextureLayout::SHADER_READ),
                            RenderTextureBarrier(snap->surface.texture.get(), RenderTextureLayout::COLOR_WRITE)});
                        list->setFramebuffer(snap->framebuffer.get());
                        list->setGraphicsPipelineLayout(layout.get());
                        for (uint32_t set = 0; set < 5; ++set) list->setGraphicsDescriptorSet(sets[set].get(), set);
                        list->setPipeline(floatPipeline.get());
                        list->setGraphicsRootDescriptor(RenderBufferReference(packConstants.get(), 256 * packAt++), 2);
                        list->setViewports(RenderViewport(0, 0, float(copyW), float(copyH)));
                        list->setScissors(RenderRect(0, 0, copyW, copyH));
                        list->drawInstanced(3, 1, 0, 0);
                        current = nullptr;
                        list->barriers(RenderBarrierStage::GRAPHICS, {
                            RenderTextureBarrier(t.ds->texture.get(), RenderTextureLayout::DEPTH_WRITE),
                            RenderTextureBarrier(snap->surface.texture.get(), RenderTextureLayout::SHADER_READ)});
                        t.ds->shaderRead = false;
                        snap->surface.shaderRead = true;
                        resolvedDepth[d.copyDest] = &snap->surface;
                        if (CachedEnv("FD_DUMP_DEPTH"))
                        {
                            Dump dump;
                            dump.depth = true;
                            dump.dest = d.copyDest;
                            dump.order = depthDumpCount++;
                            dump.format = snap->format;
                            dump.width = copyW;
                            dump.height = copyH;
                            dump.rowPitch = (copyW * 4 + 255) & ~255u;
                            dump.buffer = device->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(dump.rowPitch) * copyH));
                            list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(snap->surface.texture.get(), RenderTextureLayout::COPY_SOURCE));
                            list->copyTextureRegion(
                                RenderTextureCopyLocation::PlacedFootprint(dump.buffer.get(), dump.format, copyW, copyH, 1, dump.rowPitch / 4),
                                RenderTextureCopyLocation::Subresource(snap->surface.texture.get()));
                            list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(snap->surface.texture.get(), RenderTextureLayout::SHADER_READ));
                            dumps.push_back(std::move(dump));
                        }
                    }
                    // Each resolve preserves this moment's contents. EDRAM is
                    // reused for unrelated passes later in the same frame.
                    if (!frame.continuous && source < t.count && t.rt[source]->texture && !t.draws && !t.rt[source]->explicitlyCleared)
                    {
                        // Nothing drawn yet this frame: on the Xbox the target still
                        // holds the previous frame; the captured memory copy is closer.
                        resolved.erase(d.copyDest);
                    }
                    else if (source < t.count && t.rt[source]->texture && (CachedEnv("FD_NO_SNAPSHOT") ||
                        (CachedEnv("FD_LIVE_DEST") && strtoul(CachedEnv("FD_LIVE_DEST"), nullptr, 16) == d.copyDest)))
                    {
                        resolved[d.copyDest] = t.rt[source].get();
                        // Dump the copied rectangle as it is now, for compare_resolves.py.
                        Surface& rt = *t.rt[source];
                        Dump dump;
                        dump.dest = d.copyDest;
                        dump.order = colorDumpCount++;
                        dump.format = rt.format;
                        dump.width = copyW;
                        dump.height = copyH;
                        dump.rowPitch = (t.width * FormatBytes(rt.format) + 255) & ~255u;
                        dump.buffer = device->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(dump.rowPitch) * t.height));
                        list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(rt.texture.get(), RenderTextureLayout::COPY_SOURCE));
                        list->copyTextureRegion(
                            RenderTextureCopyLocation::PlacedFootprint(dump.buffer.get(), rt.format, t.width, t.height, 1, dump.rowPitch / FormatBytes(rt.format)),
                            RenderTextureCopyLocation::Subresource(rt.texture.get()));
                        list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(rt.texture.get(), RenderTextureLayout::COLOR_WRITE));
                        rt.shaderRead = false;
                        dumps.push_back(std::move(dump));
                    }
                    else if (source < t.count && t.rt[source]->texture)
                    {
                        // Partial resolves update one region of the full destination
                        // (for example, the four cascades of a shadow atlas).
                        Surface& rt = *t.rt[source];
                        const uint32_t mipW = (std::max)(1u, d.copyWidth >> d.copyMip);
                        const uint32_t mipH = (std::max)(1u, d.copyHeight >> d.copyMip);
                        if (d.copyX >= mipW || d.copyY >= mipH) continue;
                        const double rs = t.scale;
                        const uint32_t width = scaledSize(d.copyWidth, rs), height = scaledSize(d.copyHeight, rs);
                        const uint32_t regionW = (std::min)(t.width, scaledSize(mipW - d.copyX, rs));
                        const uint32_t regionH = (std::min)(t.height, scaledSize(mipH - d.copyY, rs));
                        const uint32_t copyX = rs == 1.0 ? d.copyX : uint32_t(std::lround(d.copyX * rs));
                        const uint32_t copyY = rs == 1.0 ? d.copyY : uint32_t(std::lround(d.copyY * rs));
                        auto& snap = snapshots[d.copyDest];
                        if (!snap || snap->format != rt.format || snap->width != width || snap->height != height || snap->mipCount != d.copyMipCount)
                        {
                            if (nextView + 1 > descriptorBase + kMaxTargets) throw std::runtime_error("Native snapshot descriptor capacity exceeded");
                            if (snap) retiredSnapshots.push_back(std::move(snap));
                            snap = std::make_unique<Snapshot>();
                            snap->format = rt.format;
                            snap->width = width;
                            snap->height = height;
                            snap->mipCount = d.copyMipCount;
                            snap->surface.format = rt.format;
                            snap->surface.texture = CreateTexture(device.get(), RenderTextureDesc::Texture2D(width, height, d.copyMipCount, rt.format));
                            snap->surface.descriptor = nextView++;
                            sets[0]->setTexture(snap->surface.descriptor, snap->surface.texture.get(), RenderTextureLayout::SHADER_READ);
                        }
                        list->barriers(RenderBarrierStage::COPY, {
                            RenderTextureBarrier(rt.texture.get(), RenderTextureLayout::COPY_SOURCE),
                            RenderTextureBarrier(snap->surface.texture.get(), RenderTextureLayout::COPY_DEST)});
                        const RenderBox box(0, 0, int32_t(regionW), int32_t(regionH));
                        list->copyTextureRegion(RenderTextureCopyLocation::Subresource(snap->surface.texture.get(), d.copyMip),
                            RenderTextureCopyLocation::Subresource(rt.texture.get()), copyX, copyY, 0, &box);
                        // Copy readbacks are diagnostic, never part of normal presentation.
                        if(!dir.empty()) {
                            Dump dump;
                            dump.dest = d.copyDest;
                            dump.order = colorDumpCount++;
                            dump.format = rt.format;
                            // The snapshot's mip in host pixels (scaled targets).
                            const uint32_t dumpW = (std::max)(1u, width >> d.copyMip), dumpH = (std::max)(1u, height >> d.copyMip);
                            dump.width = dumpW;
                            dump.height = dumpH;
                            dump.rowPitch = (dumpW * FormatBytes(rt.format) + 255) & ~255u;
                            dump.buffer = device->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(dump.rowPitch) * dumpH));
                            list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(snap->surface.texture.get(), RenderTextureLayout::COPY_SOURCE));
                            list->copyTextureRegion(
                                RenderTextureCopyLocation::PlacedFootprint(dump.buffer.get(), rt.format, dumpW, dumpH, 1, dump.rowPitch / FormatBytes(rt.format)),
                                RenderTextureCopyLocation::Subresource(snap->surface.texture.get(), d.copyMip));
                            dumps.push_back(std::move(dump));
                        }
                        list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(rt.texture.get(), RenderTextureLayout::COLOR_WRITE));
                        rt.shaderRead = false;
                        snap->surface.shaderRead = false;
                        resolved[d.copyDest] = &snap->surface;
                    }
                    else if (source == 4 && t.ds->texture && ((d.copyDestInfo >> 7) & 0x3F) == 6 && packPipeline &&
                        packAt < kMaxPacks && nextView + 2 <= descriptorBase + kMaxTargets)
                    {
                        auto& p=storage.packed[d.copyDest];
                        if(!p||p->width!=copyW||p->height!=copyH)
                        {
                            if(p)retiredPacked.push_back(std::move(p));
                            p=std::make_unique<Packed>();p->width=copyW;p->height=copyH;
                            p->surface.format=RenderFormat::R8G8B8A8_UNORM;
                            p->surface.texture=CreateTexture(device.get(),RenderTextureDesc::ColorTarget(copyW,copyH,p->surface.format));
                            bindSurface(p->surface);
                            const RenderTexture* color=p->surface.texture.get();
                            RenderFramebufferDesc fb;fb.colorAttachments=&color;fb.colorAttachmentsCount=1;
                            p->framebuffer=device->createFramebuffer(fb);
                        }
                        p->depthView = t.ds->texture->createTextureView(RenderTextureViewDesc::Texture2D(t.ds->format));
                        const uint32_t depthIndex = nextView++;
                        sets[depthSamples > 1 ? 4 : 0]->setTexture(depthIndex, t.ds->texture.get(), RenderTextureLayout::SHADER_READ, p->depthView.get());
                        // RB_DEPTH_INFO bit 16: D24FS8 (20e4 float depth).
                        const uint32_t constants[5] = {depthIndex, (d.copyDepth >> 16) & 1, packOrder, depthSamples, depthSample};
                        memcpy(packMap + 256 * packAt, constants, sizeof(constants));
                        list->barriers(RenderBarrierStage::GRAPHICS, {
                            RenderTextureBarrier(t.ds->texture.get(), RenderTextureLayout::SHADER_READ),
                            RenderTextureBarrier(p->surface.texture.get(), RenderTextureLayout::COLOR_WRITE)});
                        t.ds->shaderRead = true;
                        p->surface.shaderRead=false;
                        list->setFramebuffer(p->framebuffer.get());
                        list->setGraphicsPipelineLayout(layout.get());
                        for (uint32_t set = 0; set < 5; ++set) list->setGraphicsDescriptorSet(sets[set].get(), set);
                        list->setPipeline(packPipeline.get());
                        list->setGraphicsRootDescriptor(RenderBufferReference(packConstants.get(), 256 * packAt++), 2);
                        list->setViewports(RenderViewport(0, 0, float(copyW), float(copyH)));
                        list->setScissors(RenderRect(0, 0, copyW, copyH));
                        list->drawInstanced(3, 1, 0, 0);
                        current = nullptr;
                        resolved[d.copyDest] = &p->surface;
                        resolvedSwap[d.copyDest] = false;
                        if (CachedEnv("FD_REPLAY_TRACE")) fprintf(stderr, "packed depth -> %08X (%ux%u, float %u)\n", d.copyDest, t.width, t.height, (d.copyDepth >> 16) & 1);
                    }
                    else if (source == 4 && t.ds->texture) resolved[d.copyDest] = t.ds;
                }
            if (i == sceneResolve && best) runUpscale(d, const_cast<Target&>(*best));
            continue;
        }
        // FD_TRACE_FETCH=<draw>: debug, the draw's vertex fetch descriptors and
        // the first raw dwords each one reads from the stream space.
        if (const char* traceFetch = CachedEnv("FD_TRACE_FETCH"); traceFetch && size_t(atoi(traceFetch)) == i && d.vertexFetch)
        {
            const uint32_t* fetch = reinterpret_cast<const uint32_t*>(d.vertexFetch);
            for (uint32_t k = 0; k < 32; ++k)
            {
                const uint32_t* f = fetch + k * 4;
                if (!(f[3] >> 31)) continue;
                fprintf(stderr, "fetch %u: base %u stride %u offset %u size %u format %u signed %u int %u exp %d\n", k, f[0], f[1] & 0xFFFF, f[1] >> 16, f[2],
                    f[3] & 0x3F, (f[3] >> 8) & 1, (f[3] >> 9) & 1, int32_t(f[3] << 16) >> 26);
                for (uint32_t v = 0; v < 6; ++v)
                {
                    const size_t at = size_t(f[0]) + size_t(v) * (f[1] & 0xFFFF) + (f[1] >> 16);
                    if (at + 8 > streamFile.size()) break;
                    uint32_t a, b; memcpy(&a, streamFile.data() + at, 4); memcpy(&b, streamFile.data() + at + 4, 4);
                    fprintf(stderr, "   v%u: %08X %08X (%g %g)\n", v, a, b, std::bit_cast<float>(a), std::bit_cast<float>(b));
                }
            }
            if (d.indices && d.indexBytes)
            {
                const bool wide = d.index32;
                const size_t n = d.indexBytes / (wide ? 4 : 2);
                uint32_t lo = ~0u, hi = 0;
                for (size_t k = 0; k < n; ++k)
                {
                    uint32_t v = 0; memcpy(&v, d.indices + k * (wide ? 4 : 2), wide ? 4 : 2);
                    lo = (std::min)(lo, v); hi = (std::max)(hi, v);
                    if (k < 24) fprintf(stderr, "%s%u", k ? " " : "indices: ", v);
                }
                fprintf(stderr, " ... (%zu, range %u..%u)\n", n, lo, hi);
            }
            if (d.psConstants)
                for (uint32_t k = 0; k < 8; ++k)
                {
                    float c[4]; memcpy(c, d.psConstants + 16 * k, 16);
                    fprintf(stderr, "ps c%u: %g %g %g %g\n", k, c[0], c[1], c[2], c[3]);
                }
            for (uint32_t k = 0; k < 32; ++k)
            {
                float c[4]; memcpy(c, d.constants + 16 * k, 16);
                fprintf(stderr, "vs c%u: %.9g %.9g %.9g %.9g\n", k, c[0], c[1], c[2], c[3]);
            }
        }
        // FD_SKIP_DRAWS=<first>-<last>: debug, skip those draw records.
        if (const char* skip = CachedEnv("FD_SKIP_DRAWS"))
        {
            const size_t first = size_t(atoi(skip)), last = strchr(skip, '-') ? size_t(atoi(strchr(skip, '-') + 1)) : first;
            if (i >= first && i <= last) continue;
        }
        const auto copyStart=profile?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
        memcpy(vertexMap + vertexAt, d.vertices, d.vertexBytes);
        memcpy(indexMap + indexAt, d.indices, d.indexBytes);
        const auto indexCopied=profile?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
        uint8_t* drawConstants = constantMap + kPerDraw * (i + 1);
        // Only the registers the shaders read. FD_POISON_CONSTANTS=1 fills the
        // rest with NaN: a shader that reads outside its range shows it.
        static const bool poisonConstants = CachedEnv("FD_POISON_CONSTANTS") != nullptr;
        if (poisonConstants) std::fill_n(reinterpret_cast<uint32_t*>(drawConstants), 2048, 0x7FC00000u);
        if (d.vsConstEnd > d.vsConstFirst)
            memcpy(drawConstants + d.vsConstFirst * 16, d.constants + d.vsConstFirst * 16, size_t(d.vsConstEnd - d.vsConstFirst) * 16);
        // Shadow filter passes at shadowScale: their texel step (VS_VSM_Const, c21).
        if (!shadowDraw.empty() && shadowDraw[i] == 2 && d.vsConstFirst <= 21 && d.vsConstEnd > 21 && !CachedEnv("FD_SHADOW_KEEP_FILTER"))
        {
            float* step = reinterpret_cast<float*>(drawConstants + 21 * 16);
            for (int k = 0; k < 4; ++k) step[k] /= float(shadowScale);
        }
        if (d.psConstEnd > d.psConstFirst && d.psConstants)
            memcpy(drawConstants + 4096 + d.psConstFirst * 16, d.psConstants + d.psConstFirst * 16, size_t(d.psConstEnd - d.psConstFirst) * 16);
        uint8_t* shared = drawConstants + 8192;
        memset(shared, 0, kSharedSize);
        if(profile)
        {
            const auto copied=std::chrono::steady_clock::now();
            profileCopyMs[0]+=std::chrono::duration<double,std::milli>(indexCopied-copyStart).count();
            profileCopyMs[1]+=std::chrono::duration<double,std::milli>(copied-indexCopied).count();
        }
        const auto [viewportWidth,viewportHeight]=targetExtent(d);
        const auto nativeViewport=BuildViewport(d,viewportWidth,viewportHeight);
        float halfPixel[2]={nativeViewport.halfPixel[0],nativeViewport.halfPixel[1]};
        if(!jitterDraw.empty()&&jitterDraw[i])
        {
            // Shift the rasterized geometry by the jitter in pixels (+x right, +y down).
            // The jitter is in host (render) pixels; the offsets are in guest NDC.
            const float jx=float(jitterX/renderScale),jy=float(jitterY/renderScale);
            if(nativeViewport.format&0x80000000){halfPixel[0]+=2*jx/float(viewportWidth);halfPixel[1]-=2*jy/float(viewportHeight);}
            else{halfPixel[0]+=jx/AsFloat(d.regs[VPORT_XSCALE]);halfPixel[1]+=jy/AsFloat(d.regs[VPORT_YSCALE]);}
        }
        memcpy(shared+520,halfPixel,sizeof(halfPixel));
        memcpy(shared+1712,nativeViewport.xy,sizeof(nativeViewport.xy));
        memcpy(shared+1728,nativeViewport.z,sizeof(nativeViewport.z));
        memcpy(shared+1736,&nativeViewport.format,4);
        // g_Booleans (512): vertex bools in bits 0-15, pixel bools (Xenos bool
        // 128+, register 0x4904) in bits 16-31; loop constants at 544.
        const uint32_t booleans = (d.boolLoop[0] & 0xFFFF) | ((d.boolLoop[4] & 0xFFFF) << 16);
        memcpy(shared + 512, &booleans, 4);
        memcpy(shared + 544, &d.boolLoop[8], 32 * 4);
        memcpy(shared + kSharedAllBooleans, d.boolLoop, 8 * 4);
        memcpy(shared + kSharedVertexFetch, d.vertexFetch, 512);
        // D3D12 SV_VertexID excludes BaseVertexLocation. Restore the Xenos
        // VGT index offset for shader-side vertex fetch addressing.
        memcpy(shared + 532,&d.regs[VGT_INDX_OFFSET],4);
        // Alpha test (RB_COLORCONTROL 0x2202: function bits 0-2, enable bit 3;
        // RB_ALPHA_REF 0x210E). The shaders end with clip(oC0.w - threshold)
        // when spec bit 1 (g_NativeSpecConstants, 536) is set: exact for
        // GREATER / GEQUAL, which From Dust uses (vegetation impostors);
        // NEVER discards everything; LESS / EQUAL forms are left untested.
        {
            uint32_t spec = 0;
            float threshold = 0;
            if (d.hasContext && (d.context[0x202] & 0x8) && !CachedEnv("FD_NO_ALPHA_TEST"))
            {
                const uint32_t function = d.context[0x202] & 7;
                const float reference = AsFloat(d.context[0x10E]);
                if (function == 4 || function == 6) { spec = 2; threshold = function == 4 ? std::nextafter(reference, 2.0f) : reference; }
                else if (function == 0) { spec = 2; threshold = 2.0f; }
            }
            // Alpha to mask (bit 4; tree impostors): coverage from alpha.
            // Approximated as a cut at half coverage (hard edges).
            else if (d.hasContext && (d.context[0x202] & 0x10) && !CachedEnv("FD_NO_ALPHA_TEST"))
            {
                spec = 2; threshold = 0.5f;
            }
            memcpy(shared + 528, &threshold, 4);
            memcpy(shared + 536, &spec, 4);
        }
        // fdFetchScale (1536 + slot * 4): 1, or for a render target read in
        // place of its biased copy, 2^(copy bias + fetch exp_adjust).
        // fdColorScale (1664 + k * 4): render target k's colour exponent bias,
        // RB_COLOR_INFO bits 20-25 (signed): the output is stored times 2^bias.
        for (uint32_t k = 0; k < 4; ++k)
        {
            const int bias = noColorBias ? 0 : int32_t(ColorInfo(d.regs, k) << 6) >> 26;
            const float scale = std::ldexp(1.0f, bias - (FixedSnorm(colorFormat(ColorInfo(d.regs, k))) ? 5 : 0));
            memcpy(shared + kSharedColorScale + k * 4, &scale, 4);
        }
        // FD_ZERO_SLOTS=<ps>:<slot>[,<slot>...]: debug, zero those fetches of that pixel shader.
        static const char* zeroSlots = getenv("FD_ZERO_SLOTS");
        for (uint32_t slot = 0; slot < 32; ++slot)
        {
            float one = 1.0f;
            if (zeroSlots && d.ps == std::string(zeroSlots).substr(0, std::string(zeroSlots).find(':')))
            {
                const std::string list = "," + std::string(zeroSlots).substr(std::string(zeroSlots).find(':') + 1) + ",";
                if (list.find("," + std::to_string(slot) + ",") != std::string::npos) one = 0.0f;
            }
            memcpy(shared + kSharedFetchScale + slot * 4, &one, 4);
        }
        const uint32_t usedTextures=shaderCatalog?(shaderCatalog->Find(d.vs,ShaderStage::Vertex)->textureMask|
            (d.ps.empty()?0:shaderCatalog->Find(d.ps,ShaderStage::Pixel)->textureMask)):UINT32_MAX;
        Target& drawTarget=targetFor(d);
        // vPos in guest pixels for scaled targets (XenosRecomp shader_common.h).
        const float pixelScaleMinusOne=float(drawTarget.scale-1.0);
        memcpy(shared+1740,&pixelScaleMinusOne,4);
        for (uint32_t slot = 0; slot < 32; ++slot)
        {
            if(!(usedTextures>>slot&1))continue;
            const uint32_t samplerIndex = samplerFor(d, slot);
            memcpy(shared + kSharedSamplerIndex + slot * 4, &samplerIndex, 4);
            uint32_t index = d.slots[slot] >= 0 ? uint32_t(d.slots[slot]) : 0;
            uint32_t cubeIndex = 0;
            if (d.slots[slot] >= 0 && cubesByIndex[d.slots[slot]])
            {
                cubeIndex = uint32_t(d.slots[slot]);
                if (d.swizzles[slot] != kIdentitySwizzle)
                {
                    const uint64_t key = uint64_t(cubeIndex) << 32 | d.swizzles[slot];
                    auto found = cubeSwizzles.find(key);
                    if (found == cubeSwizzles.end() && nextCubeView < descriptorBase + kMaxTargets)
                    {
                        auto desc = RenderTextureViewDesc::TextureCube(RenderFormat::R8G8B8A8_UNORM);
                        desc.componentMapping = SwizzleMapping(d.swizzles[slot]);
                        auto view = cubesByIndex[cubeIndex]->createTextureView(desc);
                        const uint32_t descriptor = nextCubeView++;
                        sets[2]->setTexture(descriptor, cubesByIndex[cubeIndex], RenderTextureLayout::SHADER_READ, view.get());
                        cubeViews.push_back(std::move(view));
                        found = cubeSwizzles.emplace(key, descriptor).first;
                    }
                    if (found != cubeSwizzles.end()) cubeIndex = found->second;
                }
            }
            memcpy(shared + kSharedTextureIndex + 256 + slot * 4, &cubeIndex, 4);
            // Table textures are RGBA = Xenos components XYZW; the fetch swizzle
            // picks what the shader sees, through a view when not identity.
            if (d.slots[slot] >= 0 && d.swizzles[slot] != kIdentitySwizzle)
            {
                const uint64_t viewKey = uint64_t(uint32_t(d.slots[slot])) << 32 | d.swizzles[slot];
                auto view = tableViews.find(viewKey);
                if (view == tableViews.end() && nextView < descriptorBase + kMaxTargets)
                {
                    RenderTextureViewDesc viewDesc = RenderTextureViewDesc::Texture2D(textureFormats[d.slots[slot]]);
                    viewDesc.componentMapping = SwizzleMapping(d.swizzles[slot]);
                    auto textureView = textures[d.slots[slot]]->createTextureView(viewDesc);
                    sets[0]->setTexture(nextView, textures[d.slots[slot]].get(), RenderTextureLayout::SHADER_READ, textureView.get());
                    view = tableViews.emplace(viewKey, std::make_pair(nextView++, std::move(textureView))).first;
                }
                if (view != tableViews.end()) index = view->second.first;
            }
            const bool depthFetch = d.fetchFormats[slot] == 22 || d.fetchFormats[slot] == 23;
            auto& aliases = depthFetch ? resolvedDepth : resolved;
            auto alias = d.bases[slot] ? aliases.find(d.bases[slot]) : aliases.end();
            if(d.cpuTextureMask>>slot&1)alias=aliases.end();
            if (staticDestOption && staticDest == d.bases[slot]) alias = aliases.end();
            const bool own = alias != aliases.end() && (alias->second == drawTarget.ds ||
                std::any_of(std::begin(drawTarget.rt), std::end(drawTarget.rt), [&](const auto& rt) { return rt.get() == alias->second; }));
            if (alias != aliases.end() && !own)
            {
                // A resolved read sees the target through the fetch constant's
                // swizzle (component i of the result = target channel swizzle[i]).
                Surface& source = *alias->second;
                uint32_t w = d.swizzles[slot];
                const bool swap = !depthFetch && resolvedSwap[alias->first];
                const int32_t bias = depthFetch ? 0 : resolvedBias[alias->first];
                if (swap) // memory X/Z are target B/R
                    for (uint32_t c = 0; c < 4; ++c)
                    {
                        const uint32_t sel = w >> (3 * c) & 7;
                        if (sel == 0 || sel == 2) w ^= (sel ^ (2 - sel)) << (3 * c);
                    }
                auto view = source.views.find(w);
                if (view == source.views.end() && nextView < descriptorBase + kMaxTargets)
                {
                    RenderTextureViewDesc viewDesc = RenderTextureViewDesc::Texture2D(source.format);
                    viewDesc.componentMapping = SwizzleMapping(w);
                    auto textureView = source.texture->createTextureView(viewDesc);
                    sets[0]->setTexture(nextView, source.texture.get(), RenderTextureLayout::SHADER_READ, textureView.get());
                    view = source.views.emplace(w, std::make_pair(nextView++, std::move(textureView))).first;
                }
                index = view != source.views.end() ? view->second.first : alias->second->descriptor;
                ++aliasHits;
                if (tracePSOption && tracePS.find("," + d.ps + ",") != std::string::npos)
                    fprintf(stderr, "ps %s draw %u slot %u base %08X swz %o swap %d bias %d exp %d fmt %d\n", d.ps.c_str(), i, slot, alias->first,
                        d.swizzles[slot], int(swap), bias, d.expAdjust[slot], int(source.format));
                if (tracePSOption && tracePS.find("," + d.ps + ",") != std::string::npos)
                    fprintf(stderr, "   %s\n", depthFetch ? "depth snapshot" :
                        snapshots.count(alias->first) && &snapshots[alias->first]->surface == alias->second ? "snapshot" : "live");
                if (bias || d.expAdjust[slot] || FixedSnorm(source.format))
                {
                    const float scale = std::ldexp(1.0f, bias + d.expAdjust[slot] + (FixedSnorm(source.format) ? 5 : 0));
                    if (CachedEnv("FD_REPLAY_TRACE"))
                        fprintf(stderr, "scale draw %u slot %u vs %s ps %s = %g\n", i, slot, d.vs.c_str(), d.ps.c_str(), scale);
                    memcpy(shared + kSharedFetchScale + slot * 4, &scale, 4);
                }
                if (!alias->second->shaderRead)
                {
                    list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(alias->second->texture.get(), RenderTextureLayout::SHADER_READ));
                    alias->second->shaderRead = true;
                }
            }
            memcpy(shared + kSharedTextureIndex + slot * 4, &index, 4);
            // FetchOpDimension::k3DOrStacked may use a plain 2D fetch constant.
            // Its third coordinate selects the only slice; shader_common.h
            // routes this tagged descriptor to the same final 2D view (which
            // may be a resolve alias or have a component swizzle).
            uint32_t volumeIndex = 0;
            if(d.slots[slot]>=0&&volumesByIndex[d.slots[slot]])
            {
                volumeIndex=uint32_t(d.slots[slot]);
                if(d.swizzles[slot]!=kIdentitySwizzle)
                {
                    const uint64_t key=uint64_t(volumeIndex)<<32|d.swizzles[slot];auto found=volumeSwizzles.find(key);
                    if(found==volumeSwizzles.end())
                    {
                        if(nextVolumeView>=descriptorBase+kMaxTargets)throw std::runtime_error("Native volume descriptor capacity exceeded");
                        auto desc=RenderTextureViewDesc::Texture3D(textureFormats[d.slots[slot]]);desc.componentMapping=SwizzleMapping(d.swizzles[slot]);
                        auto view=volumesByIndex[d.slots[slot]]->createTextureView(desc);const uint32_t descriptor=nextVolumeView++;
                        sets[1]->setTexture(descriptor,volumesByIndex[d.slots[slot]],RenderTextureLayout::SHADER_READ,view.get());volumeViews.push_back(std::move(view));
                        found=volumeSwizzles.emplace(key,descriptor).first;
                    }
                    volumeIndex=found->second;
                }
            }
            if (d.hasFetchMetadata && (d.textureFetches[slot][5] >> 9 & 3) == 1 &&
                !(d.textureFetches[slot][1] & 0x400))
                volumeIndex = 0x80000000u | index;
            memcpy(shared + kSharedTextureIndex + 128 + slot * 4, &volumeIndex, 4);
            if (traceDrawOption && i <= traceDrawThrough && slot < 4)
                fprintf(stderr, "draw %zu slot %u: table %d base %08X -> heap %u (dim %u stacked %u volume %u -> %08X, fetch %08X %08X %08X %08X %08X %08X)\n", i, slot, d.slots[slot], d.bases[slot], index,
                    d.textureFetches[slot][5] >> 9 & 3, (d.textureFetches[slot][1] & 0x400) != 0, d.slots[slot]>=0&&volumesByIndex[d.slots[slot]]?1u:0u, volumeIndex,
                    d.textureFetches[slot][0], d.textureFetches[slot][1], d.textureFetches[slot][2], d.textureFetches[slot][3], d.textureFetches[slot][4], d.textureFetches[slot][5]);
        }
        Target& target = targetFor(d);
        if(profile)
        {
            const auto now=std::chrono::steady_clock::now();
            profileDrawMs[2]+=std::chrono::duration<double,std::milli>(now-profileDrawStart).count();
            recordTimer.enabled=false;
            recordTimer.start=now;
        }
        RecordTimer bindTimer{profile,profileDrawMs[3],recordTimer.start};
        RenderPipeline* pipeline = pipelineFor(d);
        if (pipeline && d.primitive == 4)
        {
            for (uint32_t k = 0; k < target.count; ++k)
                if (target.rt[k]->shaderRead)
                {
                    list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target.rt[k]->texture.get(), RenderTextureLayout::COLOR_WRITE));
                    target.rt[k]->shaderRead = false;
                }
            if (target.ds->shaderRead)
            {
                list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target.ds->texture.get(), RenderTextureLayout::DEPTH_WRITE));
                target.ds->shaderRead = false;
            }
            if (current != &target)
            {
                list->setFramebuffer(target.framebuffer.get());
                list->setGraphicsPipelineLayout(layout.get());
                for (uint32_t set = 0; set < 5; ++set) list->setGraphicsDescriptorSet(sets[set].get(), set);
                current = &target;
            }
            // Hardware viewport transform (PA_CL_VPORT_*): NDC -> pixels.
            auto viewport=nativeViewport.host;
            if(target.scale!=1.0)
            {
                const float vs=float(target.scale);
                viewport.x*=vs;viewport.y*=vs;viewport.width*=vs;viewport.height*=vs;
            }
            list->setViewports(viewport);
            list->setScissors(RenderRect(0, 0, target.width, target.height));
            list->setPipeline(pipeline);
            // RB_BLENDCONTROL can select constant colour/alpha (simulation
            // smoothing uses 32/255). D3D12's default all-ones factor is wrong.
            const float blendFactor[4] = {
                d.hasBlendConstants ? AsFloat(d.regs[RB_BLEND_RED]) : 1.0f,
                d.hasBlendConstants ? AsFloat(d.regs[RB_BLEND_GREEN]) : 1.0f,
                d.hasBlendConstants ? AsFloat(d.regs[RB_BLEND_BLUE]) : 1.0f,
                d.hasBlendConstants ? AsFloat(d.regs[RB_BLEND_ALPHA]) : 1.0f};
            static_cast<D3D12CommandList*>(list.get())->d3d->OMSetBlendFactor(blendFactor);
            list->setGraphicsRootDescriptor(RenderBufferReference(constantBuffer.get(), kPerDraw * (i + 1)), 0);
            list->setGraphicsRootDescriptor(RenderBufferReference(constantBuffer.get(), kPerDraw * (i + 1) + 4096), 1);
            list->setGraphicsRootDescriptor(RenderBufferReference(constantBuffer.get(), kPerDraw * (i + 1) + 8192), 2);
            list->setGraphicsRootDescriptor(RenderBufferReference(streamBuffer.get(), 0), 3);
            RenderVertexBufferView vertexView(RenderBufferReference(vertexBuffer.get(), vertexAt), d.vertexBytes);
            RenderInputSlot slot(0, d.stride);
            list->setVertexBuffers(0, &vertexView, 1, &slot);
            RenderIndexBufferView indexView(RenderBufferReference(indexBuffer.get(), indexAt), d.indexBytes,
                d.index32 ? RenderFormat::R32_UINT : RenderFormat::R16_UINT);
            list->setIndexBuffer(&indexView);
            const uint32_t count = d.indexCount;
            if (traceDrawOption && i <= traceDrawThrough)
            {
                float v[8] = {};
                memcpy(v, d.vertices, (std::min)(sizeof(v), size_t(d.vertexBytes)));
                fprintf(stderr, "draw %zu vs %s ps %s: %u indices (%u bytes, idx32 %u) stride %u vbytes %u sem0 %s v0 (%g %g %g %g) v1 (%g %g %g %g) "
                    "viewport %g %g %g %g z %g..%g target %ux%u depthctl %08X offset %d blend %08X mask %08X colorinfo %08X\n", i, d.vs.c_str(), d.ps.c_str(), count, d.indexBytes,
                    d.index32, d.stride, d.vertexBytes, d.semantics.empty() ? "-" : d.semantics[0].c_str(), v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
                    viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth, target.width, target.height,
                    d.regs[RB_DEPTHCONTROL], int32_t(d.regs[VGT_INDX_OFFSET]), d.regs[RB_BLENDCONTROL0], d.regs[RB_COLOR_MASK], d.regs[RB_COLOR_INFO]);
                fprintf(stderr, "draw %zu context %d colorcontrol %08X alpharef %g\n", i, int(d.hasContext),
                    d.hasContext ? d.context[0x202] : 0u, d.hasContext ? AsFloat(d.context[0x10E]) : 0.0f);
            }
            list->drawIndexedInstanced(count, 1, 0, int32_t(d.regs[VGT_INDX_OFFSET]), 0);
            ++target.draws;
            target.lastWrite=++storage.writeSerial;
            if(i==gradingDraw&&target.rt[0]&&target.rt[0]->texture)
            {
                Surface& rt=*target.rt[0];
                resolveColor(rt);
                if(!storage.hudless||storage.hudlessWidth!=target.width||storage.hudlessHeight!=target.height||storage.hudlessFormat!=rt.format)
                {
                    storage.hudless=CreateTexture(device.get(),RenderTextureDesc::Texture2D(target.width,target.height,1,rt.format));
                    storage.hudlessWidth=target.width;storage.hudlessHeight=target.height;storage.hudlessFormat=rt.format;
                }
                list->barriers(RenderBarrierStage::COPY,{
                    RenderTextureBarrier(rt.texture.get(),RenderTextureLayout::COPY_SOURCE),
                    RenderTextureBarrier(storage.hudless.get(),RenderTextureLayout::COPY_DEST)});
                list->copyTextureRegion(RenderTextureCopyLocation::Subresource(storage.hudless.get()),
                    RenderTextureCopyLocation::Subresource(rt.texture.get()));
                list->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(rt.texture.get(),RenderTextureLayout::COLOR_WRITE));
                // Read by the present compute shader on the presenter's queue.
                list->barriers(RenderBarrierStage::COMPUTE,RenderTextureBarrier(storage.hudless.get(),RenderTextureLayout::SHADER_READ));
                rt.shaderRead=false;
                current=nullptr;
                hudlessCopied=true;
            }
        }
        vertexAt += d.vertexBytes;
        indexAt += (size_t(d.indexBytes) + 3) & ~size_t(3);
    }
    for (auto& [key, t] : targets)
        for (uint32_t k = 0; k < t.count; ++k)
        {
            Surface& rt = *t.rt[k];
            if(dir.empty()&&(!output||&rt!=presentationSurface))continue;
            if(!rt.readback)rt.readback=device->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(rt.rowPitch)*t.height));
            resolveColor(rt);
            list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(rt.texture.get(), RenderTextureLayout::COPY_SOURCE));
            list->copyTextureRegion(
                RenderTextureCopyLocation::PlacedFootprint(rt.readback.get(), rt.format, t.width, t.height, 1, rt.rowPitch / FormatBytes(rt.format)),
                RenderTextureCopyLocation::Subresource(rt.texture.get()));
        }
    Surface* nativeOutput=presentationSurface;
    uint32_t nativeWidth=presentationWidth,nativeHeight=presentationHeight;
    if(frame.frontBufferBase)
    {
        auto copy=snapshots.find(frame.frontBufferBase);
        if(copy==snapshots.end())throw std::runtime_error("Native swap has no rendered front buffer");
        nativeOutput=&copy->second->surface;nativeWidth=copy->second->width;nativeHeight=copy->second->height;
    }
    if(frame.continuous&&nativeOutput)
    {
        list->barriers(RenderBarrierStage::GRAPHICS,RenderTextureBarrier(nativeOutput->texture.get(),RenderTextureLayout::SHADER_READ));
        nativeOutput->shaderRead=true;
    }
    const auto profileRecorded=std::chrono::steady_clock::now();
    if (streamCopy.valid()) { streamCopy.get(); streamBuffer->unmap(); }
    list->end();
    vertexBuffer->unmap();
    indexBuffer->unmap();
    constantBuffer->unmap();
    packConstants->unmap();
    const RenderCommandList* lists = list.get();
    const auto profileSubmit=std::chrono::steady_clock::now();
    queue->executeCommandLists(&lists, 1, nullptr, 0, nullptr, 0, fence.get());
    {
        auto* submitted=static_cast<D3D12CommandFence*>(fence.get());
        frameFence=submitted->d3d;frameFenceValue=submitted->fenceValue-1;
    }
    // Live frames without readbacks: no CPU wait here (see InFlight).
    static const bool frameWait=CachedEnv("FD_NATIVE_FRAME_WAIT")!=nullptr;
    const bool deferWait=frame.continuous&&dir.empty()&&!output&&dumps.empty()&&!frameWait
#ifdef FD_REPLAY_RENDERDOC
        &&!renderdoc
#endif
        ;
    if(deferWait)
    {
        inFlight=std::make_unique<InFlight>();
        for(int i=0;i<5;++i)inFlight->sets[i]=std::move(sets[i]);
        inFlight->samplers=std::move(samplers);
        inFlight->textures=std::move(textures);inFlight->cubeTextures=std::move(cubeTextures);inFlight->volumeTextures=std::move(volumeTextures);
        inFlight->cubeViews=std::move(cubeViews);inFlight->volumeViews=std::move(volumeViews);inFlight->tableViews=std::move(tableViews);
        inFlight->staging=std::move(staging);inFlight->cubeStaging=std::move(cubeStaging);inFlight->volumeStaging=std::move(volumeStaging);
        inFlight->retiredPacked=std::move(retiredPacked);
        inFlight->retiredDepthSnapshots=std::move(retiredDepthSnapshots);inFlight->retiredSnapshots=std::move(retiredSnapshots);
        inFlight->pixelShader=std::move(pixelShader);
    }
    else queue->waitForCommandFence(fence.get());
    const auto profileFinished=std::chrono::steady_clock::now();
    if(profile)
    {
        const std::chrono::steady_clock::time_point times[]={profileStart,profileResources,profileSetup,profileRecorded,profileSubmit,profileFinished};
        for(size_t i=0;i<5;++i)profileMs[i]+=std::chrono::duration<double,std::milli>(times[i+1]-times[i]).count();
        const std::chrono::steady_clock::time_point resourceTimes[]={profileStart,profileSets,profileUploads,profileTextures,profileCubes,profileResources};
        for(size_t i=0;i<5;++i)profileResMs[i]+=std::chrono::duration<double,std::milli>(resourceTimes[i+1]-resourceTimes[i]).count();
        if(++profileFrames==60)
        {
            fprintf(stderr,"native resource profile: sets %.2f ms, buffers %.2f ms, textures %.2f ms, cubes %.2f ms, volumes %.2f ms\n",profileResMs[0]/60,profileResMs[1]/60,profileResMs[2]/60,profileResMs[3]/60,profileResMs[4]/60);
            std::fill(std::begin(profileResMs),std::end(profileResMs),0.0);
            fprintf(stderr,"native render profile: resources %.2f ms, surfaces %.2f ms, record %.2f ms, close %.2f ms, submit/wait %.2f ms\n",profileMs[0]/60,profileMs[1]/60,profileMs[2]/60,profileMs[3]/60,profileMs[4]/60);
            fprintf(stderr,"native draw profile: clear %.2f ms, resolve %.2f ms, prepare %.2f ms (vertex/index copies %.2f, constant copies %.2f), bind/draw %.2f ms\n",
                profileDrawMs[0]/60,profileDrawMs[1]/60,profileDrawMs[2]/60,profileCopyMs[0]/60,profileCopyMs[1]/60,profileDrawMs[3]/60);
            std::fill(std::begin(profileCopyMs),std::end(profileCopyMs),0.0);
            std::fill(std::begin(profileMs),std::end(profileMs),0.0);profileFrames=0;
            std::fill(std::begin(profileDrawMs),std::end(profileDrawMs),0.0);
        }
    }
    PrintDebugMessages(infoQueue, debugErrors, debugWarnings, "execute");

#ifdef FD_REPLAY_RENDERDOC
    if (renderdoc) renderdoc->EndFrameCapture(nullptr, nullptr);
#endif

    if (debugErrors || failedPipelines)
    {
        fprintf(stderr, "fd_replay: rejected execution with %llu D3D12 errors and %u failed pipelines\n",
            static_cast<unsigned long long>(debugErrors), failedPipelines);
        return 1;
    }

    int n = 0;
    for (auto& [key, t] : targets)
    {
        for (uint32_t k = 0; k < t.count; ++k)
        {
            Surface& rt = *t.rt[k];
            if (dir.empty() && (!output || &rt != presentationSurface)) continue;
            const std::string path = dir + "/target_" + std::to_string(n) + (k ? "_rt" + std::to_string(k) : "") + ".png";
            const auto* pixels = static_cast<const uint8_t*>(rt.readback->map());
            std::vector<uint8_t> rgba(size_t(t.width) * t.height * 4);
            for (uint32_t y = 0; y < t.height; ++y)
                for (uint32_t x = 0; x < t.width; ++x)
                    ToRgba8(rt.format, pixels + size_t(y) * rt.rowPitch + size_t(x) * FormatBytes(rt.format), &rgba[(size_t(y) * t.width + x) * 4]);
            if (&rt == presentationSurface)
            {
                // The game displays RGB; its final target's alpha carries
                // post-processing data, not window opacity. A transparent PNG
                // otherwise looks much darker on dark viewers/backgrounds.
                std::vector<uint8_t> rgb(size_t(presentationWidth) * presentationHeight * 3);
                for (uint32_t y = 0; y < t.height; ++y)
                    for (uint32_t x = 0; x < t.width; ++x)
                    {
                        const size_t pixel = (size_t(y) * t.width + x) * 4;
                        if (y < presentationHeight && x < presentationWidth)
                            memcpy(rgb.data() + (size_t(y) * presentationWidth + x) * 3, rgba.data() + pixel, 3);
                        rgba[pixel + 3] = 255;
                    }
                if (!dir.empty()) stbi_write_png((dir + "/frame.png").c_str(), presentationWidth, presentationHeight, 3, rgb.data(), presentationWidth * 3);
                if (output)
                {
                    output->width = presentationWidth;
                    output->height = presentationHeight;
                    output->rgb = std::move(rgb);
                }
            }
            if (!dir.empty()) stbi_write_png(path.c_str(), t.width, t.height, 4, rgba.data(), t.width * 4);
            rt.readback->unmap();
        }
        if(!frame.continuous||CachedEnv("FD_REPLAY_TRACE"))printf("target_%d: %ux%u, %u RT, %u draws (surface %08X color %08X depth %08X)\n", n, t.width, t.height, t.count, t.draws,
            std::get<0>(key), std::get<1>(key), std::get<5>(key));
        ++n;
    }
    for (Dump& dump : dumps)
    {
        if (dir.empty()) continue;
        char name[64];
        snprintf(name, sizeof(name), "/%s_%02u_%08X.png", dump.depth ? "depth" : "resolve", dump.order, dump.dest);
        const auto* pixels = static_cast<const uint8_t*>(dump.buffer->map());
        std::vector<uint8_t> rgba(size_t(dump.width) * dump.height * 4);
        for (uint32_t y = 0; y < dump.height; ++y)
            for (uint32_t x = 0; x < dump.width; ++x)
                ToRgba8(dump.format, pixels + size_t(y) * dump.rowPitch + size_t(x) * FormatBytes(dump.format), &rgba[(size_t(y) * dump.width + x) * 4]);
        stbi_write_png((dir + name).c_str(), dump.width, dump.height, 4, rgba.data(), dump.width * 4);
        // Raw texels too (.raw: u32 width, height, plume format, bytes per texel, rows).
        snprintf(name, sizeof(name), "/%s_%02u_%08X.raw", dump.depth ? "depth" : "resolve", dump.order, dump.dest);
        if (FILE* f = fopen((dir + name).c_str(), "wb"))
        {
            const uint32_t head[4] = {dump.width, dump.height, uint32_t(dump.format), FormatBytes(dump.format)};
            fwrite(head, sizeof(head), 1, f);
            for (uint32_t y = 0; y < dump.height; ++y) fwrite(pixels + size_t(y) * dump.rowPitch, FormatBytes(dump.format), dump.width, f);
            fclose(f);
        }
        dump.buffer->unmap();
    }
    if(!frame.continuous||CachedEnv("FD_REPLAY_TRACE"))printf("fd_replay: %zu draws, %zu pipelines, %zu targets, %zu resolves mapped, %u resolved reads\n", draws.size(),
        pipelines.size(), targets.size(), resolved.size(), aliasHits);
    PrintDebugMessages(infoQueue, debugErrors, debugWarnings, "readback");
    if (debugErrors) return 1;
    uint64_t frameDraws = 0;
    for (const auto& [key, target] : targets) frameDraws += target.draws;
    if (output)
    {
        output->gameDraws = frameDraws;
        output->debugErrors = debugErrors;
        output->debugWarnings = debugWarnings;
    }
    if (!dir.empty())
    {
        FILE* report = fopen((dir + "/native-report.json").c_str(), "wb");
        if (!report) return 1;
        fprintf(report, "{\n  \"full_frame\": %s,\n  \"game_draws\": %llu,\n  \"records\": %zu,\n"
            "  \"debug_requested\": %s,\n  \"debug_active\": %s,\n  \"debug_errors\": %llu,\n  \"debug_warnings\": %llu,\n"
            "  \"shader_catalog\": %s,\n  \"catalog_shaders\": %zu\n}\n",
            stopAfter >= draws.size() - 1 ? "true" : "false", static_cast<unsigned long long>(frameDraws), draws.size(),
            getenv("FD_REPLAY_DEBUG") ? "true" : "false", infoQueue ? "true" : "false",
            static_cast<unsigned long long>(debugErrors), static_cast<unsigned long long>(debugWarnings),
            shaderCatalog ? "true" : "false", shaderCatalog ? shaderCatalog->Shaders().size() : 0);
        fclose(report);
    }
    completedDraws += frameDraws;
    ++completedFrames;
    // FD_TRACE_CAMERA: how often the scene camera (VS_DustConst[8..11]) changes.
    if(frame.continuous&&sceneCamera&&CachedEnv("FD_TRACE_CAMERA"))
    {
        static float lastVP[16]{};
        static uint64_t frames=0,changes=0;
        static auto since=std::chrono::steady_clock::now();
        ++frames;
        if(memcmp(lastVP,sceneVP,sizeof(lastVP))){++changes;memcpy(lastVP,sceneVP,sizeof(lastVP));}
        const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-since).count();
        if(seconds>=5){fprintf(stderr,"camera trace: %.1f frames/s, camera changed %.1f/s\n",frames/seconds,changes/seconds);frames=changes=0;since=std::chrono::steady_clock::now();}
    }
    if(sceneResolve!=SIZE_MAX)++upscale.frameIndex;
    if(frame.continuous)
    {
        UpscalerStatus status;
        status.available=upscale.ngx.Tried()?(upscale.ngx.Available()?1:0):-1;
        status.active=upscaleMode;status.evaluatedFrames=upscale.evaluated;
        status.sceneFound=sceneResolve!=SIZE_MAX;status.cameraFound=sceneCamera;
        status.renderWidth=scaledSize(fbW,renderScale);status.renderHeight=scaledSize(fbH,renderScale);
        status.outputWidth=scaledSize(fbW,outputScale);status.outputHeight=scaledSize(fbH,outputScale);
        PublishUpscalerStatus(status);
    }
    if(frame.continuous&&nativeOutput){outputTexture=static_cast<D3D12Texture*>(nativeOutput->texture.get());outputWidth=nativeWidth;outputHeight=nativeHeight;}
    if(frame.continuous)outputHudless=hudlessCopied&&storage.hudless?static_cast<D3D12Texture*>(storage.hudless.get()):nullptr;
    return 0;
}

gpu::native::Renderer::Renderer() : impl(std::make_unique<Impl>()) {}
gpu::native::Renderer::~Renderer() = default;
bool gpu::native::Renderer::UseDevice(ID3D12Device* device)
{
    if(!device||impl->device)return false;
    impl->externalDevice=device;return true;
}
uint32_t gpu::native::Renderer::OutputWidth()const{return impl->outputWidth;}
uint32_t gpu::native::Renderer::OutputHeight()const{return impl->outputHeight;}
bool gpu::native::Renderer::PresentTo(ID3D12Resource* destination,ID3D12CommandQueue* queue)
{
    return impl->PresentTo(destination,queue);
}
bool gpu::native::Renderer::Impl::PresentTo(ID3D12Resource* destination,ID3D12CommandQueue* destinationQueue)
{
    if(!outputTexture||!destination||!destinationQueue)return false;
    auto* d3d=static_cast<D3D12Device*>(device.get())->d3d;
    Microsoft::WRL::ComPtr<ID3D12Device> destinationDevice;
    if(FAILED(destination->GetDevice(IID_PPV_ARGS(&destinationDevice)))||destinationDevice.Get()!=static_cast<ID3D12Device*>(d3d))return false;
    if(!presentPipeline)
    {
        D3D12_DESCRIPTOR_RANGE ranges[2]={};
        ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[0].NumDescriptors=1;
        ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;ranges[1].NumDescriptors=1;
        ranges[1].OffsetInDescriptorsFromTableStart=1;
        D3D12_ROOT_PARAMETER parameters[2]={};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable.NumDescriptorRanges=2;parameters[0].DescriptorTable.pDescriptorRanges=ranges;
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants.Num32BitValues=4;
        D3D12_ROOT_SIGNATURE_DESC root={};root.NumParameters=2;root.pParameters=parameters;
        Microsoft::WRL::ComPtr<ID3DBlob> blob,errors;
        if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors))||
            FAILED(d3d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&presentRoot))))return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline={};pipeline.pRootSignature=presentRoot.Get();pipeline.CS={fd_native_present,sizeof(fd_native_present)};
        D3D12_DESCRIPTOR_HEAP_DESC heap={};heap.NumDescriptors=3*kPresentSlots;heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if(FAILED(d3d->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&presentPipeline)))||
            FAILED(d3d->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&presentHeap)))||
            FAILED(d3d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&presentFence))))return false;
        for(uint32_t slot=0;slot<kPresentSlots;++slot)
        {
            if(FAILED(d3d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&presentAllocators[slot])))||
                FAILED(d3d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,presentAllocators[slot].Get(),nullptr,IID_PPV_ARGS(&presentLists[slot])))||
                FAILED(presentLists[slot]->Close()))return false;
        }
    }
    // HDR (FP16 scRGB guest output): present_hdr.hlsl, with the scene before
    // the HUD as its second source.
    const bool hdr=HdrActive()&&destination->GetDesc().Format==DXGI_FORMAT_R16G16B16A16_FLOAT;
    if(hdr&&!presentHdrPipeline)
    {
        D3D12_DESCRIPTOR_RANGE ranges[2]={};
        ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[0].NumDescriptors=2;
        ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;ranges[1].NumDescriptors=1;
        ranges[1].OffsetInDescriptorsFromTableStart=2;
        D3D12_ROOT_PARAMETER parameters[2]={};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable.NumDescriptorRanges=2;parameters[0].DescriptorTable.pDescriptorRanges=ranges;
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants.Num32BitValues=8;
        D3D12_ROOT_SIGNATURE_DESC root={};root.NumParameters=2;root.pParameters=parameters;
        Microsoft::WRL::ComPtr<ID3DBlob> blob,errors;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline={};pipeline.CS={fd_native_present_hdr,sizeof(fd_native_present_hdr)};
        if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors))||
            FAILED(d3d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&presentHdrRoot))))return false;
        pipeline.pRootSignature=presentHdrRoot.Get();
        if(FAILED(d3d->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&presentHdrPipeline))))return false;
    }
    const auto targetDesc=destination->GetDesc();
    const uint32_t slot=uint32_t(presentSerial%kPresentSlots);
    auto& presentAllocator=presentAllocators[slot];auto& presentList=presentLists[slot];
    // This slot's previous copy (kPresentSlots frames ago) must be done before
    // its allocator and descriptors are reused.
    if(presentSlotSerials[slot]&&presentFence->GetCompletedValue()<presentSlotSerials[slot])
    {
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!event)return false;
        const bool done=SUCCEEDED(presentFence->SetEventOnCompletion(presentSlotSerials[slot],event))&&WaitForSingleObject(event,10000)==WAIT_OBJECT_0;
        CloseHandle(event);
        if(!done)return false;
    }
    const uint32_t increment=d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto srv=presentHeap->GetCPUDescriptorHandleForHeapStart();srv.ptr+=size_t(increment)*3*slot;
    auto uav=srv;uav.ptr+=increment*(hdr?2:1);
    auto table=presentHeap->GetGPUDescriptorHandleForHeapStart();table.ptr+=uint64_t(increment)*3*slot;
    D3D12_SHADER_RESOURCE_VIEW_DESC source={};source.Format=outputTexture->d3d->GetDesc().Format;
    source.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;source.Texture2D.MipLevels=1;source.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d3d->CreateShaderResourceView(outputTexture->d3d,&source,srv);
    // The HDR shader's second source: the scene before the HUD, or the output
    // itself when this frame has none (menus, videos: no expansion).
    D3D12Texture* hudless=hdr&&outputHudless?outputHudless:nullptr;
    if(hdr)
    {
        auto second=srv;second.ptr+=increment;
        D3D12_SHADER_RESOURCE_VIEW_DESC hudlessView=source;
        ID3D12Resource* hudlessResource=hudless?hudless->d3d:outputTexture->d3d;
        hudlessView.Format=hudlessResource->GetDesc().Format;
        d3d->CreateShaderResourceView(hudlessResource,&hudlessView,second);
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC target={};target.Format=targetDesc.Format;target.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    d3d->CreateUnorderedAccessView(destination,nullptr,&target,uav);
    if(FAILED(presentAllocator->Reset())||FAILED(presentList->Reset(presentAllocator.Get(),presentPipeline.Get())))return false;
    D3D12_RESOURCE_BARRIER barriers[2]={};
    for(auto& barrier:barriers){barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;}
    barriers[0].Transition.pResource=outputTexture->d3d;barriers[0].Transition.StateBefore=outputTexture->resourceStates;barriers[0].Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriers[1].Transition.pResource=destination;barriers[1].Transition.StateBefore=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;barriers[1].Transition.StateAfter=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    presentList->ResourceBarrier(2,barriers);
    ID3D12DescriptorHeap* heaps[]={presentHeap.Get()};presentList->SetDescriptorHeaps(1,heaps);
    if(hdr)
    {
        float paper=200,peak=800;
        HdrLevels(paper,peak);
        struct { uint32_t sourceSize[2],destinationSize[2]; float paperWhite,peakRatio,knee; uint32_t flags; } params{
            {outputWidth,outputHeight},{uint32_t(targetDesc.Width),targetDesc.Height},paper/80.0f,peak/paper,
            CachedEnv("FD_HDR_KNEE")?float(atof(CachedEnv("FD_HDR_KNEE"))):0.35f,(hudless?1u:0u)|(CachedEnv("FD_HDR_SHOW")?2u:0u)};
        presentList->SetPipelineState(presentHdrPipeline.Get());
        presentList->SetComputeRootSignature(presentHdrRoot.Get());presentList->SetComputeRootDescriptorTable(0,table);
        presentList->SetComputeRoot32BitConstants(1,8,&params,0);
    }
    else
    {
        presentList->SetComputeRootSignature(presentRoot.Get());presentList->SetComputeRootDescriptorTable(0,table);
        const uint32_t extents[]={outputWidth,outputHeight,uint32_t(targetDesc.Width),targetDesc.Height};
        presentList->SetComputeRoot32BitConstants(1,4,extents,0);
    }
    presentList->Dispatch((uint32_t(targetDesc.Width)+7)/8,(targetDesc.Height+7)/8,1);
    for(auto& barrier:barriers)std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
    presentList->ResourceBarrier(2,barriers);
    if(FAILED(presentList->Close()))return false;
    // The frame may still be rendering (no CPU wait at its end): the copy
    // waits for it on the GPU.
    if(frameFence&&FAILED(destinationQueue->Wait(frameFence,frameFenceValue)))return false;
    ID3D12CommandList* commands[]={presentList.Get()};destinationQueue->ExecuteCommandLists(1,commands);
    if(FAILED(destinationQueue->Signal(presentFence.Get(),++presentSerial)))return false;
    presentSlotSerials[slot]=presentSerial;
    // No CPU wait: the renderer's queue waits on the GPU for this copy before
    // it rewrites the output texture (the destination queue then presents in
    // order). FD_NATIVE_PRESENT_WAIT=1 restores the CPU wait.
    bool waited=true;
    if(CachedEnv("FD_NATIVE_PRESENT_WAIT"))
    {
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!event)return false;
        waited=SUCCEEDED(presentFence->SetEventOnCompletion(presentSerial,event))&&WaitForSingleObject(event,10000)==WAIT_OBJECT_0;
        CloseHandle(event);
    }
    else if(FAILED(static_cast<D3D12CommandQueue*>(queue.get())->d3d->Wait(presentFence.Get(),presentSerial)))return false;
    PrintDebugMessages(infoQueue,debugErrors,debugWarnings,"native present");
    return waited&&!debugErrors;
}
bool gpu::native::Renderer::PrepareDeviceCreation(){return EnableDebugLayer();}
bool gpu::native::Renderer::LoadShaderCatalog(const std::string& path)
{
    if (impl->device || impl->completedFrames) return false;
    try
    {
        impl->shaderCatalog = std::make_unique<ShaderCatalog>(ShaderCatalog::Load(path));
        return true;
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "native renderer: %s\n", error.what());
        return false;
    }
}
int gpu::native::Renderer::RenderReplay(const std::string& directory)
{
    try
    {
        auto frame = ReplayFrame::Load(directory);
        return RenderFrame(frame.View(), directory);
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "native renderer: %s (%s)\n", error.what(), directory.c_str());
        return 1;
    }
}
int gpu::native::Renderer::RenderFrame(const FrameInput& frame, const std::string& directory, FrameOutput* output)
{
    if (output) *output = {};
    try
    {
        const int result = impl->RenderFrame(frame, directory, output);
        if (result && output) *output = {};
        return result;
    }
    catch (const std::exception& error)
    {
        PrintDebugMessages(impl->infoQueue, impl->debugErrors, impl->debugWarnings, "failure");
        fprintf(stderr, "native renderer: %s (%s)\n", error.what(), directory.c_str());
        if (output) *output = {};
        return 1;
    }
}
uint64_t gpu::native::Renderer::CompletedFrames() const { return impl->completedFrames; }
uint64_t gpu::native::Renderer::CompletedDraws() const { return impl->completedDraws; }
