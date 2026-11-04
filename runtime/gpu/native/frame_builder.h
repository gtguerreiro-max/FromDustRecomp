#pragma once
#include "geometry.h"
#include "texture_tables.h"
#include <map>

namespace gpu::native
{
struct GeometryBatch
{
    struct Command
    {
        std::string vertexShader,pixelShader;
        std::array<uint32_t,1024> vertexConstants,pixelConstants;
        Draw state;
        Geometry geometry;
        void BindGeometryAndConstants(Draw& draw) const;
    };
    std::vector<Command> commands;
    StreamArena streams;
    uint64_t serial=0;
};
// Borrows the immutable batch's geometry/constant bytes. The batch and texture
// tables must outlive RenderFrame. The map uses physical guest base addresses.
std::vector<Draw> BuildFrameDraws(const GeometryBatch& batch,const ShaderCatalog& shaders,
    const TextureTables& textures,const std::map<uint32_t,uint32_t>& resources);
}
