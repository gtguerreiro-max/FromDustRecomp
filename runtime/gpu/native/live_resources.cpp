#include <chrono>
#ifdef FD_NATIVE_RENDERER
#include "../../kernel/kernel.h"
#include "live_resources.h"
#include "../xenia_bridge.h"
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
        const uint64_t serial=xenia::PhysicalWriteSerial();
        if(cached.checkedSerial!=serial){cached.checkedRevision=Revision(cached.storage);cached.checkedSerial=serial;}
        uint64_t revision=cached.checkedRevision;
        if(!cached.resource||revision!=cached.revision)
        {
            ++profileCaptured;profileCaptureBytes+=cached.storage.baseBytes+cached.storage.mipBytes;
            const auto captureStart=std::chrono::steady_clock::now();
            struct CaptureTimer{double& total;std::chrono::steady_clock::time_point start;~CaptureTimer(){total+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();}} captureTimer{profileCaptureMs,captureStart};
            Watch(cached.storage);
            // Watch before copying; a write during ownership acquisition must
            // trigger another copy rather than cache a mixed resource version.
            for(unsigned attempt=0;;++attempt)
            {
                revision=Revision(cached.storage);
                auto resource=TextureTables::CaptureResource(draw.textureFetches[slot],[](uint32_t address,uint32_t size)->std::span<const uint8_t>{
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
