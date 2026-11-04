#pragma once
#include "frame.h"
#include "texture_decode.h"
#include "texture_storage.h"
#include <memory>

namespace gpu::native
{
struct PackedTextures
{
    std::vector<uint8_t> textures,cubes,mips,volumes;
    std::vector<uint64_t> versions;
};
// Per-frame owned snapshots. Adding another version of the same guest address
// returns another index, preserving resources already referenced by draws.
class TextureTables
{
public:
    using Entry=TextureResource;
    std::span<const std::shared_ptr<Entry>> Resources()const{return entries;}
    static std::shared_ptr<Entry> CaptureResource(std::span<const uint32_t,6> fetch,const TextureMemoryReader& read);
    uint32_t AddResource(std::shared_ptr<Entry> entry);
    uint32_t Add(std::span<const uint32_t,6> fetch,std::span<const uint8_t> bytes);
    // Native memory producer: bank layout and mip positions come from the
    // fetch itself, with no GPU-register dump or captured mip descriptors.
    uint32_t Capture(std::span<const uint32_t,6> fetch,const TextureMemoryReader& read);
    void Mips(uint32_t index,std::span<const uint32_t,6> fetch,std::span<const uint8_t> bank,std::span<const MipLayout> layouts);
    void Bind(Draw& draw,uint32_t slot,uint32_t index)const;
    PackedTextures Pack()const;
private:
    std::vector<std::shared_ptr<Entry>> entries;
    std::vector<uint32_t> mipOrder;
};
}
