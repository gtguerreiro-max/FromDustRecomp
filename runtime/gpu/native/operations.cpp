#include "operations.h"
#include "device_state.h"
#include "texture_decode.h"
#include "command_stream.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
namespace gpu::native {
namespace {
uint32_t Big(std::span<const uint8_t> bytes,size_t at) {
    if(at>bytes.size()||bytes.size()-at<4)throw std::runtime_error("Truncated native operation state");
    return uint32_t(bytes[at])<<24|uint32_t(bytes[at+1])<<16|uint32_t(bytes[at+2])<<8|bytes[at+3];
}
uint32_t Bits(float value){return std::bit_cast<uint32_t>(value);}
uint32_t BytesPerPixel(uint32_t format) {
    switch(format) {
    case 2:return 1;
    case 4:case 10:return 2;
    case 26:case 32:return 8;
    case 6:case 7:case 14:case 16:case 17:case 22:case 23:case 25:case 31:case 36:return 4;
    default:throw std::runtime_error("Unsupported partial native resolve format");
    }
}
uint64_t Tile(uint32_t x,uint32_t y,uint32_t pitch,uint32_t bytes) {
    const uint64_t outer=((uint64_t(y>>5)*(pitch>>5)+(x>>5))<<6)|(((y>>1&7)<<3)|(x&7));
    const uint64_t value=outer*bytes;
    return uint64_t(y&1)<<4|uint64_t((x>>3&3)^((y>>3&1)<<1))<<6|uint64_t(y>>4&1)<<11|
        (value&15)|((value>>4&1)<<5)|((value>>5&7)<<8)|((value>>8)<<12);
}
}
Draw SnapshotSurfaceState(std::span<const uint8_t> device,const SurfaceReader& read) {
    if(device.size()<0x31A4)throw std::runtime_error("Missing native surface bindings");
    auto result=SnapshotDeviceState(device,{0,0,0,true});
    bool sized=false;
    const Reg registers[]={RB_COLOR_INFO,RB_COLOR1_INFO,RB_COLOR2_INFO,RB_COLOR3_INFO,RB_DEPTH_INFO};
    for(uint32_t i=0;i<5;++i) {
        const auto address=Big(device,0x3190+i*4);
        if(!address)continue;
        const auto object=read(address,0x28);
        result.regs[RB_SURFACE_INFO]=Big(object,0x18);
        result.regs[registers[i]]=Big(object,0x1C);
        result.clearFlags|=i<4?1u<<i:0x30;
        if(!sized) {
            const auto size=Big(object,0x24);
            const uint32_t width=(size>>18)+1,height=(size>>3&0x1FFF)+1;
            result.regs[PA_SC_WINDOW_SCISSOR_TL]=0;result.regs[PA_SC_WINDOW_SCISSOR_BR]=width|(height<<16);
            const float viewport[]={float(width)*.5f,float(width)*.5f,-float(height)*.5f,float(height)*.5f,-1,1};
            for(uint32_t k=0;k<6;++k)result.regs[VPORT_XSCALE+k]=Bits(viewport[k]);
            sized=true;
        }
    }
    if(!sized)throw std::runtime_error("Native operation has no bound surfaces");
    return result;
}
Draw SnapshotClear(Draw source,uint32_t flags,uint32_t color,float depth,uint32_t stencil,std::span<const RenderRect> rectangles) {
    source.clear=true;source.vs="#clear";
    source.clearFlags=(flags&~0x3Fu)|(flags&source.clearFlags);source.clearColor=color;source.clearDepth=depth;source.clearStencil=stencil;
    source.clearRects.assign(rectangles.begin(),rectangles.end());
    return source;
}
CopyRegisters ParseCopyRegisters(std::span<const uint8_t> commands) {
    if(commands.size()%4)throw std::runtime_error("Unaligned native resolve command range");
    uint32_t values[4]={},written=0;
    for(const auto& packet:CommandPackets(commands)) {
        const uint32_t header=packet.header;
        const uint32_t type=header>>30;
        const uint32_t count=uint32_t(packet.payload.size()/4);
        if(type==0&&header&&header!=0x0BADF00D) {
            for(uint32_t i=0;i<count;++i) {
                const auto index=(header&0x7FFF)+(header&0x8000?0:i);
                if(index>=0x2318&&index<=0x231B){values[index-0x2318]=Big(packet.payload,i*4);written|=1u<<(index-0x2318);}
            }
            if(written==15&&values[0])return {values[0],values[1],values[2],values[3]};
        }
    }
    throw std::runtime_error("Native resolve has no complete copy register state");
}
Draw SnapshotResolve(Draw source,uint32_t flags,std::span<const uint32_t,6> fetch,const CopyRegisters& copy) {
    if(!copy.control)throw std::runtime_error("Missing native copy registers");
    const auto info=TextureInfo::FromFetch(fetch);
    const uint32_t base=copy.base&0x1FFFFFFF,dest=fetch[1]&0x1FFFF000,pitch=copy.pitch&0x3FFF;
    source.resolve=true;source.vs="#resolve";source.copyDest=dest;source.copyControl=flags;
    source.copySurface=source.regs[RB_SURFACE_INFO];source.copyColor=source.regs[RB_COLOR_INFO];source.copyDepth=source.regs[RB_DEPTH_INFO];
    source.copyDestInfo=copy.info;source.copyWidth=info.width;source.copyHeight=info.height;
    source.copyMipCount=(fetch[4]>>6&15)+1;
    const uint32_t mipBase=fetch[5]&0x1FFFF000;
    if(mipBase&&base>=mipBase&&pitch&&pitch<info.width) {
        for(uint32_t mip=1;mip<source.copyMipCount;++mip)if((std::max)(1u,info.width>>mip)==pitch&&pitch>=32) {
            source.copyMip=mip;return source;
        }
        throw std::runtime_error("Unsupported packed native mip resolve");
    }
    if(base==dest)return source;
    if(base<dest)throw std::runtime_error("Native copy precedes destination");
    const auto bytes=BytesPerPixel(info.format);const uint32_t delta=base-dest,row=pitch?pitch:info.pitch;
    if(!info.tiled) {
        source.copyY=delta/(row*bytes);const auto remainder=delta%(row*bytes);
        source.copyX=remainder/bytes;
        if(!(remainder%bytes)&&source.copyX<info.width&&source.copyY<info.height)return source;
    } else {
        for(uint32_t y=0;y<info.height;y+=32)for(uint32_t x=0;x<info.width;x+=32)
            if(Tile(x,y,(row+31)&~31u,bytes)==delta){source.copyX=x;source.copyY=y;return source;}
    }
    throw std::runtime_error("Native copy does not identify a destination tile");
}
}
