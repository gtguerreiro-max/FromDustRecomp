#pragma once
#include <memory>
#include <map>
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
    std::string name;
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
    // With the persistent cache (live capture) streams are not copied into
    // `bytes`: each occupies [offset, offset + size) of the frame's stream
    // space and refers to the cache's converted bytes.
    struct Segment { uint32_t offset = 0; std::shared_ptr<const std::vector<uint8_t>> data; };
    std::vector<Segment> segments;
    uint32_t segmentEnd = 0;
    uint32_t Size() const { return segments.empty() ? uint32_t(bytes.size()) : segmentEnd; }
    // Optional live write-watch generation covering the entire source range.
    // Without it, snapshots are compared byte-for-byte as in offline readers.
    std::function<uint64_t(uint32_t,uint32_t)> revision;
    // Optional cross-frame cache of converted (little-endian) streams and
    // indices, keyed by guest range and valid for one write-watch revision.
    // Converted bytes are replaced, never modified, so frames in flight can
    // keep referring to an older version.
    struct Converted { uint64_t revision = 0, used = 0; uint32_t minimum = 0, maximum = 0; std::shared_ptr<const std::vector<uint8_t>> bytes; };
    struct Persistent
    {
        std::map<std::pair<uint32_t, uint32_t>, Converted> streams, indices;
        uint64_t frame = 0;
        // Replaced converted buffers, kept for reuse once no frame in flight
        // refers to them: a dynamic stream rewritten every world step then
        // alternates between two buffers instead of a fresh zero-filled
        // allocation (~16 MB per frame in play) each time.
        std::vector<std::shared_ptr<const std::vector<uint8_t>>> retired;
        size_t retiredBytes = 0;
        std::shared_ptr<std::vector<uint8_t>> Take(size_t size)
        {
            for (size_t i = retired.size(); i-- > 0;)
                if (retired[i]->size() == size && retired[i].use_count() == 1)
                {
                    auto reused = std::const_pointer_cast<std::vector<uint8_t>>(std::move(retired[i]));
                    retired.erase(retired.begin() + ptrdiff_t(i));
                    retiredBytes -= size;
                    return reused;
                }
            return std::make_shared<std::vector<uint8_t>>(size);
        }
        void Retire(std::shared_ptr<const std::vector<uint8_t>> old)
        {
            if (!old || old->size() < 4096) return;
            retiredBytes += old->size();
            retired.push_back(std::move(old));
            while (retiredBytes > (256u << 20) && !retired.empty())
            {
                retiredBytes -= retired.front()->size();
                retired.erase(retired.begin());
            }
        }
        // Called once per frame: drops ranges unused for 300 frames (moving
        // dynamic buffers would otherwise grow the cache without bound).
        void NextFrame()
        {
            if (++frame % 300) return;
            for (auto* map : {&streams, &indices})
                std::erase_if(*map, [&](const auto& entry) { return entry.second.used + 300 < frame; });
        }
    };
    std::shared_ptr<Persistent> persistent;
};

struct Geometry
{
    std::vector<uint8_t> vertices, indices;
    // Indices shared with the persistent cache (used instead of `indices`).
    std::shared_ptr<const std::vector<uint8_t>> sharedIndices;
    std::span<const uint8_t> IndexBytes() const { return sharedIndices ? std::span<const uint8_t>(*sharedIndices) : std::span<const uint8_t>(indices); }
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
