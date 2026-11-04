#include "texture_tables.h"
#include "texture_storage.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>

using namespace gpu::native;
using Bytes=std::vector<uint8_t>;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class Operation>void Reject(Operation operation)
{
    try{operation();}catch(const std::runtime_error&){return;}throw std::runtime_error("Invalid native texture input accepted");
}
std::array<uint32_t,6> Fetch(uint32_t format,uint32_t width=1,uint32_t height=1)
{
    return{2,0x1000|format,(width-1)|((height-1)<<13),0,0,1<<9};
}
int main()
{
    try
    {
        auto fetch=Fetch(6);fetch[1]|=2<<6;
        auto rgba=DecodeTexture(fetch,Bytes{4,3,2,1});Check(rgba.pixels==Bytes({1,2,3,4}),"Texture endian correction failed");
        auto bc1=Fetch(18,4,4);Bytes block={0,0,255,255,255,255,255,255};
        auto transparent=DecodeTexture(bc1,block);Check(transparent.pixels==Bytes(64,0),"BC1 transparent palette failed");
        auto bc3=Fetch(20,4,4);Bytes alpha={255,255,0,0,0,0,0,0};alpha.insert(alpha.end(),block.begin(),block.end());
        auto opaque=DecodeTexture(bc3,alpha);
        for(size_t i=0;i<opaque.pixels.size();i+=4)Check(opaque.pixels[i]==170&&opaque.pixels[i+1]==170&&opaque.pixels[i+2]==170&&opaque.pixels[i+3]==255,"BC3 forced four-colour palette failed");
        auto bc5=Fetch(49,4,4);Bytes planes={200,20,0,0,0,0,0,0,45,230,0,0,0,0,0,0};
        auto normal=DecodeTexture(bc5,planes);Check(normal.pixels[0]==200&&normal.pixels[1]==45&&normal.pixels[2]==0&&normal.pixels[3]==255,"BC5 independent channels failed");
        auto fixed=Fetch(25);fixed[0]|=1<<2|1<<4;
        auto floating=DecodeTexture(fixed,Bytes{0,128,255,127});float values[4];memcpy(values,floating.pixels.data(),16);
        Check(floating.floatPixels&&values[0]==-1&&values[1]==1&&values[2]==0&&values[3]==1,"Fixed16 signed normalization failed");
        TextureTables tables;auto image=Fetch(6);Bytes guest={1,2,3,4};
        Check(tables.Add(image,guest)==0,"Initial resource index wrong");guest[0]=9;
        Check(tables.Add(image,guest)==1,"Resource mutation reused old version");
        Draw draw;std::copy(image.begin(),image.end(),draw.textureFetches[0]);draw.textureFetches[0][1]|=0xA0000000;
        draw.textureFetches[0][3]|=0x688<<1|1<<19;tables.Bind(draw,0,0);
        auto packed=tables.Pack();Check(draw.slots[0]==0&&packed.textures[12]==1&&packed.textures[24]==9,"Owned resource versions changed after mutation");
        Reject([&]{tables.Bind(draw,32,0);});draw.textureFetches[0][1]^=1<<12;
        Reject([&]{tables.Bind(draw,0,0);});
        Reject([&]{DecodeTexture(image,Bytes{1,2,3});});
        auto unknown=Fetch(63);Reject([&]{DecodeTexture(unknown,Bytes(32));});
        auto volume=image;volume[5]=2<<9;Reject([&]{DecodeTexture(volume,Bytes(32));});
        // Actual Breath effect: linear BC3 128x128x32, distinct from the
        // 13-bit 2D size fields. Preserve z order and aligned slice strides.
        std::array<uint32_t,6> breathVolume{0x01024802,0x058C6054,0x07C3F87F,0x00A80D10,3,0x418};
        const auto volumeInfo=TextureInfo::FromFetch(breathVolume);
        Check(volumeInfo.width==128&&volumeInfo.height==128&&volumeInfo.depth==32,"3D fetch dimensions use the wrong bit fields");
        const auto volumeStorage=DescribeTextureStorage(breathVolume);
        Check(volumeStorage.baseBytes==524288,"BC3 volume storage has the wrong slice stride");
        Bytes volumeBlocks(volumeStorage.baseBytes,0);
        for(uint32_t z=0;z<32;++z)for(uint32_t at=0;at<16384;at+=16)
        {
            const size_t p=size_t(z)*16384+at;volumeBlocks[p]=volumeBlocks[p+1]=uint8_t(z*8);
            volumeBlocks[p+8]=0xFF;volumeBlocks[p+9]=0xFF;
        }
        auto volumePixels=DecodeVolume(breathVolume,volumeBlocks);
        for(uint32_t z=0;z<32;++z)for(uint32_t at=0;at<128*128;++at)
        {
            const size_t p=(size_t(z)*128*128+at)*4;
            Check(volumePixels[p]==255&&volumePixels[p+1]==255&&volumePixels[p+2]==255&&volumePixels[p+3]==z*8,"BC3 volume slice content changed");
        }
        Reject([&]{DecodeVolume(breathVolume,std::span<const uint8_t>(volumeBlocks).first(volumeBlocks.size()-1));});
        TextureTables volumeTables;
        volumeTables.Capture(breathVolume,[&](uint32_t address,uint32_t size)->std::span<const uint8_t>{Check(address==volumeStorage.baseAddress&&size==volumeBlocks.size(),"Wrong volume bank acquired");return volumeBlocks;});
        const auto volumePacked=volumeTables.Pack();
        Check(volumeTables.Resources()[0]->volumeDepth==32&&volumePacked.volumes.size()==20+volumePixels.size(),"Owned volume was omitted from replay table");
        // Known hardware tiled addresses cross both y banks and z macrotiles.
        Check(TextureBlockAddress3D(0,0,0,32,32,4)==0&&TextureBlockAddress3D(0,0,1,32,32,4)==1024&&
              TextureBlockAddress3D(0,8,0,32,32,4)==2176&&TextureBlockAddress3D(0,0,4,32,32,4)==18560,"3D tile bank addressing changed");
        auto tiledVolume=Fetch(6,8,8);tiledVolume[0]=2|(1u<<22)|(1u<<31);tiledVolume[5]=2<<9;
        tiledVolume[2]=7|(7<<11)|(4<<22); // five slices exercise the bank XOR.
        const auto tiledStorage=DescribeTextureStorage(tiledVolume);Bytes tiledBytes(tiledStorage.baseBytes);
        for(uint32_t z=0;z<5;++z)for(uint32_t y=0;y<8;++y)for(uint32_t x=0;x<8;++x)
        {
            const auto at=TextureBlockAddress3D(x,y,z,32,32,4);tiledBytes[at]=uint8_t(x);tiledBytes[at+1]=uint8_t(y);tiledBytes[at+2]=uint8_t(z);tiledBytes[at+3]=255;
        }
        auto tiledPixels=DecodeVolume(tiledVolume,tiledBytes);
        for(uint32_t z=0;z<5;++z)for(uint32_t y=0;y<8;++y)for(uint32_t x=0;x<8;++x)
        {
            const size_t at=((z*8+y)*8+x)*4;
            Check(tiledPixels[at]==x&&tiledPixels[at+1]==y&&tiledPixels[at+2]==z&&tiledPixels[at+3]==255,"Tiled volume orientation or slice order changed");
        }
        auto cube=bc1;cube[5]=3<<9;Reject([&]{DecodeCube(cube,block);});
        auto mip=Fetch(6,4,4);Reject([&]{DecodeMip(mip,Bytes(16),1,{8,16,0,0});});
        for(auto [width,height]:{std::pair{16u,64u},std::pair{64u,16u},std::pair{8u,8u}})
        {
            auto packedFetch=Fetch(6,width,height);packedFetch[0]|=4u<<22;packedFetch[5]|=1u<<11;
            Bytes bank(128*128*4);
            const uint32_t x0=width>height?0:16,y0=width>height?16:0;
            for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)
            {
                const auto at=((y+y0)*128+x+x0)*4;
                bank[at]=7;bank[at+1]=19;bank[at+2]=211;bank[at+3]=255;
            }
            auto decoded=DecodeTexture(packedFetch,bank);
            for(size_t at=0;at<decoded.pixels.size();at+=4)
                Check(decoded.pixels[at]==7&&decoded.pixels[at+1]==19&&decoded.pixels[at+2]==211&&decoded.pixels[at+3]==255,"Packed base offset ignored");
            Reject([&]{DecodeTexture(packedFetch,Bytes(4));});
        }
        // Packed level zero shares its base bank with the remaining tail.
        auto tail=Fetch(20,16,64);tail[0]|=4u<<22|1u<<31;
        tail[4]=6u<<6;tail[5]|=1u<<11|0x8000;
        const auto tailStorage=DescribeTextureStorage(tail);
        Check(tailStorage.mipAddress==0x1000&&tailStorage.mips.size()==6&&
            tailStorage.mips[0].xBlocks==2&&tailStorage.mips[0].yBlocks==0,
            "Packed base tail did not share its bank");
        Bytes guestBank((std::max)(tailStorage.baseBytes,tailStorage.mipBytes),7);
        unsigned reads=0;
        auto owned=SnapshotTexture(tail,[&](uint32_t address,uint32_t size)->std::span<const uint8_t>{
            ++reads;
            Check(address==0x1000&&size<=guestBank.size(),"Unexpected texture memory request");return guestBank;
        });
        guestBank[0]=19;
        Check(reads==1&&owned.base[0]==7&&owned.mips[0]==7,"Guest writes mutated owned/shared texture banks");
        Reject([&]{SnapshotTexture(tail,[](uint32_t,uint32_t)->std::span<const uint8_t>{return {};});});
        auto mipOnly=tail;mipOnly[4]|=1<<2;
        Reject([&]{DescribeTextureStorage(mipOnly);});
        auto crossing=Fetch(6,1024,1024);crossing[0]|=32u<<22;crossing[1]=0x1FFFF006;
        Reject([&]{DescribeTextureStorage(crossing);});
        auto flat=Fetch(6,1280,720);flat[0]|=40u<<22;
        Check(DescribeTextureStorage(flat).baseBytes==1280*720*4,"Linear base read extends into unallocated padding");
        auto pitched=Fetch(6,2,2);pitched[0]|=1u<<22;
        Bytes pitchedBank(DescribeTextureStorage(pitched).baseBytes,0);
        pitchedBank[0]=11;pitchedBank[128]=23;
        TextureTables memoryTables;
        const auto memoryIndex=memoryTables.Capture(pitched,[&](uint32_t,uint32_t)->std::span<const uint8_t>{return pitchedBank;});
        const auto memoryPacked=memoryTables.Pack();
        Check(memoryIndex==0&&memoryPacked.textures[12]==11&&memoryPacked.textures[20]==23,
            "Native memory texture was treated as tightly repacked");
        auto linearBC=Fetch(18,8,8);linearBC[0]|=1u<<22;
        Bytes linearBCBank(DescribeTextureStorage(linearBC).baseBytes,0);
        for(uint32_t x=0;x<2;++x)
        {
            linearBCBank[x*8+1]=0xF8; // red RGB565 in the first block row
            linearBCBank[256+x*8]=0xE0;linearBCBank[256+x*8+1]=7; // green in the next
        }
        const auto linearBCImage=DecodeTexture(linearBC,linearBCBank,false);
        Check(linearBCImage.pixels[0]==255&&linearBCImage.pixels[8*4*4+1]==255,
            "Native linear BC base ignored its block-row alignment");
        puts("Native texture palettes, endian, fixed16, 3D BC3/tiled slices, resource ownership and invalid-input checks passed");
        return 0;
    }
    catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}
}
