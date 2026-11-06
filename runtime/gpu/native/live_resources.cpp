#include <chrono>
#ifdef FD_NATIVE_RENDERER
#include "../../kernel/kernel.h"
#include "live_resources.h"
#include <cstring>
#include <string>
#include <map>
#include "../xenia_bridge.h"
#include "../../input.h"
#include <algorithm>
#include <stdexcept>

namespace gpu::native
{
namespace
{
void Watch(const TextureStorage& storage)
{
    xenia::WatchPhysical(storage.baseAddress,storage.baseBytes);
    if(storage.mipBytes)xenia::WatchPhysical(storage.mipAddress,storage.mipBytes);
}
uint64_t Revision(const TextureStorage& storage)
{
    return (std::max)(xenia::PhysicalRevision(storage.baseAddress,storage.baseBytes),
        xenia::PhysicalRevision(storage.mipAddress,storage.mipBytes));
}
}
void LiveResources::BeginFrame(){tables={};frameIndices.clear();profileInserted=profileCaptured=profileCaptureBytes=0;profileCaptureMs=0;}
void LiveResources::Bind(Draw& draw,const ShaderCatalog& shaders)
{
    const auto* vs=shaders.Find(draw.vs,ShaderStage::Vertex);
    const auto* ps=draw.ps.empty()?nullptr:shaders.Find(draw.ps,ShaderStage::Pixel);
    if(!vs||(!draw.ps.empty()&&!ps))throw std::runtime_error("Unregistered native resource shader");
    const uint32_t mask=vs->textureMask|(ps?ps->textureMask:0);
    for(uint32_t slot=0;slot<32;++slot)
    {
        draw.slots[slot]=-1;
        if(!(mask>>slot&1)||(draw.textureFetches[slot][0]&3)!=2||!draw.bases[slot])continue;
        std::array<uint32_t,6> key;
        std::copy_n(draw.textureFetches[slot],6,key.begin());
        // Filtering, clamp, swizzle and exponent adjustment are per draw.
        key[0]&=0xFFC003FF;key[1]&=0x1FFFF4FF;key[3]&=0x7E001;
        key[4]&=0x3FC;key[5]&=0x1FFFFE00;
        auto [at,inserted]=resources.try_emplace(key);
        auto& cached=at->second;
        if(inserted){cached.storage=DescribeTextureStorage(draw.textureFetches[slot]);Watch(cached.storage);++profileInserted;}
        const uint64_t serial=xenia::PhysicalWriteGeneration();
        if(cached.checkedSerial!=serial){cached.checkedRevision=Revision(cached.storage);cached.checkedSerial=serial;}
        uint64_t revision=cached.checkedRevision;
        // The prompt atlas is decoded again when the prompt style changes
        // (keyboard / controller glyphs, rebound keys).
        const int promptGeneration=cached.resource?ResolvedGeneration(*cached.resource):-1;
        const bool promptStale=promptGeneration>=0&&uint32_t(promptGeneration)!=(pcinput::PromptGeneration()&0x7FFFFFFF);
        if(!cached.resource||revision!=cached.revision||promptStale)
        {
            ++profileCaptured;profileCaptureBytes+=cached.storage.baseBytes+cached.storage.mipBytes;
            // FD_TRACE_TEXTURE_CAPTURES=1: every 10 s, the textures captured
            // again most often, and how often their bytes were unchanged.
            static const bool traceCaptures=getenv("FD_TRACE_TEXTURE_CAPTURES")!=nullptr;
            if(traceCaptures)
            {
                struct Seen{uint64_t captures=0,same=0,hash=0;uint32_t bytes=0,format=0,width=0,height=0;};
                static std::map<uint32_t,Seen> seen;
                static ULONGLONG since=GetTickCount64();
                auto& entry=seen[cached.storage.baseAddress];
                const uint8_t* data=mem::Ptr<uint8_t>(mem::kPhysicalBase|cached.storage.baseAddress);
                uint64_t hash=1469598103934665603ull;
                for(uint32_t at=0;at+8<=cached.storage.baseBytes;at+=8){uint64_t word;memcpy(&word,data+at,8);hash=(hash^word)*1099511628211ull;}
                entry.same+=entry.captures&&hash==entry.hash;
                ++entry.captures;entry.hash=hash;entry.bytes=cached.storage.baseBytes+cached.storage.mipBytes;
                entry.format=draw.textureFetches[slot][1]&0x3F;
                entry.width=(draw.textureFetches[slot][2]&0x1FFF)+1;entry.height=(draw.textureFetches[slot][2]>>13&0x1FFF)+1;
                if(GetTickCount64()-since>=10000)
                {
                    std::vector<std::pair<uint64_t,uint32_t>> top;
                    for(const auto& [address,value]:seen)top.push_back({value.captures,address});
                    std::sort(top.rbegin(),top.rend());
                    std::string text;
                    char item[128];
                    for(size_t i=0;i<top.size()&&i<10;++i)
                    {
                        const auto& v=seen[top[i].second];
                        snprintf(item,sizeof(item)," %08X %ux%u fmt %u %u KB: %llu captures, %llu unchanged;",top[i].second,v.width,v.height,v.format,v.bytes/1024,
                            (unsigned long long)v.captures,(unsigned long long)v.same);
                        text+=item;
                    }
                    kernel::Log("texture captures (10 s):%s",text.c_str());
                    seen.clear();since=GetTickCount64();
                }
            }
            const auto captureStart=std::chrono::steady_clock::now();
            struct CaptureTimer{double& total;std::chrono::steady_clock::time_point start;~CaptureTimer(){total+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();}} captureTimer{profileCaptureMs,captureStart};
            Watch(cached.storage);
            // Watch before copying; a write during ownership acquisition must
            // trigger another copy rather than cache a mixed resource version.
            for(unsigned attempt=0;;++attempt)
            {
                revision=Revision(cached.storage);
                auto resource=TextureTables::CaptureDeferred(draw.textureFetches[slot],[](uint32_t address,uint32_t size)->std::span<const uint8_t>{
                    if(uint64_t(address)+size>0x20000000)return {};
                    return {mem::Ptr<uint8_t>(mem::kPhysicalBase|address),size};
                });
                if(Revision(cached.storage)==revision){cached.resource=std::move(resource);cached.revision=revision;cached.checkedSerial=UINT64_MAX;break;}
                if(attempt==3)throw std::runtime_error("CPU texture changed repeatedly while acquiring native ownership");
                Watch(cached.storage);
            }
        }
        auto [index,fresh]=frameIndices.try_emplace(cached.resource.get(),0);
        if(fresh)index->second=tables.AddResource(cached.resource);
        tables.Bind(draw,slot,index->second);
        const auto authority=gpuAuthority.find(draw.bases[slot]);
        if(authority==gpuAuthority.end()||revision>authority->second)draw.cpuTextureMask|=1u<<slot;
    }
}
void LiveResources::Resolve(std::span<const uint32_t,6> fetch)
{
    Watch(DescribeTextureStorage(fetch));
    gpuAuthority[fetch[1]&0x1FFFF000]=xenia::PhysicalWriteSerial();
}
}
#endif
