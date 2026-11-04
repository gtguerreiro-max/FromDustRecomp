// Layout rules adapted from Xenia's texture_util.cc (BSD-3-Clause).
// Copyright 2022 Ben Vanik. See third_party/xenia/LICENSE.
#include "texture_storage.h"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace gpu::native
{
namespace
{
uint32_t Align(uint32_t value,uint32_t alignment){return(value+alignment-1)&~(alignment-1);}
uint32_t CeilLog(uint32_t value){return std::bit_width(value-1);}
uint32_t Narrow(uint64_t value)
{
    if(value>0x20000000)throw std::runtime_error("Native texture storage exceeds physical memory");
    return uint32_t(value);
}
std::pair<uint32_t,uint32_t> PackedOffset(uint32_t width,uint32_t height,uint32_t level,uint32_t block)
{
    const auto w=CeilLog(width),h=CeilLog(height),shortest=(std::min)(w,h);
    if(shortest>4+level)return{0,0};
    const uint32_t first=shortest>4?shortest-4:0,relative=level-first;
    const bool wide=w>(std::max)(h,1u);
    uint32_t x=0,y=0;
    if(relative<3){if(wide)y=16>>relative;else x=16>>relative;}
    else if(wide)x=(1u<<(w-first))>>(relative-2);
    else y=(1u<<(h-first))>>(relative-2);
    return{x/block,y/block};
}
uint64_t TiledAddress(uint32_t x,uint32_t y,uint32_t pitch,uint32_t bytes)
{
    const uint64_t outer=((uint64_t(y>>5)*(pitch>>5)+(x>>5))<<6)|((y>>1&7)<<3)|(x&7);
    const uint64_t value=outer*bytes;
    return uint64_t(y&1)<<4|uint64_t((x>>3&3)^((y>>3&1)<<1))<<6|uint64_t(y>>4&1)<<11|
        (value&15)|((value>>4&1)<<5)|((value>>5&7)<<8)|((value>>8)<<12);
}
uint32_t TiledExtent(uint32_t columns,uint32_t rows,uint32_t pitch,uint32_t bytes)
{
    return Narrow(TiledAddress((columns-1)&~31u,(rows-1)&~31u,pitch,bytes)+
        (bytes==1?0xA00:bytes==2?0xC00:1024*bytes));
}
uint32_t TiledUsedBytes(uint32_t columns,uint32_t rows,uint32_t pitch,uint32_t bytes)
{
    // Addresses increase within eight-block runs, but not between runs or
    // rows. Only read texels actually used by the base, including packed offset.
    uint64_t highest=0;
    for(uint32_t y=0;y<rows;++y)
        for(uint32_t x=7;;x+=8)
        {
            const uint32_t last=(std::min)(x,columns-1);
            highest=(std::max)(highest,TiledAddress(last,y,pitch,bytes)+bytes);
            if(last==columns-1)break;
        }
    return Narrow(highest);
}
}

TextureStorage DescribeTextureStorage(std::span<const uint32_t,6> fetch)
{
    const auto info=TextureInfo::FromFetch(fetch);
    const auto block=GetTextureBlockInfo(info.format);
    if(info.width>16384||info.height>16384||(fetch[0]>>22&511)*32<info.width)
        throw std::runtime_error("Unsupported native texture dimensions or overlapping rows");
    if(info.dimension==0&&(info.tiled||(fetch[5]&0x800)))
        throw std::runtime_error("Invalid native 1D texture storage");
    if(info.dimension==2)
    {
        if((fetch[4]&0x3FC)&&(fetch[5]&0x1FFFF000))throw std::runtime_error("Native volume mip chains are unsupported");
        if((fetch[5]&0x800)&&(std::min)(info.width,info.height)<=16)
            throw std::runtime_error("Native packed volume tails are unsupported");
        const uint32_t columns=(info.width+block.texels-1)/block.texels,rowsUsed=(info.height+block.texels-1)/block.texels;
        const uint32_t pitch=Align((info.pitch+block.texels-1)/block.texels,32),rows=Align(rowsUsed,32);
        const uint64_t decoded=uint64_t(info.width)*info.height*info.depth*(info.format==25||info.format==26?16:4);
        if(decoded>512u*1024*1024)throw std::runtime_error("Native volume exceeds decoded size limit");
        uint64_t extent=0;
        if(info.tiled)
            for(uint32_t z=0;z<info.depth;++z)for(uint32_t y=0;y<rowsUsed;++y)
                for(uint32_t x=0;x<columns;++x)extent=(std::max)(extent,TextureBlockAddress3D(x,y,z,pitch,rows,block.bytes)+block.bytes);
        else extent=(uint64_t(info.depth-1)*rows+rowsUsed-1)*pitch*block.bytes+uint64_t(columns)*block.bytes;
        TextureStorage result;result.baseAddress=fetch[1]&0x1FFFF000;result.baseBytes=Narrow(extent);
        if(!result.baseAddress||uint64_t(result.baseAddress)+result.baseBytes>0x20000000)
            throw std::runtime_error("Native volume bank crosses physical memory");
        return result;
    }
    const uint32_t sizeLimit=std::bit_width((std::max)(info.width,info.height))-1;
    const bool packed=info.dimension!=0&&(fetch[5]&0x800);
    const uint32_t shortest=(std::min)(CeilLog(info.width),CeilLog(info.height));
    const uint32_t tail=packed?(shortest>4?shortest-4:0):UINT32_MAX;
    TextureStorage result;
    result.baseAddress=fetch[1]&0x1FFFF000;
    result.mipAddress=fetch[5]&0x1FFFF000;
    if(result.baseAddress&&tail==0)result.mipAddress=result.baseAddress;
    const uint32_t minimum=(std::min)(fetch[4]>>2&15,sizeLimit);
    const uint32_t maximum=result.mipAddress?(std::max)((std::min)(fetch[4]>>6&15,sizeLimit),minimum):0;
    if(maximum&&minimum&&result.baseAddress==result.mipAddress)result.baseAddress=0;
    if(!maximum)result.mipAddress=0;
    // The current FrameInput represents a base image and its complete higher
    // mip chain. A mip-only texture needs a distinct renderer view first.
    if(!result.baseAddress)throw std::runtime_error("Native mip-only texture views are unsupported");
    if(info.dimension==3&&maximum)throw std::runtime_error("Native cubemap mip chains are unsupported");
    const uint32_t faces=info.dimension==3?6:1;
    std::array<MipLayout,16> stored{};
    uint32_t offset=0;
    const uint32_t last=tail==0?uint32_t(maximum!=0):(std::min)(tail,maximum);
    for(uint32_t iteration=0;iteration<=last;++iteration)
    {
        const bool base=iteration==0;
        const uint32_t level=tail==0?0:iteration;
        const uint32_t pitchTexels=base?(fetch[0]>>22&511)*32:(std::max)(1u,std::bit_ceil(info.width)>>level);
        const uint32_t rowAlignment=!info.tiled&&!base?(std::max)(256/block.bytes,32u):32;
        const uint32_t pitch=Align((pitchTexels+block.texels-1)/block.texels,rowAlignment);
        const uint32_t height=base&&tail!=0?info.height:(std::max)(1u,std::bit_ceil(info.height)>>level);
        const uint32_t rows=info.dimension==0?1:Align((height+block.texels-1)/block.texels,32);
        const uint32_t stride=Narrow((uint64_t(pitch)*rows*block.bytes+4095)&~4095ull);
        uint32_t columnsUsed=0,rowsUsed=0;
        const uint32_t packedLast=base?0:maximum;
        if(level==tail)
        {
            for(uint32_t mip=tail;mip<=packedLast;++mip)
            {
                const auto [x,y]=PackedOffset(info.width,info.height,mip,block.texels);
                columnsUsed=(std::max)(columnsUsed,x+((std::max)(1u,info.width>>mip)+block.texels-1)/block.texels);
                rowsUsed=(std::max)(rowsUsed,y+((std::max)(1u,info.height>>mip)+block.texels-1)/block.texels);
            }
        }
        else
        {
            columnsUsed=((std::max)(1u,info.width>>level)+block.texels-1)/block.texels;
            rowsUsed=((std::max)(1u,info.height>>level)+block.texels-1)/block.texels;
        }
        const uint32_t extent=info.tiled?TiledExtent(columnsUsed,rowsUsed,pitch,block.bytes):
            Narrow(uint64_t(pitch)*block.bytes*(rowsUsed-1)+uint64_t(columnsUsed)*block.bytes);
        if(base)
        {
            // DecodeCube currently consumes six complete aligned face slices.
            result.baseBytes=faces==1?(info.tiled?TiledUsedBytes(columnsUsed,rowsUsed,pitch,block.bytes):extent):Narrow(uint64_t(stride)*faces);
        }
        else
        {
            stored[level]={pitch*block.bytes,offset,0,0};
            result.mipBytes=(std::max)(result.mipBytes,Narrow(uint64_t(offset)+extent));
            offset=Narrow(uint64_t(offset)+stride);
        }
    }
    for(uint32_t level=1;level<=maximum;++level)
    {
        auto layout=stored[(std::min)(level,tail)];
        if(packed)
        {
            const auto [x,y]=PackedOffset(info.width,info.height,level,block.texels);
            layout.xBlocks=x;layout.yBlocks=y;
        }
        result.mips.push_back(layout);
    }
    for(const auto [address,size]:{std::pair{result.baseAddress,result.baseBytes},std::pair{result.mipAddress,result.mipBytes}})
        if(uint64_t(address)+size>0x20000000)throw std::runtime_error("Native texture bank crosses physical memory");
    return result;
}

TextureSnapshot SnapshotTexture(std::span<const uint32_t,6> fetch,const TextureMemoryReader& read)
{
    TextureSnapshot snapshot;
    std::copy(fetch.begin(),fetch.end(),snapshot.fetch.begin());
    snapshot.storage=DescribeTextureStorage(fetch);
    auto copy=[&](uint32_t address,uint32_t size,std::vector<uint8_t>& destination)
    {
        if(!size)return;
        const auto bytes=read(address,size);
        if(bytes.size()<size)throw std::runtime_error("Unreadable native texture bank");
        destination.assign(bytes.begin(),bytes.begin()+size);
    };
    if(snapshot.storage.mipBytes&&snapshot.storage.baseAddress==snapshot.storage.mipAddress)
    {
        // Packed level zero and its mips overlap. Own one memory version.
        std::vector<uint8_t> shared;
        copy(snapshot.storage.baseAddress,(std::max)(snapshot.storage.baseBytes,snapshot.storage.mipBytes),shared);
        snapshot.base.assign(shared.begin(),shared.begin()+snapshot.storage.baseBytes);
        snapshot.mips.assign(shared.begin(),shared.begin()+snapshot.storage.mipBytes);
    }
    else
    {
        copy(snapshot.storage.baseAddress,snapshot.storage.baseBytes,snapshot.base);
        copy(snapshot.storage.mipAddress,snapshot.storage.mipBytes,snapshot.mips);
    }
    return snapshot;
}
}
