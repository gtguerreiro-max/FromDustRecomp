#pragma once

#include <cstdint>
#include <memory>
#include <string>
struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12CommandQueue;

namespace gpu::native
{
struct FrameInput;
struct FrameOutput;
// Shared renderer context. RenderFrame accepts immutable in-memory inputs;
// RenderReplay is a file adapter. Continuous frames retain targets, copies,
// shaders and owned textures; PresentTo shares their device with the window.
// Calls are serialized by the caller; each frame waits for GPU completion.
class Renderer
{
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    static bool PrepareDeviceCreation();

    int RenderReplay(const std::string& directory);
    int RenderFrame(const FrameInput& frame, const std::string& diagnosticDirectory = {}, FrameOutput* output = nullptr);
    // Select the same compiled assets used by the live shader registry.
    // Must be called before rendering; missing catalog entries are fatal.
    bool LoadShaderCatalog(const std::string& path);
    bool UseDevice(ID3D12Device* device);
    bool PresentTo(ID3D12Resource* destination,ID3D12CommandQueue* queue);
    uint32_t OutputWidth()const;
    uint32_t OutputHeight()const;
    uint64_t CompletedFrames() const;
    uint64_t CompletedDraws() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
