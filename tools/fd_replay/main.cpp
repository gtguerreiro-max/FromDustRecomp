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

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace plume
{
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
}

using namespace plume;

namespace
{
// Order of the per-draw registers written by replay_pack.py (STATE_REGISTERS).
enum Reg
{
    RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO, PA_SC_WINDOW_SCISSOR_TL, PA_SC_WINDOW_SCISSOR_BR,
    RB_COLOR_MASK, RB_DEPTHCONTROL, RB_BLENDCONTROL0, PA_SU_SC_MODE_CNTL, VGT_INDX_OFFSET,
    VPORT_XSCALE, VPORT_XOFFSET, VPORT_YSCALE, VPORT_YOFFSET, VPORT_ZSCALE, VPORT_ZOFFSET, PA_CL_VTE_CNTL,
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
};

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

// A render target configuration of the frame (EDRAM color/depth bases + pitch).
struct Target
{
    uint32_t width = 0, height = 0;
    std::unique_ptr<RenderTexture> color, depth;
    std::unique_ptr<RenderFramebuffer> framebuffer;
    std::unique_ptr<RenderBuffer> readback;
    uint32_t rowPitch = 0, draws = 0;
};
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

    auto api = CreateD3D12Interface();
    auto device = api ? api->createDevice() : nullptr;
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
    };
    RenderPipelineLayoutDesc layoutDesc;
    layoutDesc.rootDescriptorDescs = roots;
    layoutDesc.rootDescriptorDescsCount = 3;
    layoutDesc.allowInputLayout = true;
    auto layout = device->createPipelineLayout(layoutDesc);
    const std::vector<uint8_t> psCode = ReadFile(dir + "/debug.ps.dxil");
    auto pixelShader = device->createShader(psCode.data(), psCode.size(), "main", RenderShaderFormat::DXIL);

    // All per-draw data in a few upload buffers.
    size_t vertexTotal = 16, indexTotal = 16;
    for (const Draw& d : draws) vertexTotal += d.vertexBytes, indexTotal += d.indexBytes;
    auto vertexBuffer = device->createBuffer(RenderBufferDesc::VertexBuffer(vertexTotal, RenderHeapType::UPLOAD));
    auto indexBuffer = device->createBuffer(RenderBufferDesc::IndexBuffer(indexTotal, RenderHeapType::UPLOAD));
    auto constantBuffer = device->createBuffer(RenderBufferDesc::UploadBuffer(4096 * (draws.size() + 1), RenderBufferFlag::CONSTANT));
    auto* vertexMap = static_cast<uint8_t*>(vertexBuffer->map());
    auto* indexMap = static_cast<uint8_t*>(indexBuffer->map());
    auto* constantMap = static_cast<uint8_t*>(constantBuffer->map());
    memset(constantMap, 0, 4096); // shared and pixel constants: zero for now

    // Targets keyed by (surface info, color info, depth info).
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, Target> targets;
    auto targetFor = [&](const Draw& d) -> Target& {
        auto key = std::make_tuple(d.regs[RB_SURFACE_INFO], d.regs[RB_COLOR_INFO], d.regs[RB_DEPTH_INFO]);
        Target& t = targets[key];
        if (t.color) return t;
        // Size: window scissor when the frame wrote it, else the viewport
        // (PA_CL_VPORT scale is half the size in pixels).
        const uint32_t br = d.regs[PA_SC_WINDOW_SCISSOR_BR];
        t.width = br & 0x7FFF;
        t.height = (br >> 16) & 0x7FFF;
        if (!t.width || !t.height)
        {
            t.width = uint32_t(std::lround(2 * std::fabs(AsFloat(d.regs[VPORT_XSCALE]))));
            t.height = uint32_t(std::lround(2 * std::fabs(AsFloat(d.regs[VPORT_YSCALE]))));
        }
        t.width = (std::clamp)(t.width, 1u, 4096u);
        t.height = (std::clamp)(t.height, 1u, 4096u);
        t.color = device->createTexture(RenderTextureDesc::ColorTarget(t.width, t.height, RenderFormat::R8G8B8A8_UNORM));
        t.depth = device->createTexture(RenderTextureDesc::DepthTarget(t.width, t.height, RenderFormat::D32_FLOAT));
        const RenderTexture* color = t.color.get();
        RenderFramebufferDesc fb;
        fb.colorAttachments = &color;
        fb.colorAttachmentsCount = 1;
        fb.depthAttachment = t.depth.get();
        t.framebuffer = device->createFramebuffer(fb);
        t.rowPitch = (t.width * 4 + 255) & ~255u;
        t.readback = device->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(t.rowPitch) * t.height));
        list->barriers(RenderBarrierStage::GRAPHICS, {RenderTextureBarrier(t.color.get(), RenderTextureLayout::COLOR_WRITE),
            RenderTextureBarrier(t.depth.get(), RenderTextureLayout::DEPTH_WRITE)});
        list->setFramebuffer(t.framebuffer.get());
        list->clearColor(0, RenderColor(0.1f, 0.0f, 0.2f, 1.0f));
        // From Dust uses reversed depth (viewport Z scale -1, offset 1, GREATER_EQUAL).
        list->clearDepthStencil(true, false, 0.0f);
        return t;
    };

    // Pipelines keyed by shader, vertex layout and fixed-function state.
    std::map<std::string, std::pair<std::unique_ptr<RenderShader>, std::unique_ptr<RenderPipeline>>> pipelines;
    auto pipelineFor = [&](const Draw& d) -> RenderPipeline* {
        const uint32_t depthControl = d.regs[RB_DEPTHCONTROL], mode = d.regs[PA_SU_SC_MODE_CNTL];
        std::string key = d.vs + "|" + std::to_string(d.stride) + "|" + std::to_string(depthControl & 0x76) + "|" + std::to_string(mode & 7);
        for (const auto& s : d.semantics) key += "|" + s;
        auto it = pipelines.find(key);
        if (it != pipelines.end()) return it->second.second.get();
        auto& [vs, pipeline] = pipelines[key];
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
        desc.pixelShader = pixelShader.get();
        desc.renderTargetFormat[0] = RenderFormat::R8G8B8A8_UNORM;
        desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
        desc.renderTargetCount = 1;
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
        if (!pipeline) fprintf(stderr, "fd_replay: no pipeline for VS %s\n", d.vs.c_str());
        return pipeline.get();
    };

    list->begin();
    size_t vertexAt = 0, indexAt = 0;
    const Target* current = nullptr;
    for (size_t i = 0; i < draws.size(); ++i)
    {
        const Draw& d = draws[i];
        memcpy(vertexMap + vertexAt, d.vertices, d.vertexBytes);
        memcpy(indexMap + indexAt, d.indices, d.indexBytes);
        memcpy(constantMap + 4096 * (i + 1), d.constants, 4096);
        Target& target = targetFor(d);
        RenderPipeline* pipeline = pipelineFor(d);
        if (pipeline && d.primitive == 4)
        {
            if (current != &target)
            {
                list->setFramebuffer(target.framebuffer.get());
                list->setGraphicsPipelineLayout(layout.get());
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
            list->setGraphicsRootDescriptor(RenderBufferReference(constantBuffer.get(), 4096 * (i + 1)), 0);
            list->setGraphicsRootDescriptor(RenderBufferReference(constantBuffer.get(), 0), 1);
            list->setGraphicsRootDescriptor(RenderBufferReference(constantBuffer.get(), 0), 2);
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
    {
        list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(t.color.get(), RenderTextureLayout::COPY_SOURCE));
        list->copyTextureRegion(
            RenderTextureCopyLocation::PlacedFootprint(t.readback.get(), RenderFormat::R8G8B8A8_UNORM, t.width, t.height, 1, t.rowPitch / 4),
            RenderTextureCopyLocation::Subresource(t.color.get()));
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
        const std::string path = dir + "/target_" + std::to_string(n++) + ".png";
        const auto* pixels = static_cast<const uint8_t*>(t.readback->map());
        stbi_write_png(path.c_str(), t.width, t.height, 4, pixels, t.rowPitch);
        t.readback->unmap();
        printf("%s: %ux%u, %u draws (surface %08X color %08X depth %08X)\n", path.c_str(), t.width, t.height, t.draws,
            std::get<0>(key), std::get<1>(key), std::get<2>(key));
    }
    printf("fd_replay: %zu draws, %zu pipelines, %zu targets\n", draws.size(), pipelines.size(), targets.size());
    return 0;
}
