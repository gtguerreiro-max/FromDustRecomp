// fd_replay: redraws a frame captured by runtime/gpu/frame_capture.cpp with the
// native renderer's building blocks (plume + XenosRecomp shaders), offscreen.
// tools/replay_pack.py prepares <dir>/replay.bin and the shaders; every render
// target configuration of the frame is written as <dir>/target_<n>.png.
//
// Usage: fd_replay <replay dir>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "plume_render_interface.h"
#include "plume_d3d12.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace plume
{
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
}

// D3D12 Agility SDK (third_party/agility, DLLs in D3D12\ next to the exe): gives
// the debug layer used by FD_REPLAY_DEBUG without installing Graphics Tools.
extern "C" {
__declspec(dllexport) extern const unsigned int D3D12SDKVersion = 614;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

using namespace plume;

namespace
{
// FD_REPLAY_DEBUG=1: D3D12 debug layer, messages printed after pipeline creation.
ID3D12InfoQueue* g_infoQueue = nullptr;

void EnableDebugLayer()
{
    if (!getenv("FD_REPLAY_DEBUG")) return;
    ID3D12Debug* debug = nullptr;
    const HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    if (SUCCEEDED(hr)) debug->EnableDebugLayer();
}

void AttachInfoQueue(RenderDevice* device)
{
    if (!getenv("FD_REPLAY_DEBUG")) return;
    auto* d3d = static_cast<D3D12Device*>(device)->d3d;
    d3d->QueryInterface(IID_PPV_ARGS(&g_infoQueue));
    if (g_infoQueue)
    {
        g_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        g_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
    }
}

void PrintDebugMessages(const char* context)
{
    if (!g_infoQueue) return;
    for (UINT64 i = 0, n = g_infoQueue->GetNumStoredMessages(); i < n; ++i)
    {
        SIZE_T size = 0;
        g_infoQueue->GetMessage(i, nullptr, &size);
        std::vector<char> buffer(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        if (SUCCEEDED(g_infoQueue->GetMessage(i, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            fprintf(stderr, "[d3d12 %s] %s\n", context, message->pDescription);
    }
    g_infoQueue->ClearStoredMessages();
}

// Order of the per-draw registers written by replay_pack.py (STATE_REGISTERS).
enum Reg
{
    RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO, PA_SC_WINDOW_SCISSOR_TL, PA_SC_WINDOW_SCISSOR_BR,
    RB_COLOR_MASK, RB_DEPTHCONTROL, RB_BLENDCONTROL0, PA_SU_SC_MODE_CNTL, VGT_INDX_OFFSET,
    VPORT_XSCALE, VPORT_XOFFSET, VPORT_YSCALE, VPORT_YOFFSET, VPORT_ZSCALE, VPORT_ZOFFSET, PA_CL_VTE_CNTL,
    RB_COLOR1_INFO, RB_COLOR2_INFO, RB_COLOR3_INFO, RB_BLENDCONTROL1, RB_BLENDCONTROL2, RB_BLENDCONTROL3,
    kRegCount
};

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

float AsFloat(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

struct Draw
{
    std::string vs;
    uint32_t primitive = 0, indexCount = 0, index32 = 0, stride = 0;
    const uint8_t* indices = nullptr;
    uint32_t indexBytes = 0;
    const uint8_t* vertices = nullptr;
    uint32_t vertexBytes = 0;
    std::vector<std::string> semantics;
    const uint8_t* constants = nullptr; // 4096 bytes
    uint32_t regs[kRegCount] = {};
    int32_t slots[32] = {}; // texture table index per fetch slot, -1 if none
    std::string ps;                       // empty: depth only
    const uint8_t* psConstants = nullptr; // 4096 bytes
    uint32_t bases[32] = {};              // texture base address per fetch slot
    uint32_t swizzles[32] = {};           // fetch swizzle per slot
    uint32_t boolLoop[40] = {};           // registers 0x4900-0x4927
    const uint8_t* vertexFetch = nullptr; // 32 uint4 g_VertexFetch descriptors (pulled vertex fetches)
    // "#resolve" records: EDRAM -> texture copy.
    bool resolve = false;
    uint32_t copyDest = 0, copyControl = 0, copySurface = 0, copyColor = 0, copyDepth = 0, copyDestInfo = 0;
};

// Shared constants layout of the patched XenosRecomp (shader_common.h).
constexpr uint32_t kSharedTextureIndex = 0;   // + dimension * 128 + slot * 4
constexpr uint32_t kSharedSamplerIndex = 384; // + slot * 4
constexpr uint32_t kSharedVertexFetch = 1024; // 32 uint4
constexpr uint32_t kSharedSize = 2048;

std::vector<Draw> ParseReplay(const std::vector<uint8_t>& file)
{
    std::vector<Draw> draws;
    size_t at = 0;
    auto u32 = [&] { uint32_t v; memcpy(&v, file.data() + at, 4); at += 4; return v; };
    auto bytes = [&](uint32_t n) { const uint8_t* p = file.data() + at; at += n; return p; };
    if (file.size() < 8 || u32() != 0x50524446) return draws;
    for (uint32_t count = u32(); count; --count)
    {
        Draw d;
        const uint32_t nameLength = u32();
        d.vs.assign(reinterpret_cast<const char*>(bytes(nameLength)), nameLength);
        if (d.vs == "#resolve")
        {
            d.resolve = true;
            d.copyDest = u32();
            d.copyControl = u32();
            d.copySurface = u32();
            d.copyColor = u32();
            d.copyDepth = u32();
            d.copyDestInfo = u32();
            draws.push_back(d);
            continue;
        }
        d.primitive = u32();
        d.indexCount = u32();
        d.indexBytes = u32();
        d.indices = bytes(d.indexBytes);
        d.stride = u32();
        d.vertexBytes = u32();
        d.vertices = bytes(d.vertexBytes);
        for (uint32_t e = u32(); e; --e)
        {
            const uint32_t length = u32();
            d.semantics.emplace_back(reinterpret_cast<const char*>(bytes(length)), length);
        }
        d.constants = bytes(4096);
        d.index32 = u32();
        for (uint32_t& r : d.regs) r = u32();
        for (int32_t& slot : d.slots) slot = int32_t(u32());
        const uint32_t psLength = u32();
        d.ps.assign(reinterpret_cast<const char*>(bytes(psLength)), psLength);
        d.psConstants = bytes(4096);
        for (uint32_t& b : d.bases) b = u32();
        for (uint32_t& w : d.swizzles) w = u32();
        for (uint32_t& r : d.boolLoop) r = u32();
        d.vertexFetch = bytes(512);
        draws.push_back(d);
    }
    return draws;
}

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
// 16-bit and float formats keep their precision (k_16_16 as half float: the
// Xenos fixed-point range is wider than unorm).
RenderFormat ColorFormat(uint32_t colorInfo)
{
    switch ((colorInfo >> 16) & 0xF)
    {
    case 2: case 10: case 3: case 12: case 5: case 7: return RenderFormat::R16G16B16A16_FLOAT; // no 10-bit format in plume
    case 4: case 6: return RenderFormat::R16G16_FLOAT;
    case 14: return RenderFormat::R32_FLOAT;
    case 15: return RenderFormat::R32G32_FLOAT;
    default: return RenderFormat::R8G8B8A8_UNORM;
    }
}

uint32_t FormatBytes(RenderFormat f)
{
    switch (f)
    {
    case RenderFormat::R16G16B16A16_FLOAT: case RenderFormat::R32G32_FLOAT: return 8;
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
    std::unique_ptr<RenderBuffer> readback;
    uint32_t rowPitch = 0;
    uint32_t descriptor = 0;      // index in the Texture2D heap (resolved reads)
    std::map<uint32_t, std::pair<uint32_t, std::unique_ptr<RenderTextureView>>> views; // swizzle -> heap index, view
    bool shaderRead = false;      // texture currently in SHADER_READ
    bool isDepth = false;         // depth surface: written as D32_FLOAT, read as R32_FLOAT
};

// A render target configuration of the frame (EDRAM color/depth bases + pitch).
struct Target
{
    uint32_t width = 0, height = 0, count = 1, draws = 0;
    Surface rt[4];
    Surface ds;                                // depth: R32_TYPELESS texture
    std::unique_ptr<RenderTextureView> dsv;    // D32_FLOAT view for the framebuffer
    std::unique_ptr<RenderFramebuffer> framebuffer;
};

using TargetKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: fd_replay <replay dir>\n");
        return 1;
    }
    const std::string dir = argv[1];
    const std::vector<uint8_t> file = ReadFile(dir + "/replay.bin");
    const std::vector<Draw> draws = ParseReplay(file);

    EnableDebugLayer();
    auto api = CreateD3D12Interface();
    auto device = api ? api->createDevice() : nullptr;
    if (device) AttachInfoQueue(device.get());
    if (!device)
    {
        fprintf(stderr, "fd_replay: D3D12 initialization failed\n");
        return 1;
    }
    auto queue = device->createCommandQueue(RenderCommandListType::DIRECT);
    auto list = queue->createCommandList();
    auto fence = device->createCommandFence();

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
    const std::vector<uint8_t> textureFile = ReadFile(dir + "/textures.rgba");
    uint32_t textureCount = 0;
    if (textureFile.size() >= 4) memcpy(&textureCount, textureFile.data(), 4);
    // RenderDescriptorRange(type, binding, count).
    constexpr uint32_t kMaxTargets = 2048; // heap slots after the table for target and swizzled views
    const RenderDescriptorRange textureRange(RenderDescriptorRangeType::TEXTURE, 0, textureCount + kMaxTargets);
    const RenderDescriptorRange emptyRange(RenderDescriptorRangeType::TEXTURE, 0, 1);
    const RenderDescriptorRange samplerRange(RenderDescriptorRangeType::SAMPLER, 0, 1);
    const RenderDescriptorSetDesc setDescs[] = {
        RenderDescriptorSetDesc(&textureRange, 1, true, textureCount + kMaxTargets),
        RenderDescriptorSetDesc(&emptyRange, 1, true, 1),
        RenderDescriptorSetDesc(&emptyRange, 1, true, 1),
        RenderDescriptorSetDesc(&samplerRange, 1, true, 1),
    };
    RenderPipelineLayoutDesc layoutDesc;
    layoutDesc.rootDescriptorDescs = roots;
    layoutDesc.rootDescriptorDescsCount = 4;
    layoutDesc.descriptorSetDescs = setDescs;
    layoutDesc.descriptorSetDescsCount = 4;
    layoutDesc.allowInputLayout = true;
    auto layout = device->createPipelineLayout(layoutDesc);
    std::unique_ptr<RenderDescriptorSet> sets[4];
    for (int i = 0; i < 4; ++i) sets[i] = device->createDescriptorSet(setDescs[i]);
    auto sampler = device->createSampler(RenderSamplerDesc());
    sets[3]->setSampler(0, sampler.get());
    const std::vector<uint8_t> psCode = ReadFile(dir + "/debug.ps.dxil");
    auto pixelShader = device->createShader(psCode.data(), psCode.size(), "main", RenderShaderFormat::DXIL);

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
    for (const Draw& d : draws) vertexTotal += d.vertexBytes, indexTotal += d.indexBytes;
    auto vertexBuffer = device->createBuffer(RenderBufferDesc::VertexBuffer(vertexTotal, RenderHeapType::UPLOAD));
    auto indexBuffer = device->createBuffer(RenderBufferDesc::IndexBuffer(indexTotal, RenderHeapType::UPLOAD));
    // Per draw: 4096 bytes vertex constants, 4096 pixel constants, kSharedSize shared.
    constexpr uint32_t kPerDraw = 4096 + 4096 + kSharedSize;
    auto constantBuffer = device->createBuffer(RenderBufferDesc::UploadBuffer(kPerDraw * (draws.size() + 1), RenderBufferFlag::CONSTANT));
    // Guest vertex streams read by pulled vertex fetches.
    const std::vector<uint8_t> streamFile = ReadFile(dir + "/streams.bin");
    auto streamBuffer = device->createBuffer(RenderBufferDesc::UploadBuffer((std::max)(streamFile.size(), size_t(16))));
    if (!streamFile.empty())
    {
        memcpy(streamBuffer->map(), streamFile.data(), streamFile.size());
        streamBuffer->unmap();
    }
    auto* vertexMap = static_cast<uint8_t*>(vertexBuffer->map());
    auto* indexMap = static_cast<uint8_t*>(indexBuffer->map());
    auto* constantMap = static_cast<uint8_t*>(constantBuffer->map());
    memset(constantMap, 0, kPerDraw); // pixel constants: zero for now

    // Textures: RGBA8, uploaded through one staging buffer.
    std::vector<std::unique_ptr<RenderTexture>> textures;
    std::unique_ptr<RenderBuffer> staging;
    {
        size_t at = 4, stagingSize = 0;
        std::vector<std::tuple<uint32_t, uint32_t, size_t, size_t>> layoutInfo; // w, h, file offset, staging offset
        for (uint32_t i = 0; i < textureCount; ++i)
        {
            uint32_t w, h;
            memcpy(&w, textureFile.data() + at, 4);
            memcpy(&h, textureFile.data() + at + 4, 4);
            layoutInfo.emplace_back(w, h, at + 8, stagingSize);
            stagingSize += size_t((w * 4 + 255) & ~255u) * h;
            stagingSize = (stagingSize + 511) & ~size_t(511);
            at += 8 + size_t(w) * h * 4;
        }
        staging = device->createBuffer(RenderBufferDesc::UploadBuffer((std::max)(stagingSize, size_t(16))));
        auto* map = static_cast<uint8_t*>(staging->map());
        for (const auto& [w, h, from, to] : layoutInfo)
        {
            const uint32_t pitch = (w * 4 + 255) & ~255u;
            for (uint32_t y = 0; y < h; ++y) memcpy(map + to + size_t(y) * pitch, textureFile.data() + from + size_t(y) * w * 4, w * 4);
        }
        staging->unmap();
        list->begin();
        for (uint32_t i = 0; i < textureCount; ++i)
        {
            const auto& [w, h, from, to] = layoutInfo[i];
            textures.push_back(device->createTexture(RenderTextureDesc::Texture2D(w, h, 1, RenderFormat::R8G8B8A8_UNORM)));
            list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(textures.back().get(), RenderTextureLayout::COPY_DEST));
            list->copyTextureRegion(RenderTextureCopyLocation::Subresource(textures.back().get()),
                RenderTextureCopyLocation::PlacedFootprint(staging.get(), RenderFormat::R8G8B8A8_UNORM, w, h, 1, ((w * 4 + 255) & ~255u) / 4, to));
            list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(textures.back().get(), RenderTextureLayout::SHADER_READ));
            sets[0]->setTexture(i, textures.back().get(), RenderTextureLayout::SHADER_READ);
        }
        list->end();
        const RenderCommandList* uploads = list.get();
        queue->executeCommandLists(&uploads, 1, nullptr, 0, nullptr, 0, fence.get());
        queue->waitForCommandFence(fence.get());
    }

    // Targets keyed by (surface info, color info, depth info), sized to the
    // largest viewport any draw of the frame uses with them.
    std::map<TargetKey, Target> targets;
    uint32_t nextPlain = 0; // plain (unswizzled) heap slots for target surfaces
    std::map<TargetKey, std::pair<uint32_t, uint32_t>> targetSizes;
    std::map<TargetKey, uint32_t> targetCounts;
    auto keyOf = [](const Draw& d) {
        return TargetKey(d.regs[RB_SURFACE_INFO], d.regs[RB_COLOR_INFO], d.regs[RB_COLOR1_INFO], d.regs[RB_COLOR2_INFO],
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
            const std::vector<uint8_t> text = ReadFile(dir + "/" + d.ps + ".ps.hlsl");
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
        if (d.resolve) continue;
        const uint32_t written = writtenTargets(d);
        uint32_t& count = targetCounts[keyOf(d)];
        for (uint32_t k = 0; k < 4; ++k)
            if (written >> k & 1) count = (std::max)(count, k + 1);
        auto& size = targetSizes[keyOf(d)];
        size.first = (std::max)(size.first, uint32_t(std::lround(2 * std::fabs(AsFloat(d.regs[VPORT_XSCALE])))));
        size.second = (std::max)(size.second, uint32_t(std::lround(2 * std::fabs(AsFloat(d.regs[VPORT_YSCALE])))));
    }
    auto targetFor = [&](const Draw& d) -> Target& {
        const TargetKey key = keyOf(d);
        Target& t = targets[key];
        if (t.framebuffer) return t;
        // Size: the larger of the window scissor and the largest viewport of
        // the target's draws (PA_CL_VPORT scale is half the size in pixels);
        // the scissor can be stale, e.g. the final 1024x600 composite after a
        // 512x300 pass.
        const uint32_t br = d.regs[PA_SC_WINDOW_SCISSOR_BR];
        const auto& size = targetSizes[key];
        t.width = (std::max)(br & 0x7FFF, size.first);
        t.height = (std::max)((br >> 16) & 0x7FFF, size.second);
        t.width = (std::clamp)(t.width, 1u, 4096u);
        t.height = (std::clamp)(t.height, 1u, 4096u);
        t.count = (std::max)(targetCounts[key], 1u);
        const RenderTexture* colors[4] = {};
        for (uint32_t k = 0; k < t.count; ++k)
        {
            Surface& rt = t.rt[k];
            rt.format = ColorFormat(ColorInfo(d.regs, k));
            rt.texture = device->createTexture(RenderTextureDesc::ColorTarget(t.width, t.height, rt.format));
            rt.descriptor = textureCount + uint32_t((std::min)(nextPlain++, kMaxTargets - 1));
            sets[0]->setTexture(rt.descriptor, rt.texture.get(), RenderTextureLayout::SHADER_READ);
            rt.rowPitch = (t.width * FormatBytes(rt.format) + 255) & ~255u;
            rt.readback = device->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(rt.rowPitch) * t.height));
            colors[k] = rt.texture.get();
            list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(rt.texture.get(), RenderTextureLayout::COLOR_WRITE));
        }
        // Depth as R32_TYPELESS: D32_FLOAT view to render, R32_FLOAT views to
        // read it after a depth resolve (fog, lighting).
        t.ds.isDepth = true;
        t.ds.format = RenderFormat::R32_FLOAT;
        t.ds.texture = device->createTexture(RenderTextureDesc::DepthTarget(t.width, t.height, RenderFormat::R32_TYPELESS));
        t.dsv = t.ds.texture->createTextureView(RenderTextureViewDesc::Texture2D(RenderFormat::D32_FLOAT));
        RenderFramebufferDesc fb;
        fb.colorAttachments = colors;
        fb.colorAttachmentsCount = t.count;
        fb.depthAttachment = t.ds.texture.get();
        fb.depthAttachmentView = t.dsv.get();
        t.framebuffer = device->createFramebuffer(fb);
        list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(t.ds.texture.get(), RenderTextureLayout::DEPTH_WRITE));
        list->setFramebuffer(t.framebuffer.get());
        for (uint32_t k = 0; k < t.count; ++k) list->clearColor(k, RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
        // From Dust uses reversed depth (viewport Z scale -1, offset 1, GREATER_EQUAL).
        list->clearDepthStencil(true, false, 0.0f);
        return t;
    };

    // Pipelines keyed by shader, vertex layout and fixed-function state.
    std::map<std::string, std::pair<std::unique_ptr<RenderShader>, std::unique_ptr<RenderPipeline>>> pipelines;
    std::map<std::string, std::unique_ptr<RenderShader>> pixelShaders;
    auto pipelineFor = [&](const Draw& d) -> RenderPipeline* {
        const uint32_t depthControl = d.regs[RB_DEPTHCONTROL], mode = d.regs[PA_SU_SC_MODE_CNTL];
        std::string key = d.vs + "|" + d.ps + "|" + std::to_string(d.stride) + "|" + std::to_string(depthControl & 0x76) + "|" + std::to_string(mode & 7) +
            "|" + std::to_string(d.regs[RB_BLENDCONTROL0]) + "|" + std::to_string(d.regs[RB_COLOR_MASK] & 0xF) +
            "|" + std::to_string(int(ColorFormat(d.regs[RB_COLOR_INFO])));
        for (uint32_t k = 1; k < 4; ++k)
            key += "|" + std::to_string(ColorInfo(d.regs, k)) + "/" + std::to_string(BlendControl(d.regs, k));
        key += "|" + std::to_string(d.regs[RB_COLOR_MASK]) + "|" + std::to_string(writtenTargets(d));
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
                const std::vector<uint8_t> psFile = ReadFile(dir + "/" + d.ps + ".ps.dxil");
                found = pixelShaders.emplace(d.ps, psFile.empty() ? nullptr
                    : device->createShader(psFile.data(), psFile.size(), "main", RenderShaderFormat::DXIL)).first;
            }
            ps = found->second.get();
        }
        const std::vector<uint8_t> code = ReadFile(dir + "/" + d.vs + ".vs.dxil");
        if (!code.empty()) vs = device->createShader(code.data(), code.size(), "main", RenderShaderFormat::DXIL);
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
        desc.pixelShader = getenv("FD_REPLAY_DEBUG_PS") ? pixelShader.get() : ps;
        const uint32_t count = targetFor(d).count, written = writtenTargets(d);
        for (uint32_t k = 0; k < count; ++k)
        {
            desc.renderTargetFormat[k] = ColorFormat(ColorInfo(d.regs, k));
            desc.renderTargetBlend[k] = Blend(BlendControl(d.regs, k), (written >> k & 1) ? d.regs[RB_COLOR_MASK] >> (4 * k) : 0);
        }
        desc.renderTargetCount = count;
        // RB_DEPTHCONTROL: z_enable bit 1, z_write bit 2, zfunc bits 4-6.
        desc.depthEnabled = (depthControl >> 1) & 1;
        desc.depthWriteEnabled = (depthControl >> 2) & 1;
        desc.depthFunction = Compare(depthControl >> 4);
        desc.depthTargetFormat = RenderFormat::D32_FLOAT;
        // PA_SU_SC_MODE_CNTL: cull_front bit 0, cull_back bit 1, face bit 2 (1 =
        // clockwise front, the D3D12 default; 0 = counter-clockwise: swap sides).
        const bool cullFront = (mode & 4) ? (mode & 1) : (mode & 2), cullBack = (mode & 4) ? (mode & 2) : (mode & 1);
        desc.cullMode = cullBack ? RenderCullMode::BACK : cullFront ? RenderCullMode::FRONT : RenderCullMode::NONE;
        desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        if (vs) pipeline = device->createGraphicsPipeline(desc);
        PrintDebugMessages(d.vs.c_str());
        if (pipeline && !static_cast<D3D12GraphicsPipeline*>(pipeline.get())->d3d) { fprintf(stderr, "fd_replay: D3D12 rejected the pipeline for VS %s\n", d.vs.c_str()); pipeline.reset(); }
        if (!pipeline) fprintf(stderr, "fd_replay: no pipeline for VS %s\n", d.vs.c_str());
        return pipeline.get();
    };

    list->begin();
    size_t vertexAt = 0, indexAt = 0;
    const Target* current = nullptr;
    std::map<uint32_t, Surface*> resolved; // texture base address -> surface copied there
    std::map<uint64_t, std::pair<uint32_t, std::unique_ptr<RenderTextureView>>> tableViews; // (texture, swizzle) -> view
    uint32_t aliasHits = 0;
    uint32_t nextView = textureCount + 64; // first 64 slots: one plain view per target
    for (size_t i = 0; i < draws.size(); ++i)
    {
        const Draw& d = draws[i];
        if (d.resolve)
        {
            // Copy source: the target configured when the copy runs. Only
            // color copies (copy_src_select 0-3) are aliased for now.
            const uint32_t source = d.copyControl & 7; // copy_src_select: 0-3 color RT, 4 depth
            for (auto& [key, t] : targets)
                if (std::get<0>(key) == d.copySurface && std::get<1>(key) == d.copyColor && std::get<5>(key) == d.copyDepth)
                {
                    if (source < t.count && t.rt[source].texture) resolved[d.copyDest] = &t.rt[source];
                    else if (source == 4 && t.ds.texture) resolved[d.copyDest] = &t.ds;
                }
            continue;
        }
        memcpy(vertexMap + vertexAt, d.vertices, d.vertexBytes);
        memcpy(indexMap + indexAt, d.indices, d.indexBytes);
        uint8_t* drawConstants = constantMap + kPerDraw * (i + 1);
        memcpy(drawConstants, d.constants, 4096);
        memcpy(drawConstants + 4096, d.psConstants, 4096);
        uint8_t* shared = drawConstants + 8192;
        memset(shared, 0, kSharedSize);
        // g_Booleans (512): vertex bools in bits 0-15, pixel bools (Xenos bool
        // 128+, register 0x4904) in bits 16-31; loop constants at 544.
        const uint32_t booleans = (d.boolLoop[0] & 0xFFFF) | ((d.boolLoop[4] & 0xFFFF) << 16);
        memcpy(shared + 512, &booleans, 4);
        memcpy(shared + 544, &d.boolLoop[8], 32 * 4);
        memcpy(shared + kSharedVertexFetch, d.vertexFetch, 512);
        for (uint32_t slot = 0; slot < 32; ++slot)
        {
            uint32_t index = d.slots[slot] >= 0 ? uint32_t(d.slots[slot]) : 0;
            // Table textures are RGBA = Xenos components XYZW; the fetch swizzle
            // picks what the shader sees, through a view when not identity.
            if (d.slots[slot] >= 0 && d.swizzles[slot] != kIdentitySwizzle)
            {
                const uint64_t viewKey = uint64_t(uint32_t(d.slots[slot])) << 32 | d.swizzles[slot];
                auto view = tableViews.find(viewKey);
                if (view == tableViews.end() && nextView < textureCount + kMaxTargets)
                {
                    RenderTextureViewDesc viewDesc = RenderTextureViewDesc::Texture2D(RenderFormat::R8G8B8A8_UNORM);
                    viewDesc.componentMapping = SwizzleMapping(d.swizzles[slot]);
                    auto textureView = textures[d.slots[slot]]->createTextureView(viewDesc);
                    sets[0]->setTexture(nextView, textures[d.slots[slot]].get(), RenderTextureLayout::SHADER_READ, textureView.get());
                    view = tableViews.emplace(viewKey, std::make_pair(nextView++, std::move(textureView))).first;
                }
                if (view != tableViews.end()) index = view->second.first;
            }
            auto alias = d.bases[slot] ? resolved.find(d.bases[slot]) : resolved.end();
            Target& drawTarget = targetFor(d);
            const bool own = alias != resolved.end() && (alias->second == &drawTarget.ds ||
                std::any_of(std::begin(drawTarget.rt), std::end(drawTarget.rt), [&](const Surface& rt) { return &rt == alias->second; }));
            if (alias != resolved.end() && !own)
            {
                // A resolved read sees the target through the fetch constant's
                // swizzle (component i of the result = target channel swizzle[i]).
                Surface& source = *alias->second;
                auto view = source.views.find(d.swizzles[slot]);
                if (view == source.views.end() && nextView < textureCount + kMaxTargets)
                {
                    RenderTextureViewDesc viewDesc = RenderTextureViewDesc::Texture2D(source.format);
                    const uint32_t w = d.swizzles[slot];
                    viewDesc.componentMapping = SwizzleMapping(w);
                    auto textureView = source.texture->createTextureView(viewDesc);
                    sets[0]->setTexture(nextView, source.texture.get(), RenderTextureLayout::SHADER_READ, textureView.get());
                    view = source.views.emplace(w, std::make_pair(nextView++, std::move(textureView))).first;
                }
                index = view != source.views.end() ? view->second.first : alias->second->descriptor;
                ++aliasHits;
                if (!alias->second->shaderRead)
                {
                    list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(alias->second->texture.get(), RenderTextureLayout::SHADER_READ));
                    alias->second->shaderRead = true;
                }
            }
            memcpy(shared + kSharedTextureIndex + slot * 4, &index, 4); // 2D heap; sampler index stays 0
        }
        Target& target = targetFor(d);
        RenderPipeline* pipeline = pipelineFor(d);
        if (pipeline && d.primitive == 4)
        {
            for (uint32_t k = 0; k < target.count; ++k)
                if (target.rt[k].shaderRead)
                {
                    list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target.rt[k].texture.get(), RenderTextureLayout::COLOR_WRITE));
                    target.rt[k].shaderRead = false;
                }
            if (target.ds.shaderRead)
            {
                list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target.ds.texture.get(), RenderTextureLayout::DEPTH_WRITE));
                target.ds.shaderRead = false;
            }
            if (current != &target)
            {
                list->setFramebuffer(target.framebuffer.get());
                list->setGraphicsPipelineLayout(layout.get());
                for (uint32_t set = 0; set < 4; ++set) list->setGraphicsDescriptorSet(sets[set].get(), set);
                current = &target;
            }
            // Hardware viewport transform (PA_CL_VPORT_*): NDC -> pixels.
            const float xs = AsFloat(d.regs[VPORT_XSCALE]), xo = AsFloat(d.regs[VPORT_XOFFSET]);
            const float ys = AsFloat(d.regs[VPORT_YSCALE]), yo = AsFloat(d.regs[VPORT_YOFFSET]);
            const float zs = AsFloat(d.regs[VPORT_ZSCALE]), zo = AsFloat(d.regs[VPORT_ZOFFSET]);
            // Z keeps the game's mapping (reversed: min 1, max 0), which D3D12 allows.
            RenderViewport viewport(xo - std::fabs(xs), yo - std::fabs(ys), 2 * std::fabs(xs), 2 * std::fabs(ys), zo, zo + zs);
            if (!(viewport.width > 0)) viewport = RenderViewport(0, 0, float(target.width), float(target.height));
            list->setViewports(viewport);
            list->setScissors(RenderRect(0, 0, target.width, target.height));
            list->setPipeline(pipeline);
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
            const uint32_t count = (std::min)(d.indexCount, d.indexBytes / (d.index32 ? 4u : 2u));
            list->drawIndexedInstanced(count, 1, 0, int32_t(d.regs[VGT_INDX_OFFSET]), 0);
            ++target.draws;
        }
        vertexAt += d.vertexBytes;
        indexAt += d.indexBytes;
    }
    for (auto& [key, t] : targets)
        for (uint32_t k = 0; k < t.count; ++k)
        {
            Surface& rt = t.rt[k];
            list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(rt.texture.get(), RenderTextureLayout::COPY_SOURCE));
            list->copyTextureRegion(
                RenderTextureCopyLocation::PlacedFootprint(rt.readback.get(), rt.format, t.width, t.height, 1, rt.rowPitch / FormatBytes(rt.format)),
                RenderTextureCopyLocation::Subresource(rt.texture.get()));
        }
    list->end();
    vertexBuffer->unmap();
    indexBuffer->unmap();
    constantBuffer->unmap();
    const RenderCommandList* lists = list.get();
    queue->executeCommandLists(&lists, 1, nullptr, 0, nullptr, 0, fence.get());
    queue->waitForCommandFence(fence.get());

    int n = 0;
    for (auto& [key, t] : targets)
    {
        for (uint32_t k = 0; k < t.count; ++k)
        {
            Surface& rt = t.rt[k];
            const std::string path = dir + "/target_" + std::to_string(n) + (k ? "_rt" + std::to_string(k) : "") + ".png";
            const auto* pixels = static_cast<const uint8_t*>(rt.readback->map());
            std::vector<uint8_t> rgba(size_t(t.width) * t.height * 4);
            for (uint32_t y = 0; y < t.height; ++y)
                for (uint32_t x = 0; x < t.width; ++x)
                    ToRgba8(rt.format, pixels + size_t(y) * rt.rowPitch + size_t(x) * FormatBytes(rt.format), &rgba[(size_t(y) * t.width + x) * 4]);
            stbi_write_png(path.c_str(), t.width, t.height, 4, rgba.data(), t.width * 4);
            rt.readback->unmap();
        }
        printf("target_%d: %ux%u, %u RT, %u draws (surface %08X color %08X depth %08X)\n", n, t.width, t.height, t.count, t.draws,
            std::get<0>(key), std::get<1>(key), std::get<5>(key));
        ++n;
    }
    printf("fd_replay: %zu draws, %zu pipelines, %zu targets, %zu resolves mapped, %u resolved reads\n", draws.size(),
        pipelines.size(), targets.size(), resolved.size(), aliasHits);
    return 0;
}
