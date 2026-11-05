#include "texture_decode.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <future>
#include <thread>

namespace gpu::native
{
namespace
{
struct Storage { uint32_t bytes,block; };
Storage Format(uint32_t format)
{
    switch(format)
    {
    case 18:return{8,4};
    case 19:case 20:case 49:return{16,4};
    case 2:return{1,1};
    case 4:case 10:return{2,1};
    case 26:case 32:return{8,1};
    case 6:case 7:case 14:case 16:case 17:case 22:case 23:case 25:case 31:case 36:return{4,1};
    default:throw std::runtime_error("Unsupported native texture format");
    }
}
uint64_t Tiled(uint32_t x,uint32_t y,uint32_t pitch,uint32_t bytes)
{
    if(pitch&31)throw std::runtime_error("Unaligned tiled texture pitch");
    const uint64_t outer=((uint64_t(y>>5)*(pitch>>5)+(x>>5))<<6)|(((y>>1&7)<<3)|(x&7));
    const uint64_t value=outer*bytes;
    const uint32_t bank=y>>4&1,pipe=(x>>3&3)^((y>>3&1)<<1);
    return uint64_t(y&1)<<4|uint64_t(pipe)<<6|uint64_t(bank)<<11|(value&15)|((value>>4&1)<<5)|((value>>5&7)<<8)|((value>>8)<<12);
}
struct Raw
{
    std::span<const uint8_t> bytes;
    uint32_t endian;
    uint8_t Byte(uint64_t offset)const
    {
        const uint32_t group=endian==1?2:4;
        if(endian && (offset&~uint64_t(group-1))+group<=bytes.size())offset^=endian==1?1:endian==2?3:2;
        if(offset>=bytes.size())throw std::runtime_error("Texture texel outside supplied storage");
        return bytes[size_t(offset)];
    }
    uint32_t Word(uint64_t offset,uint32_t count=4)const
    {
        // Fast path: every byte's endian group lies inside the storage, so
        // Byte's per-byte bounds checks reduce to one test (same mapping).
        if(offset+count+4<=bytes.size())
        {
            const uint32_t swap=endian==1?1:endian==2?3:endian==3?2:0;
            uint32_t value=0;
            for(uint32_t i=0;i<count;++i)value|=uint32_t(bytes[size_t((offset+i)^swap)])<<(i*8);
            return value;
        }
        uint32_t value=0;for(uint32_t i=0;i<count;++i)value|=uint32_t(Byte(offset+i))<<(i*8);return value;
    }
};
uint8_t Unorm(uint32_t value,uint32_t bits)
{
    const uint64_t maximum=(uint64_t(1)<<bits)-1;
    return uint8_t((value*255ull+maximum/2)/maximum);
}
uint8_t Clamp(double value)
{
    if(std::isnan(value)||value<=0)return 0;
    if(value>=1)return 255;
    return uint8_t(value*255.0+0.5);
}
double Half(uint32_t bits)
{
    const bool sign=bits&0x8000;const uint32_t exponent=bits>>10&31,mantissa=bits&1023;
    double value;
    if(exponent==31)value=mantissa?NAN:INFINITY;
    else value=exponent?std::ldexp(1.0+mantissa/1024.0,int(exponent)-15):std::ldexp(double(mantissa),-24);
    return sign?-value:value;
}
using Pixel=std::array<uint8_t,4>;
Pixel Color565(uint32_t value)
{
    const uint32_t r=value>>11&31,g=value>>5&63,b=value&31;
    return{uint8_t(r<<3|r>>2),uint8_t(g<<2|g>>4),uint8_t(b<<3|b>>2),255};
}
std::array<uint8_t,16> Channel(const Raw& raw,uint64_t at)
{
    const uint8_t a=raw.Byte(at),b=raw.Byte(at+1);std::array<uint8_t,8> palette{a,b};
    if(a>b)for(uint32_t k=1;k<7;++k)palette[k+1]=uint8_t(((7-k)*a+k*b)/7);
    else
    {
        for(uint32_t k=1;k<5;++k)palette[k+1]=uint8_t(((5-k)*a+k*b)/5);
        palette[6]=0;palette[7]=255;
    }
    uint64_t indices=0;for(uint32_t i=0;i<6;++i)indices|=uint64_t(raw.Byte(at+2+i))<<(8*i);
    std::array<uint8_t,16> result;for(uint32_t i=0;i<16;++i)result[i]=palette[indices>>(3*i)&7];return result;
}
std::array<Pixel,16> Block(const Raw& raw,uint64_t at,uint32_t format)
{
    std::array<Pixel,16> result;
    if(format==49)
    {
        const auto r=Channel(raw,at),g=Channel(raw,at+8);
        for(uint32_t i=0;i<16;++i)result[i]={r[i],g[i],0,255};
        return result;
    }
    const uint64_t color=at+(format==18?0:8);
    const uint32_t c0=raw.Word(color,2),c1=raw.Word(color+2,2),indices=raw.Word(color+4);
    std::array<Pixel,4> palette{Color565(c0),Color565(c1),Pixel{0,0,0,255},Pixel{0,0,0,255}};
    const bool four=c0>c1||format!=18;
    for(uint32_t channel=0;channel<3;++channel)
    {
        const uint32_t a=palette[0][channel],b=palette[1][channel];
        if(four){palette[2][channel]=uint8_t((2*a+b)/3);palette[3][channel]=uint8_t((a+2*b)/3);}
        else palette[2][channel]=uint8_t((a+b)/2);
    }
    if(!four)palette[3][3]=0;
    std::array<uint8_t,16> alpha{};
    if(format==20)alpha=Channel(raw,at);
    for(uint32_t i=0;i<16;++i)
    {
        result[i]=palette[indices>>(2*i)&3];
        if(format==19)result[i][3]=uint8_t((raw.Byte(at+i/2)>>(i%2*4)&15)*17);
        if(format==20)result[i][3]=alpha[i];
    }
    return result;
}
Pixel Texel(const Raw& raw,uint64_t at,uint32_t format)
{
    switch(format)
    {
    case 2:{auto v=raw.Byte(at);return{v,v,v,255};}
    case 4:return Color565(raw.Word(at,2));
    case 6:case 14:return{raw.Byte(at),raw.Byte(at+1),raw.Byte(at+2),raw.Byte(at+3)};
    case 10:return{raw.Byte(at),raw.Byte(at+1),0,255};
    case 7:{auto v=raw.Word(at);return{Unorm(v&1023,10),Unorm(v>>10&1023,10),Unorm(v>>20&1023,10),Unorm(v>>30,2)};}
    case 16:{auto v=raw.Word(at);return{Unorm(v&1023,10),Unorm(v>>10&2047,11),Unorm(v>>21,11),255};}
    case 17:{auto v=raw.Word(at);return{Unorm(v&2047,11),Unorm(v>>11&2047,11),Unorm(v>>22,10),255};}
    case 31:return{Clamp(Half(raw.Word(at,2))),Clamp(Half(raw.Word(at+2,2))),0,255};
    case 32:return{Clamp(Half(raw.Word(at,2))),Clamp(Half(raw.Word(at+2,2))),Clamp(Half(raw.Word(at+4,2))),Clamp(Half(raw.Word(at+6,2)))};
    case 36:{auto v=Clamp(std::bit_cast<float>(raw.Word(at)));return{v,v,v,255};}
    case 22:{auto v=Unorm(raw.Word(at)>>8,24);return{v,v,v,255};}
    case 23:
    {
        const uint32_t depth=raw.Word(at)>>8,e=depth>>20,m=depth&0xFFFFF;
        const auto value=Clamp(e?std::ldexp(1.0+m/1048576.0,int(e)-15):std::ldexp(double(m),-34));
        return{value,value,value,255};
    }
    default:throw std::runtime_error("Unsupported native texture texel");
    }
}
DecodedTexture Decode(std::span<const uint32_t,6> fetch,std::span<const uint8_t> bytes,uint32_t width,uint32_t height,
    uint32_t pitchBlocks,uint64_t offset,uint32_t xBlocks,uint32_t yBlocks,bool tiled,bool tight,uint32_t volumeZ=0,uint32_t volumeRows=0)
{
    const auto info=TextureInfo::FromFetch(fetch);const auto storage=Format(info.format);
    const bool fixed=info.format==25||info.format==26;
    if(!width||!height||width>16384||height>16384||uint64_t(width)*height*(fixed?16:4)>512u*1024*1024)
        throw std::runtime_error("Invalid native texture dimensions");
    const uint32_t columns=(width+storage.block-1)/storage.block,rows=(height+storage.block-1)/storage.block;
    if(pitchBlocks<columns)throw std::runtime_error("Native texture pitch is smaller than width");
    if(!tiled&&tight)
    {
        const uint64_t packed=uint64_t(columns)*rows*storage.bytes,pitched=uint64_t(pitchBlocks)*rows*storage.bytes;
        if(bytes.size()==packed&&packed!=pitched)pitchBlocks=columns;
        else if(bytes.size()<pitched){if(bytes.size()<packed)throw std::runtime_error("Truncated native texture");pitchBlocks=columns;}
    }
    Raw raw{bytes,info.endian};
    DecodedTexture result{width,height,fixed,{}};result.pixels.resize(size_t(width)*height*(fixed?16:4));
    // Fixed-point formats: per-component sign and scale, hoisted out of the texel loop.
    const uint32_t fixedComponents=info.format==25?2:4;
    bool fixedSigned[4]{},fixedNormalized=!(fetch[3]&1);double fixedPower=1;
    if(fixed)
    {
        fixedPower=std::ldexp(1.0,int(((fetch[3]>>13&63)^32))-32);
        for(uint32_t c=0;c<fixedComponents;++c)fixedSigned[c]=(fetch[0]>>(2+c*2)&3)==1;
    }
    // Large images (the CPU-written water textures change every frame) are
    // decoded in row bands on a few threads; each band writes its own rows.
    auto decodeRows=[&](uint32_t firstRow,uint32_t endRow){
    for(uint32_t y=firstRow;y<endRow;++y)for(uint32_t x=0;x<columns;++x)
    {
        const uint64_t at=offset+(tiled?(volumeRows?TextureBlockAddress3D(x,y,volumeZ,pitchBlocks,volumeRows,storage.bytes):Tiled(x+xBlocks,y+yBlocks,(pitchBlocks+31)&~31u,storage.bytes)):
            (uint64_t(y+yBlocks)*pitchBlocks+x+xBlocks)*storage.bytes);
        if(storage.block==4)
        {
            const auto pixels=Block(raw,at,info.format);
            for(uint32_t p=0;p<16;++p)
            {
                const uint32_t px=x*4+p%4,py=y*4+p/4;
                if(px<width&&py<height)memcpy(result.pixels.data()+(size_t(py)*width+px)*4,pixels[p].data(),4);
            }
        }
        else if(fixed)
        {
            float pixel[4]={0,0,0,1};
            for(uint32_t c=0;c<fixedComponents;++c)
            {
                const uint32_t value=raw.Word(at+c*2,2);
                double v=fixedSigned[c]?double(int32_t((value^32768)-32768)):double(value);
                if(fixedNormalized)v=fixedSigned[c]?(std::max)(v/32767.0,-1.0):v/65535.0;
                pixel[c]=float(v*fixedPower); // exact: a power-of-two scale, as ldexp
            }
            memcpy(result.pixels.data()+(size_t(y)*width+x)*16,pixel,16);
        }
        else
        {
            const auto pixel=Texel(raw,at,info.format);
            memcpy(result.pixels.data()+(size_t(y)*width+x)*4,pixel.data(),4);
        }
    }
    };
    const uint32_t bands=rows>=256&&uint64_t(rows)*columns>=65536?(std::min)(4u,(std::max)(1u,std::thread::hardware_concurrency()/2)):1u;
    if(bands<=1)decodeRows(0,rows);
    else
    {
        std::vector<std::future<void>> work;
        const uint32_t step=(rows+bands-1)/bands;
        for(uint32_t band=1;band<bands;++band)
            work.push_back(std::async(std::launch::async,decodeRows,(std::min)(rows,band*step),(std::min)(rows,(band+1)*step)));
        decodeRows(0,(std::min)(rows,step));
        for(auto& done:work)done.get(); // rethrows decode errors
    }
    return result;
}
}

TextureInfo TextureInfo::FromFetch(std::span<const uint32_t,6> fetch)
{
    if((fetch[0]&3)!=2)throw std::runtime_error("Not a native texture fetch");
    const uint32_t dimension=fetch[5]>>9&3;
    if(dimension==1 && (fetch[1]&1024))throw std::runtime_error("Native stacked texture decoding is unsupported");
    const uint32_t width=(dimension==0?fetch[2]&0xFFFFFF:dimension==2?fetch[2]&0x7FF:fetch[2]&0x1FFF)+1;
    const uint32_t height=dimension==0?1:((fetch[2]>>(dimension==2?11:13))&(dimension==2?0x7FF:0x1FFF))+1;
    return{width,height,(std::max)(width,(fetch[0]>>22&511)*32),fetch[1]&63,dimension,fetch[1]>>6&3,bool(fetch[0]>>31),dimension==2?(fetch[2]>>22)+1:1};
}
TextureBlockInfo GetTextureBlockInfo(uint32_t format)
{
    const auto storage=Format(format);return{storage.bytes,storage.block};
}
DecodedTexture DecodeTexture(std::span<const uint32_t,6> fetch,std::span<const uint8_t> bytes,bool allowTightStorage)
{
    const auto info=TextureInfo::FromFetch(fetch);const auto storage=Format(info.format);
    if(info.dimension==2)throw std::runtime_error("A native volume requires DecodeVolume");
    uint32_t pitch=(std::max)((info.width+storage.block-1)/storage.block,info.pitch/storage.block);
    if(!allowTightStorage)pitch=(pitch+31)&~31u;
    uint32_t x=0,y=0;
    const bool packed=(fetch[5]&0x800)&&info.dimension!=0&&(std::min)(info.width,info.height)<=16;
    if(packed)
    {
        // GetPackedMipOffset at level zero: the first level is offset by
        // 16 texels along the shorter axis, even though it is the base mip.
        const auto widthBits=std::bit_width(info.width-1),heightBits=std::bit_width(info.height-1);
        const bool wide=widthBits>heightBits&&widthBits>1;
        if(wide)y=16/storage.block;else x=16/storage.block;
        pitch=(pitch+31)&~31u; // base rows align to 32 blocks, even if linear
    }
    return Decode(fetch,bytes,info.width,info.height,pitch,0,x,y,info.tiled,!packed&&allowTightStorage);
}
// Xenia texture_address.h Tiled3D (BSD-3-Clause, see third_party/xenia/LICENSE).
uint64_t TextureBlockAddress3D(uint32_t x,uint32_t y,uint32_t z,uint32_t pitch,uint32_t rows,uint32_t bytes)
{
    if((pitch|rows)&31)throw std::runtime_error("Unaligned native volume storage");
    const uint64_t outer=((uint64_t(z>>2)*(rows>>4)+(y>>4))*(pitch>>5)+(x>>5))<<7;
    const uint64_t value=(outer|((z&3)<<5)|((y>>1&3)<<3)|(x&7))*bytes;
    const uint32_t bank=((y>>3)^(z>>2))&1,pipe=(x>>3&3)^(bank<<1);
    return uint64_t(y&1)<<4|uint64_t(pipe)<<6|uint64_t(bank)<<11|(value&15)|((value>>4&1)<<5)|((value>>5&7)<<8)|((value>>8)<<12);
}
std::vector<uint8_t> DecodeVolume(std::span<const uint32_t,6> fetch,std::span<const uint8_t> bytes)
{
    const auto info=TextureInfo::FromFetch(fetch);const auto block=Format(info.format);
    if(info.dimension!=2)throw std::runtime_error("Not a native volume");
    if((fetch[5]&0x800)&&(std::min)(info.width,info.height)<=16)
        throw std::runtime_error("Native packed volume tails are unsupported");
    const uint32_t texel=info.format==25||info.format==26?16:4;
    const uint64_t sliceBytes=uint64_t(info.width)*info.height*texel,totalBytes=sliceBytes*info.depth;
    if(totalBytes>512u*1024*1024)throw std::runtime_error("Native volume exceeds decoded size limit");
    const uint32_t pitch=((info.pitch+block.block-1)/block.block+31)&~31u;
    const uint32_t rows=((info.height+block.block-1)/block.block+31)&~31u;
    std::vector<uint8_t> result;result.reserve(size_t(totalBytes));
    for(uint32_t z=0;z<info.depth;++z)
    {
        auto slice=Decode(fetch,bytes,info.width,info.height,pitch,info.tiled?0:uint64_t(z)*pitch*rows*block.bytes,0,0,info.tiled,false,z,rows);
        result.insert(result.end(),slice.pixels.begin(),slice.pixels.end());
    }
    return result;
}
DecodedTexture DecodeMip(std::span<const uint32_t,6> fetch,std::span<const uint8_t> bank,uint32_t level,const MipLayout& layout)
{
    const auto info=TextureInfo::FromFetch(fetch);const auto storage=Format(info.format);
    if(!level||level>15||layout.rowPitchBytes%storage.bytes)throw std::runtime_error("Invalid native mip layout");
    return Decode(fetch,bank,(std::max)(1u,info.width>>level),(std::max)(1u,info.height>>level),
        layout.rowPitchBytes/storage.bytes,layout.offsetBytes,layout.xBlocks,layout.yBlocks,info.tiled,false);
}
uint32_t CubeFaceStride(const TextureInfo& info)
{
    const auto storage=Format(info.format);
    const uint64_t columns=((info.pitch+storage.block-1)/storage.block+31)&~31u,rows=((info.height+storage.block-1)/storage.block+31)&~31u;
    const uint64_t bytes=(columns*rows*storage.bytes+4095)&~4095ull;
    if(bytes>UINT32_MAX)throw std::runtime_error("Native cube face is too large");return uint32_t(bytes);
}
std::vector<uint8_t> DecodeCube(std::span<const uint32_t,6> fetch,std::span<const uint8_t> bytes,bool allowTightStorage)
{
    const auto info=TextureInfo::FromFetch(fetch);
    if(info.dimension!=3||info.format==25||info.format==26)throw std::runtime_error("Unsupported native cube format");
    const uint32_t stride=CubeFaceStride(info);
    if(uint64_t(stride)*6>bytes.size())throw std::runtime_error("Truncated native cube faces");
    std::vector<uint8_t> result;
    for(uint32_t face=0;face<6;++face)
    {
        auto image=DecodeTexture(fetch,bytes.subspan(size_t(face)*stride,stride),allowTightStorage);
        result.insert(result.end(),image.pixels.begin(),image.pixels.end());
    }
    return result;
}
}
