#include "frame_builder.h"
#include <stdexcept>

namespace gpu::native
{
void GeometryBatch::Command::BindGeometryAndConstants(Draw& draw)const
{
    draw=state;
    if(draw.clear||draw.resolve)return;
    geometry.Bind(draw);draw.vs=vertexShader;draw.ps=pixelShader;
    draw.constants=reinterpret_cast<const uint8_t*>(vertexConstants.data());
    draw.psConstants=reinterpret_cast<const uint8_t*>(pixelConstants.data());
}
void GeometryBatch::Command::BindInPlace()
{
    if(state.clear||state.resolve)return;
    geometry.Bind(state);state.vs=vertexShader;state.ps=pixelShader;
    state.constants=reinterpret_cast<const uint8_t*>(vertexConstants.data());
    state.psConstants=reinterpret_cast<const uint8_t*>(pixelConstants.data());
}
std::vector<Draw> BuildFrameDraws(const GeometryBatch& batch,const ShaderCatalog& shaders,
    const TextureTables& textures,const std::map<uint32_t,uint32_t>& resources)
{
    std::vector<Draw> result;result.reserve(batch.commands.size());
    for(const auto& command:batch.commands)
    {
        Draw draw;command.BindGeometryAndConstants(draw);
        if(!draw.clear&&!draw.resolve)
        {
            const auto* vertex=shaders.Find(draw.vs,ShaderStage::Vertex);
            const auto* pixel=draw.ps.empty()?nullptr:shaders.Find(draw.ps,ShaderStage::Pixel);
            if(!vertex||(!draw.ps.empty()&&!pixel))throw std::runtime_error("Native frame references an unknown shader");
            const uint32_t mask=vertex->textureMask|(pixel?pixel->textureMask:0);
            for(uint32_t slot=0;slot<32;++slot)
            {
                draw.slots[slot]=-1;
                if(!(mask>>slot&1)||(draw.textureFetches[slot][0]&3)!=2||!draw.bases[slot])continue;
                const auto found=resources.find(draw.bases[slot]);
                if(found==resources.end())throw std::runtime_error("Missing owned native texture at slot "+std::to_string(slot)+" in VS "+draw.vs+" PS "+draw.ps);
                textures.Bind(draw,slot,found->second);
            }
        }
        result.push_back(std::move(draw));
    }
    return result;
}
}
