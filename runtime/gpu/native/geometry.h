#pragma once
#include "frame.h"
#include "shader_catalog.h"
#include <array>
#include <functional>

namespace gpu::native
{
struct VertexProgram
{
    struct Input { uint32_t destination; std::string semantic; };
    uint32_t codeOffset = 0;
    std::vector<Input> inputs;
    static VertexProgram FromShader(const NativeShader& shader);
};

// Physical address and requested byte count -> currently readable guest bytes.
// An offline reader may expose a longer captured snapshot at that address.
using GeometryReader = std::function<std::span<const uint8_t>(uint32_t, uint32_t)>;

struct StreamArena
{
    struct Entry { uint32_t address, offset, size; uint64_t revision=0; };
    std::vector<Entry> entries;
    std::vector<uint8_t> bytes;
    // Optional live write-watch generation covering the entire source range.
    // Without it, snapshots are compared byte-for-byte as in offline readers.
    std::function<uint64_t(uint32_t,uint32_t)> revision;
};

struct Geometry
{
    std::vector<uint8_t> vertices, indices;
    std::vector<std::string> semantics;
    std::array<uint32_t, 128> fetches = {};
    uint32_t stride = 0, primitive = 0, indexCount = 0, index32 = 0;
    bool replacesIndexOffset = false;
    void Bind(Draw& draw) const;
};
struct GeometryBounds { int32_t indexOffset=0; bool pullPrimary=false; };

// Copies geometry while guest data is valid. The result and arena own every
// byte consumed by RenderFrame; no guest pointer crosses the GPU submission.
Geometry BuildGeometry(const VertexProgram& program, std::span<const uint8_t> livePhysical,
    std::span<const uint32_t, 192> fetchConstants, uint32_t primitive, uint32_t count,
    bool indexed, bool index32, uint32_t indexAddress, const GeometryReader& read, StreamArena& arena,
    const GeometryBounds* bounds=nullptr);
}
