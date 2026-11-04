#include "texture_tables.h"
#include <algorithm>
#include <stdexcept>
#include <atomic>

namespace gpu::native
{
namespace
{
uint64_t NextVersion(){static std::atomic<uint64_t> version{1};return version++;}
void Word(std::vector<uint8_t>& bytes,uint32_t value){for(uint32_t b=0;b<4;++b)bytes.push_back(uint8_t(value>>(b*8)));}
void Image(std::vector<uint8_t>& bytes,const DecodedTexture& image)
{
    Word(bytes,image.width|(image.floatPixels?0x80000000:0));Word(bytes,image.height);
    bytes.insert(bytes.end(),image.pixels.begin(),image.pixels.end());
}
bool SameImage(std::span<const uint32_t,6> a,std::span<const uint32_t,6> b)
{
    // Sampler/filter/swizzle/LOD state belongs to the draw, not the decoded image.
    return ((a[0]^b[0])&0xFFC003FF)==0 && ((a[1]^b[1])&0x1FFFF4FF)==0 && a[2]==b[2] &&
        ((a[3]^b[3])&0x7E001)==0 && ((a[5]^b[5])&0x600)==0;
}
void BaseImage(TextureResource& entry,std::span<const uint32_t,6> fetch,std::span<const uint8_t> bytes,bool tight)
{
    const auto info=TextureInfo::FromFetch(fetch);
    if(info.dimension==2)
    {
        entry.volumeDepth=info.depth;entry.volume=DecodeVolume(fetch,bytes);
        entry.image={info.width,info.height,info.format==25||info.format==26,{}};
        const size_t size=size_t(info.width)*info.height*(entry.image.floatPixels?16:4);
        entry.image.pixels.assign(entry.volume.begin(),entry.volume.begin()+size);
    }
    else entry.image=DecodeTexture(fetch,bytes,tight);
}
}
uint32_t TextureTables::Add(std::span<const uint32_t,6> fetch,std::span<const uint8_t> bytes)
{
    Entry entry;std::copy(fetch.begin(),fetch.end(),entry.fetch.begin());
    BaseImage(entry,fetch,bytes,true);
    if(TextureInfo::FromFetch(fetch).dimension==3)entry.cube=DecodeCube(fetch,bytes);
    return AddResource(std::make_shared<Entry>(std::move(entry)));
}
uint32_t TextureTables::Capture(std::span<const uint32_t,6> fetch,const TextureMemoryReader& read)
{
    return AddResource(CaptureResource(fetch,read));
}
std::shared_ptr<TextureTables::Entry> TextureTables::CaptureResource(std::span<const uint32_t,6> fetch,const TextureMemoryReader& read)
{
    const auto snapshot=SnapshotTexture(fetch,read);
    Entry entry;entry.fetch=snapshot.fetch;
    BaseImage(entry,fetch,snapshot.base,false);
    if(TextureInfo::FromFetch(fetch).dimension==3)entry.cube=DecodeCube(fetch,snapshot.base,false);
    if(entry.image.floatPixels&&!snapshot.storage.mips.empty())
        throw std::runtime_error("Native float texture mip tables are unsupported");
    for(uint32_t level=0;level<snapshot.storage.mips.size();++level)
        entry.mips.push_back(DecodeMip(fetch,snapshot.mips,level+1,snapshot.storage.mips[level]));
    return std::make_shared<Entry>(std::move(entry));
}
uint32_t TextureTables::AddResource(std::shared_ptr<Entry> entry)
{
    if(!entry)throw std::runtime_error("Empty native texture resource");
    if(!entry->version)entry->version=NextVersion();
    const uint32_t index=uint32_t(entries.size());
    if(!entry->mips.empty())mipOrder.push_back(index);
    entries.push_back(std::move(entry));
    return index;
}
void TextureTables::Mips(uint32_t index,std::span<const uint32_t,6> fetch,std::span<const uint8_t> bank,std::span<const MipLayout> layouts)
{
    if(index>=entries.size()||layouts.empty()||layouts.size()>15)throw std::runtime_error("Invalid native mip assignment");
    const auto& entry=*entries[index];
    if(!SameImage(entry.fetch,fetch)||entry.image.floatPixels||!entry.mips.empty())throw std::runtime_error("Incompatible or repeated native mip assignment");
    std::vector<DecodedTexture> decoded;
    for(uint32_t level=0;level<layouts.size();++level)decoded.push_back(DecodeMip(fetch,bank,level+1,layouts[level]));
    auto updated=std::make_shared<Entry>(entry);
    updated->mips=std::move(decoded);updated->version=NextVersion();
    entries[index]=std::move(updated);mipOrder.push_back(index);
}
void TextureTables::Bind(Draw& draw,uint32_t slot,uint32_t index)const
{
    if(slot>=32||index>=entries.size()||!SameImage(draw.textureFetches[slot],entries[index]->fetch))
        throw std::runtime_error("Native draw texture does not match its resource snapshot");
    draw.slots[slot]=int32_t(index);
}
PackedTextures TextureTables::Pack()const
{
    PackedTextures result;Word(result.textures,uint32_t(entries.size()));
    uint32_t volumes=0;for(const auto& entry:entries)if(entry->volumeDepth)++volumes;
    Word(result.volumes,volumes);
    uint32_t cubes=0;for(const auto& entry:entries)if(!entry->cube.empty())++cubes;
    Word(result.cubes,cubes);Word(result.mips,uint32_t(mipOrder.size()));
    for(uint32_t index=0;index<entries.size();++index)
    {
        const auto& entry=*entries[index];Image(result.textures,entry.image);result.versions.push_back(entry.version);
        if(entry.volumeDepth)
        {
            Word(result.volumes,index);Word(result.volumes,entry.image.width|(entry.image.floatPixels?0x80000000u:0));
            Word(result.volumes,entry.image.height);Word(result.volumes,entry.volumeDepth);
            result.volumes.insert(result.volumes.end(),entry.volume.begin(),entry.volume.end());
        }
        if(!entry.cube.empty())
        {
            Word(result.cubes,index);Word(result.cubes,entry.image.width);Word(result.cubes,entry.image.height);
            result.cubes.insert(result.cubes.end(),entry.cube.begin(),entry.cube.end());
        }
    }
    for(uint32_t index:mipOrder)
    {
        Word(result.mips,index);Word(result.mips,uint32_t(entries[index]->mips.size()));
        for(const auto& image:entries[index]->mips)Image(result.mips,image);
    }
    return result;
}
}
