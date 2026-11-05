#pragma once
#include <array>
#include <memory>
#include <cstdint>
#include <span>
#include <vector>

namespace gpu::native
{
struct TextureInfo
{
    uint32_t width,height,pitch,format,dimension,endian;
    bool tiled;
    uint32_t depth=1;
    static TextureInfo FromFetch(std::span<const uint32_t,6> fetch);
};
struct DecodedTexture
{
    uint32_t width=0,height=0;
    bool floatPixels=false;
    std::vector<uint8_t> pixels;
};
struct MipLayout
{
    uint32_t rowPitchBytes,offsetBytes,xBlocks,yBlocks;
};
struct TextureDeferred;
struct TextureResource
{
    std::array<uint32_t,6> fetch;
    // Live captures: the guest bytes copied on the game thread, decoded by
    // ResolveTexture on the render worker (image, cube, mips, volume).
    std::shared_ptr<TextureDeferred> deferred;
    DecodedTexture image;
    std::vector<uint8_t> cube;
    std::vector<DecodedTexture> mips;
    uint64_t version=0;
    // Volume slices in z-major RGBA / float4 order. image owns slice zero.
    uint32_t volumeDepth=0;
    std::vector<uint8_t> volume;
};
struct TextureBlockInfo { uint32_t bytes,texels; };
TextureBlockInfo GetTextureBlockInfo(uint32_t format);
// Old capture files may contain a tightly repacked linear image. Memory
// snapshots retain the fetch pitch and must pass allowTightStorage=false.
DecodedTexture DecodeTexture(std::span<const uint32_t,6> fetch,std::span<const uint8_t> bytes,bool allowTightStorage=true);
std::vector<uint8_t> DecodeVolume(std::span<const uint32_t,6> fetch,std::span<const uint8_t> bytes);
uint64_t TextureBlockAddress3D(uint32_t x,uint32_t y,uint32_t z,uint32_t pitch,uint32_t rows,uint32_t bytes);
DecodedTexture DecodeMip(std::span<const uint32_t,6> fetch,std::span<const uint8_t> bank,uint32_t level,const MipLayout& layout);
uint32_t CubeFaceStride(const TextureInfo& info);
std::vector<uint8_t> DecodeCube(std::span<const uint32_t,6> fetch,std::span<const uint8_t> bytes,bool allowTightStorage=true);
}
