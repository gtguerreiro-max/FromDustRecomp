#include "memory.h"
#include "gpu/occlusion_report.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void Check(bool condition){if(!condition)throw std::runtime_error("Physical memory alias regression");}
int main()
{
    try
    {
        Check(mem::Init());
        Check(mem::AllocPhysical(0x10000,0x10000)!=0); // E has no alias of physical page zero.
        const auto address=mem::AllocPhysical(0x10000,0x10000);
        Check(address>=mem::kPhysicalBase);
        const uint32_t offset=address&(mem::kPhysicalSize-1);
        const std::array<uint32_t,3> aliases{0xA0000000u|offset,0xC0000000u|offset,0xE0000000u|(offset-0x1000)};
        for(auto alias:aliases)Check(mem::PhysicalAddress(alias)==offset);
        for(unsigned i=0;i<3;++i)
        {
            mem::Write32(aliases[i]+16,0x10203040+i);
            for(auto alias:aliases)Check(mem::Read32(alias+16)==0x10203040+i);
            Check(mem::AllocationSize(aliases[i])==0x10000);
        }
        // GPU writes reports via A; XDK GetData polls and byte-swaps via C.
        gpu::VisibleSampleCounter counter;
        const auto begin=counter.Complete(),end=counter.Complete();
        mem::Write32(aliases[1]+16,0xFFFFFEED);
        memcpy(mem::Translate(aliases[0]+32),begin.data(),32);
        memcpy(mem::Translate(aliases[0]),end.data(),32);
        auto little=[&](uint32_t at){uint32_t value;memcpy(&value,mem::Translate(aliases[1]+at),4);return value;};
        Check(mem::Read32(aliases[1]+16)!=0xFFFFFEED);
        Check(little(16)+little(20)-little(48)-little(52)==100);
        // Private virtual / image / MMIO regions must remain independent.
        mem::Write32(0x82000010,0x76543210);mem::Write32(0x7FC80010,0x12345678);
        Check(mem::Read32(0x82000010)==0x76543210&&mem::Read32(0x7FC80010)==0x12345678);
        Check(mem::ProtectPhysical(offset,0x1000,true));
        for(auto alias:aliases){MEMORY_BASIC_INFORMATION info{};Check(VirtualQuery(mem::Translate(alias),&info,sizeof(info))&&info.Protect==PAGE_READONLY);}
        Check(mem::ProtectPhysical(offset,0x1000,false));
        for(auto alias:aliases){MEMORY_BASIC_INFORMATION info{};Check(VirtualQuery(mem::Translate(alias),&info,sizeof(info))&&info.Protect==PAGE_READWRITE);}
        mem::Write32(aliases[2]+16,0xAABBCCDD);Check(mem::Read32(aliases[0]+16)==0xAABBCCDD);
        mem::Free(aliases[1]);Check(mem::AllocationSize(address)==0);
        const auto reused=mem::AllocPhysical(0x10000,0x10000);Check(reused==address&&mem::Read32(reused+16)==0);
        puts("Physical A/C/E aliases, bidirectional writes, shared protection, private regions and aliased free: passed");return 0;
    }
    catch(const std::exception& error){fprintf(stderr,"%s\n",error.what());return 1;}
}
