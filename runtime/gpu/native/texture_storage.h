#pragma once
#include "texture_decode.h"
#include <functional>

namespace gpu::native
{
// Addresses are physical guest byte offsets. 2D bases exclude unused trailing
// row padding; tiled mip banks retain the hardware tile extent. Cubes use six
// complete aligned face slices, matching the current cube decoder contract.
struct TextureStorage
{
    uint32_t baseAddress=0,baseBytes=0,mipAddress=0,mipBytes=0;
    std::vector<MipLayout> mips;
};
TextureStorage DescribeTextureStorage(std::span<const uint32_t,6> fetch);
using TextureMemoryReader=std::function<std::span<const uint8_t>(uint32_t,uint32_t)>;
struct TextureSnapshot
{
    std::array<uint32_t,6> fetch;
    TextureStorage storage;
    std::vector<uint8_t> base,mips;
};
// Reads and owns both banks before returning. The caller must synchronize
// GPU-produced memory first; copying CPU memory alone does not prove freshness.
TextureSnapshot SnapshotTexture(std::span<const uint32_t,6> fetch,const TextureMemoryReader& read);
}
