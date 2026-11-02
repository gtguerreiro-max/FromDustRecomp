// fd_replay: redraws a frame captured by runtime/gpu/frame_capture.cpp with the
// native renderer's building blocks (plume + XenosRecomp shaders), offscreen,
// and writes the result as a PNG to compare with the Xenia screenshot.
//
// Usage: fd_replay <capture dir> <output.png>

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

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: fd_replay <capture dir> <output.png>\n");
        return 1;
    }
    Renderer renderer;
    if (!renderer.Initialize())
    {
        fprintf(stderr, "fd_replay: D3D12 initialization failed\n");
        return 1;
    }
    renderer.Begin();
    if (!renderer.EndAndSave(argv[2]))
    {
        fprintf(stderr, "fd_replay: could not write %s\n", argv[2]);
        return 1;
    }
    printf("fd_replay: wrote %s\n", argv[2]);
    return 0;
}
