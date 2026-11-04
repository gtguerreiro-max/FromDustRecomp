#include "operations.h"
#include <cstdio>
#include <stdexcept>
using namespace gpu::native;
void Check(bool value){if(!value)throw std::runtime_error("Native operation unit regression");}
void Append(std::vector<uint8_t>& bytes,uint32_t value){for(int shift=24;shift>=0;shift-=8)bytes.push_back(uint8_t(value>>shift));}
template<class F> void Reject(F&& operation){bool rejected=false;try{operation();}catch(const std::runtime_error&){rejected=true;}Check(rejected);}
int main(){
    try{
        std::vector<uint8_t> packet;
        for(auto word:{0x00032318u,1u,0x1000u,128u,6u})Append(packet,word);
        const auto decoded=ParseCopyRegisters(packet);Check(decoded.control==1&&decoded.base==0x1000&&decoded.pitch==128&&decoded.info==6);
        std::vector<uint8_t> commands;
        Append(commands,0xC0041000); // Type-3 NOP with five payload words.
        commands.insert(commands.end(),packet.begin(),packet.end());
        Append(commands,0);Append(commands,0x80000000); // Single-word padding.
        Append(commands,0x0003A318); // Write-one-register is not a contiguous copy state.
        for(auto word:{2u,0x2000u,256u,7u})Append(commands,word);
        auto actual=packet;actual[11]=0x80; // Real copy base 0x1080, after fake payloads.
        commands.insert(commands.end(),actual.begin(),actual.end());
        Check(ParseCopyRegisters(commands).base==0x1080);
        std::vector<uint8_t> split;
        for(auto word:{0x00012318u,1u,0x5000u,0x0001231Au,128u,6u})Append(split,word);
        Check(ParseCopyRegisters(split).base==0x5000);
        Reject([&]{ParseCopyRegisters(std::span(packet).first(16));});
        Reject([&]{ParseCopyRegisters(std::span(packet).first(19));});
        Reject([&]{ParseCopyRegisters(std::span(commands).first(24));});
        Reject([&]{SnapshotSurfaceState({},{});});
        std::vector<uint8_t> device(0x3294);
        Reject([&]{SnapshotSurfaceState(device,{});});
        Draw source;source.clearFlags=0x31;
        std::vector<RenderRect> rectangles{{2,3,20,30}};
        auto clear=SnapshotClear(std::move(source),0xBF,0x12345678,.5f,7,rectangles);
        rectangles[0]={0,0,1,1};
        Check(clear.clear&&clear.clearFlags==0xB1&&clear.clearColor==0x12345678&&clear.clearDepth==.5f&&clear.clearStencil==7&&clear.clearRects[0].left==2);
        std::array<uint32_t,6> fetch{};
        Reject([&]{SnapshotResolve({},0,fetch,{});});
        puts("Native copy packet boundaries, rejected inputs and clear ownership: passed");return 0;
    }catch(const std::exception& error){fprintf(stderr,"%s\n",error.what());return 1;}
}
