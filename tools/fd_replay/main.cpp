// fd_replay: redraws a frame captured by runtime/gpu/frame_capture.cpp with the
// native renderer's building blocks (plume + XenosRecomp shaders), offscreen,
// and writes the result as a PNG to compare with the Xenia screenshot.
//
// Usage: fd_replay <capture dir> <output.png>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
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
constexpr uint32_t kWidth = 1024, kHeight = 600; // the game's render size
constexpr uint32_t kRowPitch = (kWidth * 4 + 255) & ~255u;

struct Renderer
{
    std::unique_ptr<RenderInterface> api;
    std::unique_ptr<RenderDevice> device;
    std::unique_ptr<RenderCommandQueue> queue;
    std::unique_ptr<RenderCommandList> list;
    std::unique_ptr<RenderCommandFence> fence;
    std::unique_ptr<RenderTexture> color;
    std::unique_ptr<RenderFramebuffer> framebuffer;
    std::unique_ptr<RenderBuffer> readback;

    bool Initialize()
    {
        api = CreateD3D12Interface();
        if (!api) return false;
        device = api->createDevice();
        if (!device) return false;
        queue = device->createCommandQueue(RenderCommandListType::DIRECT);
        list = queue->createCommandList();
        fence = device->createCommandFence();
        color = device->createTexture(RenderTextureDesc::ColorTarget(kWidth, kHeight, RenderFormat::R8G8B8A8_UNORM));
        const RenderTexture* attachment = color.get();
        RenderFramebufferDesc desc;
        desc.colorAttachments = &attachment;
        desc.colorAttachmentsCount = 1;
        framebuffer = device->createFramebuffer(desc);
        readback = device->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(kRowPitch) * kHeight));
        return framebuffer && readback;
    }

    void Begin()
    {
        list->begin();
        list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(color.get(), RenderTextureLayout::COLOR_WRITE));
        list->setFramebuffer(framebuffer.get());
        list->setViewports(RenderViewport(0.0f, 0.0f, float(kWidth), float(kHeight)));
        list->setScissors(RenderRect(0, 0, kWidth, kHeight));
        list->clearColor(0, RenderColor(0.1f, 0.0f, 0.2f, 1.0f));
    }

    bool EndAndSave(const char* path)
    {
        list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(color.get(), RenderTextureLayout::COPY_SOURCE));
        list->copyTextureRegion(
            RenderTextureCopyLocation::PlacedFootprint(readback.get(), RenderFormat::R8G8B8A8_UNORM, kWidth, kHeight, 1, kRowPitch / 4),
            RenderTextureCopyLocation::Subresource(color.get()));
        list->end();
        const RenderCommandList* lists = list.get();
        queue->executeCommandLists(&lists, 1, nullptr, 0, nullptr, 0, fence.get());
        queue->waitForCommandFence(fence.get());
        const auto* pixels = static_cast<const uint8_t*>(readback->map());
        const bool ok = stbi_write_png(path, kWidth, kHeight, 4, pixels, kRowPitch) != 0;
        readback->unmap();
        return ok;
    }
};
}

namespace
{
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

// One draw from replay.bin (tools/replay_pack.py documents the layout).
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
};

std::vector<Draw> ParseReplay(const std::vector<uint8_t>& file)
{
    std::vector<Draw> draws;
    size_t at = 0;
    auto u32 = [&] { uint32_t v; memcpy(&v, file.data() + at, 4); at += 4; return v; };
    auto bytes = [&](uint32_t n) { const uint8_t* p = file.data() + at; at += n; return p; };
    if (file.size() < 8 || u32() != 0x50524446) return draws;
    const uint32_t count = u32();
    for (uint32_t i = 0; i < count; ++i)
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
        draws.push_back(d);
    }
    return draws;
}
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: fd_replay <replay dir> <output.png>\n");
        return 1;
    }
    const std::string dir = argv[1];
    const std::vector<uint8_t> file = ReadFile(dir + "/replay.bin");
    const std::vector<Draw> draws = ParseReplay(file);
    fprintf(stderr, "\nfd_replay: parsed %zu draws", draws.size());
    Renderer renderer;
    if (!renderer.Initialize())
    {
        fprintf(stderr, "\nfd_replay: D3D12 initialization failed\n");
        return 1;
    }
    RenderDevice& device = *renderer.device;

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
    fprintf(stderr, "\nfd_replay: before layout");
    auto layout = device.createPipelineLayout(layoutDesc);

    fprintf(stderr, "\nfd_replay: layout ok");
    const std::vector<uint8_t> psCode = ReadFile(dir + "/debug.ps.dxil");
    auto pixelShader = device.createShader(psCode.data(), psCode.size(), "main", RenderShaderFormat::DXIL);

    fprintf(stderr, "\nfd_replay: pixel shader %p", (void*)pixelShader.get());
    // All per-draw data in a few upload buffers.
    size_t vertexTotal = 0, indexTotal = 0;
    for (const Draw& d : draws) vertexTotal += d.vertexBytes, indexTotal += d.indexBytes;
    auto vertexBuffer = device.createBuffer(RenderBufferDesc::VertexBuffer((std::max<size_t>)(vertexTotal, 16), RenderHeapType::UPLOAD));
    auto indexBuffer = device.createBuffer(RenderBufferDesc::IndexBuffer((std::max<size_t>)(indexTotal, 16), RenderHeapType::UPLOAD));
    auto constantBuffer = device.createBuffer(RenderBufferDesc::UploadBuffer(4096 * (draws.size() + 1) + 4096, RenderBufferFlag::CONSTANT));
    auto* vertexMap = static_cast<uint8_t*>(vertexBuffer->map());
    auto* indexMap = static_cast<uint8_t*>(indexBuffer->map());
    auto* constantMap = static_cast<uint8_t*>(constantBuffer->map());
    memset(constantMap, 0, 4096); // shared constants and pixel constants: zero for now

    fprintf(stderr, "\nfd_replay: buffers mapped");
    std::unique_ptr<RenderTexture> depth = device.createTexture(RenderTextureDesc::DepthTarget(kWidth, kHeight, RenderFormat::D32_FLOAT));
    const RenderTexture* colorAttachment = renderer.color.get();
    RenderFramebufferDesc fbDesc;
    fbDesc.colorAttachments = &colorAttachment;
    fbDesc.colorAttachmentsCount = 1;
    fbDesc.depthAttachment = depth.get();
    renderer.framebuffer = device.createFramebuffer(fbDesc);

    struct Pipeline { std::unique_ptr<RenderShader> vs; std::unique_ptr<RenderPipeline> pipeline; };
    std::vector<std::pair<std::string, Pipeline>> pipelines;
    auto pipelineFor = [&](const Draw& d) -> RenderPipeline* {
        std::string key = d.vs;
        for (const auto& s : d.semantics) key += "|" + s;
        for (auto& [k, p] : pipelines) if (k == key) return p.pipeline.get();
        Pipeline p;
        const std::vector<uint8_t> code = ReadFile(dir + "/" + d.vs + ".vs.dxil");
        if (!code.empty()) p.vs = device.createShader(code.data(), code.size(), "main", RenderShaderFormat::DXIL);
        std::vector<std::string> names;
        std::vector<RenderInputElement> elements;
        for (const auto& s : d.semantics) names.push_back(s.substr(0, s.find_first_of("0123456789")));
        for (size_t i = 0; i < d.semantics.size(); ++i)
        {
            const uint32_t index = uint32_t(atoi(d.semantics[i].c_str() + names[i].size()));
            elements.emplace_back(names[i].c_str(), index, uint32_t(i), RenderFormat::R32G32B32A32_FLOAT, 0, uint32_t(i * 16));
        }
        RenderInputSlot slot(0, d.stride);
        RenderGraphicsPipelineDesc desc;
        desc.inputSlots = &slot;
        desc.inputSlotsCount = 1;
        desc.inputElements = elements.data();
        desc.inputElementsCount = uint32_t(elements.size());
        desc.pipelineLayout = layout.get();
        desc.vertexShader = p.vs.get();
        desc.pixelShader = pixelShader.get();
        desc.renderTargetFormat[0] = RenderFormat::R8G8B8A8_UNORM;
        desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
        desc.renderTargetCount = 1;
        desc.depthEnabled = true;
        desc.depthWriteEnabled = true;
        desc.depthFunction = RenderComparisonFunction::LESS_EQUAL;
        desc.depthTargetFormat = RenderFormat::D32_FLOAT;
        desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        fprintf(stderr, "\n  vs %s code %zu shader %p", d.vs.c_str(), code.size(), (void*)p.vs.get());
        if (p.vs) p.pipeline = device.createGraphicsPipeline(desc);
        if (!p.pipeline) fprintf(stderr, "\nfd_replay: no pipeline for VS %s\n", d.vs.c_str());
        pipelines.emplace_back(key, std::move(p));
        return pipelines.back().second.pipeline.get();
    };

    fprintf(stderr, "\nfd_replay: framebuffer %p", (void*)renderer.framebuffer.get());
    renderer.Begin();
    RenderCommandList& list = *renderer.list;
    list.barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(depth.get(), RenderTextureLayout::DEPTH_WRITE));
    list.setFramebuffer(renderer.framebuffer.get());
    list.clearDepthStencil(true, false, 1.0f);
    list.setGraphicsPipelineLayout(layout.get());
    size_t vertexAt = 0, indexAt = 0;
    uint32_t drawn = 0;
    for (size_t i = 0; i < draws.size(); ++i)
    {
        const Draw& d = draws[i];
        memcpy(vertexMap + vertexAt, d.vertices, d.vertexBytes);
        memcpy(indexMap + indexAt, d.indices, d.indexBytes);
        memcpy(constantMap + 4096 * (i + 1), d.constants, 4096);
        if (i < 2) fprintf(stderr, "\ndraw %zu vs %s stride %u elements %zu", i, d.vs.c_str(), d.stride, d.semantics.size());
        RenderPipeline* pipeline = pipelineFor(d);
        if (i < 2) fprintf(stderr, "\n  pipeline %p", (void*)pipeline);
        if (pipeline && d.primitive == 4)
        {
            list.setPipeline(pipeline);
            list.setGraphicsRootDescriptor(RenderBufferReference(constantBuffer.get(), 4096 * (i + 1)), 0);
            list.setGraphicsRootDescriptor(RenderBufferReference(constantBuffer.get(), 0), 1);
            list.setGraphicsRootDescriptor(RenderBufferReference(constantBuffer.get(), 0), 2);
            RenderVertexBufferView vertexView(RenderBufferReference(vertexBuffer.get(), vertexAt), d.vertexBytes);
            RenderInputSlot slot(0, d.stride);
            list.setVertexBuffers(0, &vertexView, 1, &slot);
            RenderIndexBufferView indexView(RenderBufferReference(indexBuffer.get(), indexAt), d.indexBytes,
                d.index32 ? RenderFormat::R32_UINT : RenderFormat::R16_UINT);
            list.setIndexBuffer(&indexView);
            list.drawIndexedInstanced((std::min)(d.indexCount, d.indexBytes / (d.index32 ? 4u : 2u)), 1, 0, 0, 0);
            ++drawn;
        }
        vertexAt += d.vertexBytes;
        indexAt += d.indexBytes;
    }
    vertexBuffer->unmap();
    indexBuffer->unmap();
    constantBuffer->unmap();
    printf("fd_replay: %zu draws read, %u drawn, %zu pipelines\n", draws.size(), drawn, pipelines.size());
    if (!renderer.EndAndSave(argv[2]))
    {
        fprintf(stderr, "\nfd_replay: could not write %s\n", argv[2]);
        return 1;
    }
    printf("fd_replay: wrote %s\n", argv[2]);
    return 0;
}
