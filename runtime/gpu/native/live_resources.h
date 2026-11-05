#pragma once
#ifdef FD_NATIVE_RENDERER
#include "texture_tables.h"
#include "shader_catalog.h"
#include <map>

namespace gpu::native
{
// Called on the engine render thread. Memory invalidations only change atomic
// page revisions; they never acquire this cache's locks or decode textures.
class LiveResources
{
public:
    void BeginFrame();
    void Bind(Draw& draw,const ShaderCatalog& shaders);
    void Resolve(std::span<const uint32_t,6> fetch);
    PackedTextures Pack()const{return tables.Pack();}
    auto FrameResources()const{return tables.Resources();}
private:
    struct Cached
    {
        TextureStorage storage;
        uint64_t revision=UINT64_MAX;
        // Revision of the storage pages as of a global write serial: while no
        // watched page has been written since, the revision is unchanged.
        uint64_t checkedSerial=UINT64_MAX,checkedRevision=0;
        std::shared_ptr<TextureTables::Entry> resource;
    };
    std::map<std::array<uint32_t,6>,Cached> resources;
    std::map<uint32_t,uint64_t> gpuAuthority;
    std::map<const TextureTables::Entry*,uint32_t> frameIndices;
    TextureTables tables;
public:
    // FD_NATIVE_PROFILE counters since the last BeginFrame.
    uint64_t profileInserted=0,profileCaptured=0,profileCaptureBytes=0;
    double profileCaptureMs=0;
};
}
#endif
