#include "texture_decode.h"
#include "texture_tables.h"
#include "texture_storage.h"
#include "renderer.h"
#include "replay_frame.h"
#include "agility_exports.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/xenia/third_party/stb/stb_image.h"

using namespace gpu::native;
using Bytes=std::vector<uint8_t>;
Bytes Read(const std::string& path)
{
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file)throw std::runtime_error("Missing texture fixture: "+path);
    const auto size=file.tellg();file.seekg(0);Bytes data(static_cast<size_t>(size));
    if(!file.read(reinterpret_cast<char*>(data.data()),data.size()))throw std::runtime_error("Truncated texture fixture");return data;
}
struct Reader
{
    std::span<const uint8_t> data;
    std::span<const uint8_t> Take(size_t size){if(size>data.size())throw std::runtime_error("Truncated texture record");auto result=data.first(size);data=data.subspan(size);return result;}
    uint32_t Word(){auto b=Take(4);uint32_t result;memcpy(&result,b.data(),4);return result;}
};
struct Source{std::array<uint32_t,6> fetch;std::span<const uint8_t> bytes;};
void Equal(const DecodedTexture& actual,uint32_t width,uint32_t height,bool floats,std::span<const uint8_t> expected,const char* kind,uint32_t index)
{
    if(actual.width!=width||actual.height!=height||actual.floatPixels!=floats||actual.pixels.size()!=expected.size()||memcmp(actual.pixels.data(),expected.data(),expected.size()))
    {
        fprintf(stderr,"%s %u: %ux%u/%ux%u float %u/%u bytes %zu/%zu\n",kind,index,actual.width,actual.height,width,height,actual.floatPixels,floats,actual.pixels.size(),expected.size());
        for(size_t b=0;b<actual.pixels.size()&&b<expected.size();++b)if(actual.pixels[b]!=expected[b]){fprintf(stderr,"first pixel byte %zu: %u/%u\n",b,actual.pixels[b],expected[b]);break;}
        throw std::runtime_error("Native texture differs from validated replay");
    }
}
int main(int argc,char**argv)
{
    const bool owned=argc>2&&std::string(argv[1])=="--render-owned";
    const bool render=owned||(argc>2&&std::string(argv[1])=="--render");
    const int first=render?3:1;
    if(argc<first+2||argc%2!=1)return 2;
    try
    {
        uint64_t textures=0,cubes=0,mips=0;
        Renderer renderer;
        if(render&&!renderer.LoadShaderCatalog(argv[2]))return 1;
        for(int path=first;path<argc;path+=2)
        {
            const std::string capture=argv[path],replay=argv[path+1];
            auto sourceBytes=Read(capture+"/gpu_textures.bin");Reader raw{sourceBytes};
            std::vector<Source> sources;std::map<uint32_t,uint32_t> indices;
            while(!raw.data.empty())
            {
                if(raw.Word()!=0x46445458)throw std::runtime_error("Bad texture capture magic");
                Source source;for(auto& word:source.fetch)word=raw.Word();const auto size=raw.Word();source.bytes=raw.Take(size);
                const uint32_t base=source.fetch[1]&0xFFFFF000;
                auto found=indices.find(base);
                if(found==indices.end()){indices[base]=uint32_t(sources.size());sources.push_back(source);}else sources[found->second]=source;
            }
            auto expectedBytes=Read(replay+"/textures.rgba");Reader expected{expectedBytes};
            TextureTables tables;
            if(expected.Word()!=sources.size())throw std::runtime_error("Texture table count mismatch");
            for(uint32_t index=0;index<sources.size();++index)
            {
                const uint32_t storedWidth=expected.Word(),height=expected.Word(),width=storedWidth&0x7FFFFFFF;
                const bool floats=storedWidth>>31;const auto pixels=expected.Take(size_t(width)*height*(floats?16:4));
                const auto storage=DescribeTextureStorage(sources[index].fetch);
                if(storage.baseBytes>sources[index].bytes.size())
                {
                    fprintf(stderr,"%s base %u: native %u / captured %zu bytes (%ux%u, format %u)\n",capture.c_str(),index,storage.baseBytes,sources[index].bytes.size(),width,height,sources[index].fetch[1]&63);
                    throw std::runtime_error("Native base extent exceeds captured bank");
                }
                const auto base=sources[index].bytes.first(storage.baseBytes);
                auto result=DecodeTexture(sources[index].fetch,base,false);
                Equal(result,width,height,floats,pixels,"texture",index);++textures;
                if(tables.Add(sources[index].fetch,base)!=index)throw std::runtime_error("Native resource index changed");
            }
            auto cubeBytes=Read(replay+"/cubes.rgba");Reader cube{cubeBytes};
            for(uint32_t count=cube.Word();count;--count)
            {
                const auto index=cube.Word(),width=cube.Word(),height=cube.Word();const auto wanted=cube.Take(size_t(width)*height*4*6);
                auto actual=DecodeCube(sources[index].fetch,sources[index].bytes);
                if(actual.size()!=wanted.size()||memcmp(actual.data(),wanted.data(),wanted.size()))throw std::runtime_error("Native cube faces differ from validated replay");++cubes;
            }
            auto mipBytes=Read(replay+"/mips.rgba");Reader mip{mipBytes};
            struct ExpectedMip{uint32_t width,height;std::span<const uint8_t> bytes;};
            std::map<uint32_t,std::vector<ExpectedMip>> levels;
            for(uint32_t count=mip.Word();count;--count)
            {
                const uint32_t index=mip.Word(),number=mip.Word();
                for(uint32_t level=0;level<number;++level){const uint32_t width=mip.Word(),height=mip.Word();levels[index].push_back({width,height,mip.Take(size_t(width)*height*4)});}
            }
            auto bankBytes=Read(capture+"/gpu_mips.bin");Reader bank{bankBytes};
            if(bank.Word()!=0x31504D46)throw std::runtime_error("Bad mip capture magic");
            const uint32_t mipRecords=bank.Word();bank.Take(8);
            std::map<uint32_t,Source> mipSources;
            for(uint32_t record=0;record<mipRecords;++record)
            {
                const uint32_t base=bank.Word();std::array<uint32_t,6> fetch;for(auto& word:fetch)word=bank.Word();
                const uint32_t number=bank.Word(),size=bank.Word();std::vector<MipLayout> layouts;
                for(uint32_t level=0;level<number;++level)layouts.push_back({bank.Word(),bank.Word(),bank.Word(),bank.Word()});
                const auto bytes=bank.Take(size);const auto index=indices.find(base);
                mipSources[base]={fetch,bytes};
                const auto storage=DescribeTextureStorage(fetch);
                if(storage.mipBytes!=size||storage.mips.size()!=number)
                    throw std::runtime_error("Native mip storage extent differs from GPU reference");
                for(uint32_t level=0;level<number;++level)
                    if(memcmp(&storage.mips[level],&layouts[level],sizeof(MipLayout)))
                        throw std::runtime_error("Native mip layout differs from GPU reference");
                if(index==indices.end()||(fetch[1]&63)==25||(fetch[1]&63)==26)continue;
                const auto& wanted=levels.at(index->second);
                const auto snapshot=SnapshotTexture(fetch,[&](uint32_t address,uint32_t)->std::span<const uint8_t>{
                    // Small packed tails can share the same base and mip bank.
                    if(address==storage.mipAddress)return bytes;
                    if(address==storage.baseAddress)return sources[index->second].bytes;
                    return {};
                });
                tables.Mips(index->second,fetch,snapshot.mips,snapshot.storage.mips);
                if(wanted.size()!=number)throw std::runtime_error("Mip level count mismatch");
                for(uint32_t level=0;level<number;++level)
                {
                    auto actual=DecodeMip(fetch,snapshot.mips,level+1,snapshot.storage.mips[level]);
                    Equal(actual,wanted[level].width,wanted[level].height,false,wanted[level].bytes,"mip",level+1);++mips;
                }
            }
            const auto packed=tables.Pack();
            if(packed.textures!=expectedBytes||packed.cubes!=cubeBytes||packed.mips!=mipBytes)
                throw std::runtime_error("Native packed resource tables differ from validated replay");
            // Exercise the production memory-reader entry point for every base,
            // cube and mip bank, without passing it any captured layouts.
            TextureTables memoryTables;
            for(uint32_t index=0;index<sources.size();++index)
            {
                const auto& source=sources[index];
                const auto base=source.fetch[1]&0x1FFFF000;
                const auto higher=mipSources.find(base);
                const bool hasMips=levels.contains(index);
                if(hasMips&&higher==mipSources.end())throw std::runtime_error("Missing native memory mip bank");
                const auto& fetch=hasMips?higher->second.fetch:source.fetch;
                const auto storage=DescribeTextureStorage(fetch);
                auto reader=[&](uint32_t address,uint32_t)->std::span<const uint8_t>{
                    if(storage.mipBytes&&address==storage.mipAddress&&higher!=mipSources.end())return higher->second.bytes;
                    if(address==storage.baseAddress)return source.bytes;
                    return {};
                };
                if(memoryTables.Capture(fetch,reader)!=index)throw std::runtime_error("Native memory resource index changed");
            }
            const auto memoryPacked=memoryTables.Pack();
            if(memoryPacked.textures!=expectedBytes||memoryPacked.cubes!=cubeBytes)
                throw std::runtime_error("Native memory resources differ from validated replay");
            Reader memoryMips{memoryPacked.mips};
            if(memoryMips.Word()!=levels.size())throw std::runtime_error("Native memory mip table count differs");
            for(size_t record=0;record<levels.size();++record)
            {
                const auto index=memoryMips.Word(),count=memoryMips.Word();
                const auto& wanted=levels.at(index);
                if(count!=wanted.size())throw std::runtime_error("Native memory mip chain length differs");
                for(uint32_t level=0;level<count;++level)
                {
                    const auto width=memoryMips.Word(),height=memoryMips.Word();
                    const auto bytes=memoryMips.Take(size_t(width)*height*4);
                    if(width!=wanted[level].width||height!=wanted[level].height||bytes.size()!=wanted[level].bytes.size()||
                        memcmp(bytes.data(),wanted[level].bytes.data(),bytes.size()))
                        throw std::runtime_error("Native memory mip pixels differ");
                }
            }
            if(!memoryMips.data.empty())throw std::runtime_error("Unexpected native memory mip table data");
            if(render)
            {
                auto frame=ReplayFrame::Load(replay);auto view=frame.View();
                view.textures=memoryPacked.textures;view.cubes=memoryPacked.cubes;view.mips=memoryPacked.mips;
                if(owned){view.textures={};view.cubes={};view.mips={};view.resources=memoryTables.Resources();}
                FrameOutput output;
                if(renderer.RenderFrame(view,{},&output))return 1;
                int width,height,channels;
                auto* image=stbi_load((replay+"/frame.png").c_str(),&width,&height,&channels,3);
                if(!image)throw std::runtime_error("Missing baseline PNG");
                const bool equal=output.width==width&&output.height==height&&output.rgb.size()==size_t(width)*height*3&&
                    !memcmp(output.rgb.data(),image,output.rgb.size());stbi_image_free(image);
                if(!equal)throw std::runtime_error("Native texture reconstruction changed rendered RGB");
            }
        }
        printf("Native textures: %llu bases, %llu cubes and %llu mip levels exactly match validated replay pixels\n",textures,cubes,mips);
        if(render)printf("Native rebuilt resource frames: %llu frames, %llu draws, exact baseline RGB\n",renderer.CompletedFrames(),renderer.CompletedDraws());
        return 0;
    }
    catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}
}
