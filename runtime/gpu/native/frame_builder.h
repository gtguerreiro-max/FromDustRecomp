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
        // Copies only the constant registers the shaders read (the ranges in
        // drawState); the rest of the banks is left unset.
        struct UsedConstants{};
        Command(UsedConstants,const std::string& vs,const std::string& ps,const std::array<uint32_t,1024>& vertex,
            const std::array<uint32_t,1024>& pixel,const Draw& drawState,Geometry&& drawGeometry)
            :vertexShader(vs),pixelShader(ps),state(drawState),geometry(std::move(drawGeometry))
        {
            std::copy(vertex.begin()+state.vsConstFirst*4,vertex.begin()+state.vsConstEnd*4,vertexConstants.begin()+state.vsConstFirst*4);
            std::copy(pixel.begin()+state.psConstFirst*4,pixel.begin()+state.psConstEnd*4,pixelConstants.begin()+state.psConstFirst*4);
        }
        std::string vertexShader,pixelShader;
        std::array<uint32_t,1024> vertexConstants,pixelConstants;
        Draw state;
        Geometry geometry;
        void BindGeometryAndConstants(Draw& draw) const;
        // The same, into `state` itself (the render worker owns the batch).
        void BindInPlace();
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
