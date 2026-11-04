#include "geometry.h"
#include <bit>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <cstring>

using namespace gpu::native;
using Bytes = std::vector<uint8_t>;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Big(Bytes& bytes, uint32_t value) { for (int i = 3; i >= 0; --i) bytes.push_back(uint8_t(value >> (8 * i))); }
void Value(Bytes& bytes, float value) { Big(bytes, std::bit_cast<uint32_t>(value)); }
Bytes Program(bool pulled = true)
{
    Bytes result;
    Big(result, 1 | (pulled ? 2 : 1) << 12 | (pulled ? 5 : 1) << 16);
    Big(result, 2 << 12); Big(result, 0);
    for (uint32_t ordinal = 0; ordinal < (pulled ? 2u : 1u); ++ordinal)
    {
        const uint32_t fetch = 95 - ordinal;
        Big(result, 1 << 19 | fetch / 3 << 20 | fetch % 3 << 25 | ordinal << 12);
        Big(result, 38 << 16); Big(result, 4);
    }
    return result;
}
float Float(std::span<const uint8_t> data, size_t offset) { float value; memcpy(&value, data.data() + offset, 4); return value; }
template<class Function> void Reject(Function operation)
{
    try { operation(); } catch (const std::runtime_error&) { return; }
    throw std::runtime_error("Invalid live geometry accepted");
}
int main()
{
    try
    {
        std::map<uint32_t,Bytes> buffers;
        for (auto point : {std::pair{0.0f,0.0f}, std::pair{1.0f,0.0f}, std::pair{0.0f,1.0f}})
        {
            Value(buffers[0x1000],point.first); Value(buffers[0x1000],point.second);
            Value(buffers[0x1000],0); Value(buffers[0x1000],1);
            for (float value : {0.0f,0.0f,1.0f,0.0f}) Value(buffers[0x2000],value);
        }
        GeometryReader read = [&](uint32_t address, uint32_t) -> std::span<const uint8_t> { return buffers[address]; };
        std::array<uint32_t,192> fetches = {};
        fetches[190] = 0x1003; fetches[191] = 48;
        fetches[188] = 0x2003; fetches[189] = 48;
        VertexProgram program{0, {{0,"POSITION0"},{1,"NORMAL0"}}};
        auto code = Program();
        StreamArena arena;
        auto first = BuildGeometry(program,code,fetches,4,3,false,false,0,read,arena);
        Check(first.vertices.size()==96 && first.indices.size()==12 && first.fetches[4]==0 && arena.bytes.size()==48,"Initial geometry incorrect");
        Draw draw; first.Bind(draw);
        Check(draw.vertices==first.vertices.data() && draw.vertexFetch==reinterpret_cast<const uint8_t*>(first.fetches.data()),"Geometry binding lost owned data");
        buffers[0x1000][0]=0x41; // Change guest data after the first snapshot.
        buffers[0x2000][0]=0x41;
        auto second=BuildGeometry(program,code,fetches,4,3,false,false,0,read,arena);
        Check(Float(first.vertices,0)==0 && Float(second.vertices,0)!=0,"Guest mutation changed owned geometry");
        Check(second.fetches[4]==48 && Float(arena.bytes,0)==0 && arena.bytes.size()==96,"Changed stream reused stale snapshot");
        auto third=BuildGeometry(program,code,fetches,4,3,false,false,0,read,arena);
        Check(third.fetches[4]==48 && arena.bytes.size()==96,"Identical stream snapshot was duplicated");
        buffers[0x1000][0]=0;
        auto rectangle=BuildGeometry(program,code,fetches,8,3,false,false,0,read,arena);
        Check(rectangle.primitive==4 && rectangle.indexCount==6 && rectangle.index32==0 && rectangle.vertices.size()==128,"Rectangle topology incorrect");
        Check(Float(rectangle.vertices,96)==1 && Float(rectangle.vertices,100)==1,"Rectangle fourth vertex incorrect");
        buffers[0x3000]={0,0,0,1};
        Reject([&]{ BuildGeometry(program,code,fetches,4,3,true,false,0x3000,read,arena); });
        Reject([&]{ BuildGeometry(program,code,fetches,1,3,false,false,0,read,arena); });
        Reject([&]{ BuildGeometry(program,std::span(code).first(12),fetches,4,3,false,false,0,read,arena); });
        Reject([&]{ BuildGeometry(program,code,fetches,8,2,false,false,0,read,arena); });
        // A large stream's unused tail must not be converted for every draw.
        const auto firstVertices=buffers[0x1000];
        buffers[0x1000].insert(buffers[0x1000].end(),firstVertices.begin(),firstVertices.end());
        fetches[191]=96;
        buffers[0x3000]={0,0,0,1,0,2,0,255};
        GeometryBounds bounds{};
        auto limited=BuildGeometry(program,code,fetches,4,3,true,false,0x3000,read,arena,&bounds);
        Check(limited.vertices.size()==96&&limited.indices.size()==6,"Bounded draw converted unused vertices or indices");
        buffers[0x3000]={0,2,0,3,0,4};bounds.indexOffset=-2;
        auto negative=BuildGeometry(program,code,fetches,4,3,true,false,0x3000,read,arena,&bounds);
        Check(negative.vertices.size()==96,"Negative base vertex was not applied to bounds");
        buffers[0x3000]={0,0,0,1,0,2};bounds.indexOffset=-1;
        Reject([&]{BuildGeometry(program,code,fetches,4,3,true,false,0x3000,read,arena,&bounds);});
        bounds.indexOffset=3;
        auto offsetRect=BuildGeometry(program,code,fetches,8,3,false,false,0,read,arena,&bounds);
        Draw rectDraw;rectDraw.regs[VGT_INDX_OFFSET]=3;offsetRect.Bind(rectDraw);
        Check(offsetRect.vertices.size()==128&&rectDraw.regs[VGT_INDX_OFFSET]==0,"Expanded rectangle retained its source base vertex");
        bounds.indexOffset=4;
        Reject([&]{BuildGeometry(program,code,fetches,4,3,false,false,0,read,arena,&bounds);});
        bounds={0,true};
        auto pulled=BuildGeometry(program,code,fetches,4,3,false,false,0,read,arena,&bounds);
        Check(pulled.stride==0&&pulled.vertices.size()==32&&(pulled.fetches[3]>>31)&&pulled.fetches[2]==96,
            "Primary stream pulling retained converted per-vertex data");
        auto mini=code;mini[28]|=0x40;mini[35]=0;
        auto inherited=BuildGeometry(program,mini,fetches,4,3,false,false,0,read,arena,&bounds);
        Check(inherited.fetches[6]==96&&(inherited.fetches[5]&0xFFFF)==16,
            "Mini vertex fetch did not inherit its full fetch stream and stride");
        StreamArena versioned;
        std::map<uint32_t,uint64_t> revisions;
        versioned.revision=[&](uint32_t address,uint32_t){return revisions[address];};
        auto owned=BuildGeometry(program,code,fetches,4,3,false,false,0,read,versioned,&bounds);
        const auto retained=versioned.bytes;
        auto unchanged=BuildGeometry(program,code,fetches,4,3,false,false,0,read,versioned,&bounds);
        Check(versioned.bytes==retained&&unchanged.fetches==owned.fetches,"Unchanged revision duplicated a stream");
        buffers[0x2000][1]^=0x40;++revisions[0x2000];
        auto updated=BuildGeometry(program,code,fetches,4,3,false,false,0,read,versioned,&bounds);
        Check(updated.fetches[0]==owned.fetches[0]&&updated.fetches[4]!=owned.fetches[4],"Changed revision did not isolate the modified stream");
        Check(std::equal(retained.begin(),retained.end(),versioned.bytes.begin()),"New stream revision mutated an owned snapshot");
        StreamArena unstable;uint64_t serial=0;
        unstable.revision=[&](uint32_t,uint32_t){return ++serial;};
        Reject([&]{BuildGeometry(program,code,fetches,4,3,false,false,0,read,unstable,&bounds);});
        Check(unstable.bytes.empty(),"Failed acquisition retained a partial stream");
        puts("Native geometry ownership, stream mutation, rectangle and invalid-input checks passed");
        return 0;
    }
    catch(const std::exception& error) { fprintf(stderr,"%s\n",error.what()); return 1; }
}
