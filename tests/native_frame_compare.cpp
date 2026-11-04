// Diagnostic state substitution on a frame emitted by the live CPU producer.
#include "renderer.h"
#include "replay_frame.h"
#include "agility_exports.h"
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <cstdio>

using namespace gpu::native;
int main(int argc,char** argv)
{
    if(argc<6)return 2;
    try
    {
        std::set<std::string> options;for(int at=6;at<argc;++at)options.insert(argv[at]);
        auto native=ReplayFrame::Load(argv[2]),reference=ReplayFrame::Load(argv[3]);
        auto view=native.View();const auto other=reference.View();
        std::ifstream mapFile(argv[4]);if(!mapFile)throw std::runtime_error("Missing native/reference record map");
        std::map<uint32_t,uint32_t> mapping;uint32_t a,b;
        while(mapFile>>a>>b)if(!mapping.emplace(a,b).second)throw std::runtime_error("Duplicate native record mapping");
        if(!mapFile.eof()||mapping.size()!=other.draws.size())throw std::runtime_error("Incomplete native record mapping");
        std::filesystem::create_directories(argv[5]);
        FILE* report=fopen((std::filesystem::path(argv[5])/"input-differences.txt").string().c_str(),"wb");
        if(!report)throw std::runtime_error("Cannot write frame difference report");
        std::vector<Draw> draws;
        for(uint32_t record=0;record<view.draws.size();++record)
        {
            auto draw=view.draws[record];const auto found=mapping.find(record);
            if(found==mapping.end())
            {
                fprintf(report,"native %u unmatched: VS %s PS %s clear=%u resolve=%u\n",record,draw.vs.c_str(),draw.ps.c_str(),draw.clear,draw.resolve);
                if(!options.contains("--omit-unmatched"))draws.push_back(draw);
                continue;
            }
            const auto& expected=other.draws[found->second];
            if(draw.clear!=expected.clear||draw.resolve!=expected.resolve)throw std::runtime_error("Mapped frame operation kinds differ");
            fprintf(report,"native %u / reference %u: VS %s/%s PS %s/%s\n",record,found->second,draw.vs.c_str(),expected.vs.c_str(),draw.ps.c_str(),expected.ps.c_str());
            if(draw.clear||draw.resolve)
            {
                if(options.contains("--gpu-operations"))draw=expected;
                draws.push_back(std::move(draw));continue;
            }
            for(uint32_t reg=0;reg<kRegCount;++reg)if(draw.regs[reg]!=expected.regs[reg])
                fprintf(report,"  reg %u: %08X / %08X\n",reg,draw.regs[reg],expected.regs[reg]);
            for(uint32_t reg=0;reg<40;++reg)if(draw.boolLoop[reg]!=expected.boolLoop[reg])
                fprintf(report,"  bool/loop %u: %08X / %08X\n",reg,draw.boolLoop[reg],expected.boolLoop[reg]);
            for(uint32_t stage=0;stage<2;++stage)
            {
                const auto* actual=stage?draw.psConstants:draw.constants;
                const auto* wanted=stage?expected.psConstants:expected.constants;
                unsigned changed=0,printed=0;
                for(uint32_t at=0;at<1024;++at)
                {
                    uint32_t value,otherValue;memcpy(&value,actual+at*4,4);memcpy(&otherValue,wanted+at*4,4);
                    if(value==otherValue)continue;
                    ++changed;
                    if(printed++<12)fprintf(report,"  %s float c%u.%u: %08X / %08X\n",stage?"PS":"VS",at/4,at%4,value,otherValue);
                }
                if(changed)fprintf(report,"  %s changed constant words: %u\n",stage?"PS":"VS",changed);
            }
            if(memcmp(draw.textureFetches,expected.textureFetches,sizeof(draw.textureFetches)))fprintf(report,"  texture fetch metadata differs\n");
            if(options.contains("--gpu-state"))
            {
                const auto geometry=draw;
                draw=expected;
                draw.vertices=geometry.vertices;draw.vertexBytes=geometry.vertexBytes;draw.stride=geometry.stride;
                draw.indices=geometry.indices;draw.indexBytes=geometry.indexBytes;draw.indexCount=geometry.indexCount;draw.index32=geometry.index32;
                draw.semantics=geometry.semantics;draw.vertexFetch=geometry.vertexFetch;
                draw.constants=geometry.constants;draw.psConstants=geometry.psConstants;
            }
            if(options.contains("--gpu-constants")){draw.constants=expected.constants;draw.psConstants=expected.psConstants;}
            if(options.contains("--gpu-geometry"))
            {
                draw.vertices=expected.vertices;draw.vertexBytes=expected.vertexBytes;draw.stride=expected.stride;
                draw.indices=expected.indices;draw.indexBytes=expected.indexBytes;draw.indexCount=expected.indexCount;draw.index32=expected.index32;
                draw.semantics=expected.semantics;draw.vertexFetch=expected.vertexFetch;
            }
            draws.push_back(std::move(draw));
        }
        fclose(report);
        view.draws=draws;
        if(options.contains("--gpu-geometry"))view.streams=other.streams;
        Renderer renderer;if(!renderer.LoadShaderCatalog(argv[1]))return 1;
        return renderer.RenderFrame(view,argv[5]);
    }
    catch(const std::exception& error){fprintf(stderr,"Native frame comparison: %s\n",error.what());return 1;}
}
