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
        Command()=default;
        // Copy-constructs the large members directly (value-initializing a
        // command zeroed ~15 KB per draw before every field was overwritten).
        Command(const std::string& vs,const std::string& ps,const std::array<uint32_t,1024>& vertex,
            const std::array<uint32_t,1024>& pixel,const Draw& drawState,Geometry&& drawGeometry)
            :vertexShader(vs),pixelShader(ps),vertexConstants(vertex),pixelConstants(pixel),state(drawState),geometry(std::move(drawGeometry)){}
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
