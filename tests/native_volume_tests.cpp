#include "renderer.h"
#include "texture_tables.h"
#include "replay_frame.h"
#include "agility_exports.h"
#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include <source_location>
using namespace gpu::native;
using Bytes=std::vector<uint8_t>;
void Check(bool value,const std::source_location at=std::source_location::current()){if(!value)throw std::runtime_error("Native volume rendering regression at line "+std::to_string(at.line()));}
Bytes Read(const char* path){std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};}
int main(int argc,char** argv)
{
    if(argc!=4)return 2;
    try
    {
        std::filesystem::create_directories(argv[1]);
        Bytes catalog;
        auto word=[&](uint32_t value){for(unsigned b=0;b<4;++b)catalog.push_back(uint8_t(value>>(b*8)));};
        word(0x43534446);word(1);word(2);
        for(unsigned stage=0;stage<2;++stage)
        {
            const auto dxil=Read(argv[2+stage]);Check(dxil.size()>4);
            const std::string name=stage?"volume_ps":"volume_vs";
            const std::string source="#define sample_Texture2DDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 0)\nvoid main(){sample_Texture3DDescriptorIndex;} // SV_Target0";
            word(uint32_t(name.size()));word(stage);word(36);word(4);word(uint32_t(source.size()));word(uint32_t(dxil.size()));
            catalog.insert(catalog.end(),name.begin(),name.end());
            std::array<uint8_t,36> metadata{};metadata[0]=0x10;metadata[1]=0x2A;metadata[2]=0x11;metadata[3]=uint8_t(stage?0:1);metadata[7]=36;metadata[11]=4;
            catalog.insert(catalog.end(),metadata.begin(),metadata.end());word(stage);
            catalog.insert(catalog.end(),source.begin(),source.end());catalog.insert(catalog.end(),dxil.begin(),dxil.end());
        }
        const auto path=std::filesystem::path(argv[1])/"catalog.bin";
        {std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(catalog.data()),catalog.size());Check(bool(file));}
        Renderer renderer;Check(renderer.LoadShaderCatalog(path.string()));
        auto resource=std::make_shared<TextureResource>();resource->image={4,4,false,{}};resource->volumeDepth=3;
        for(uint32_t z=0;z<3;++z)for(uint32_t y=0;y<4;++y)for(uint32_t x=0;x<4;++x)
            for(uint8_t c:{uint8_t(x*31+7),uint8_t(y*47+13),uint8_t(z*67+19),uint8_t(255)})resource->volume.push_back(c);
        resource->image.pixels.assign(resource->volume.begin(),resource->volume.begin()+64);
        TextureTables tables;tables.AddResource(resource);
        std::array<uint16_t,3> indices{0,1,2};
        std::array<float,12> vertices{-1,-1,0,1,3,-1,0,1,-1,3,0,1};
        std::array<uint8_t,4096> constants{};std::array<uint8_t,512> fetch{};
        Draw draw;draw.vs="volume_vs";draw.ps="volume_ps";draw.primitive=4;draw.indexCount=3;draw.stride=16;
        draw.indices=reinterpret_cast<const uint8_t*>(indices.data());draw.indexBytes=sizeof(indices);
        draw.vertices=reinterpret_cast<const uint8_t*>(vertices.data());draw.vertexBytes=sizeof(vertices);draw.semantics={"POSITION0"};
        draw.constants=draw.psConstants=constants.data();draw.vertexFetch=fetch.data();
        draw.regs[RB_SURFACE_INFO]=128;draw.regs[PA_SC_WINDOW_SCISSOR_BR]=12|(4<<16);draw.regs[RB_COLOR_MASK]=15;draw.regs[RB_BLENDCONTROL0]=0x10001;
        draw.hasContext=draw.hasFetchMetadata=draw.hasSamplerStates=draw.hasPixelCenter=draw.hasBlendConstants=draw.hasStencilState=true;
        draw.textureFetches[0][5]=2<<9;draw.swizzles[0]=0x688;draw.slots[0]=0;draw.fetchFormats[0]=6;
        Draw copy=draw;copy.resolve=true;copy.copyDest=0x1000;copy.copySurface=128;copy.copyWidth=12;copy.copyHeight=4;copy.copyDestInfo=6<<7;
        std::array<Draw,2> draws{draw,copy};
        FrameInput input;input.draws=draws;input.resources=tables.Resources();input.continuous=true;
        auto verify=[&](FrameInput frame,bool swapped)
        {
            FrameOutput output;Check(renderer.RenderFrame(frame,{},&output)==0&&output.width==12&&output.height==4&&output.debugErrors==0);
            for(uint32_t y=0;y<4;++y)for(uint32_t x=0;x<12;++x)
            {
                const size_t source=(((x/4)*4+y)*4+x%4)*4,target=(y*12+x)*3;
                for(uint32_t c=0;c<3;++c)if(output.rgb[target+c]!=resource->volume[source+(swapped?2-c:c)])
                {
                    fprintf(stderr,"pixel %u,%u channel %u: actual %u expected %u (swapped %u)\n",x,y,c,output.rgb[target+c],resource->volume[source+(swapped?2-c:c)],swapped);Check(false);
                }
            }
        };
        verify(input,false);verify(input,false); // persistent upload reuse
        draws[0].swizzles[0]=0x60A;verify(input,true);
        const auto replayPath=(std::filesystem::path(argv[1])/"frame").string();ReplayFrame::Save(input,replayPath);
        auto saved=ReplayFrame::Load(replayPath);auto savedView=saved.View();Check(!savedView.volumes.empty());verify(savedView,true);
        savedView.volumes=savedView.volumes.first(savedView.volumes.size()-1);FrameOutput bad;Check(renderer.RenderFrame(savedView,{},&bad)!=0&&bad.rgb.empty());
        puts("Native volumes: all slice pixels, padded upload rows, reuse, swizzle, saved replay and truncation rejection passed");return 0;
    }
    catch(const std::exception& error){fprintf(stderr,"%s\n",error.what());return 1;}
}
