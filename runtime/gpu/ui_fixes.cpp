#include "../kernel/kernel.h"
#include <array>
#include <bit>
#include <cmath>
#include <atomic>

namespace
{
// The title sprite occupies the lower-left 512x256 region of the UI atlas
// (the UI shader flips V). Its original UV maximum spills into the adjacent
// sprite and draws a thin vertical line. Leave enough room for the bilinear
// footprint of the two mip levels used by the title at its native size.
bool TitleLogo(uint32_t device,uint32_t primitive,uint32_t count,uint32_t vertices,uint32_t stride)
{
    if((primitive&63)!=4||count!=6||stride!=16||device<0x40000000||device>0xBFFFCC00||
        vertices<0x40000000||uint64_t(vertices)+96>0xC0000000)return false;
    const uint32_t size=mem::Read32(device+0x488);
    if((size&0x1FFF)!=1023||(size>>13&0x1FFF)!=1023)return false;
    for(uint32_t i=0;i<6;++i)
    {
        const uint32_t at=vertices+i*16;
        const float x=std::bit_cast<float>(mem::Read32(at)),y=std::bit_cast<float>(mem::Read32(at+4));
        const float u=std::bit_cast<float>(mem::Read32(at+8)),v=std::bit_cast<float>(mem::Read32(at+12));
        if(std::fabs(std::fabs(x)-512)>0.01f||(std::fabs(y)>0.01f&&std::fabs(y-512)>0.01f)||
            !std::isfinite(u)||!std::isfinite(v)||
            (std::fabs(u-.0013634f)>.0001f&&std::fabs(u-.501371f)>.0001f)||
            (std::fabs(v-.0013672f)>.0001f&&std::fabs(v-.251367f)>.0001f))return false;
    }
    return true;
}
}

PPC_FUNC_IMPL(__imp__sub_8250D940);
PPC_FUNC(sub_8250D940)
{
    const uint32_t vertices=ctx.r6.u32;
    const bool logo=TitleLogo(ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,vertices,ctx.r7.u32);
    std::array<uint32_t,12> original{};
    if(logo)
    {
        static std::atomic<bool> logged{false};
        if(!logged.exchange(true))kernel::Log("ui: title logo UVs constrained to its atlas region");
        for(uint32_t i=0;i<6;++i)
        {
            const auto at=vertices+i*16;
            original[i*2]=mem::Read32(at+8);original[i*2+1]=mem::Read32(at+12);
            const float u=std::bit_cast<float>(original[i*2]),v=std::bit_cast<float>(original[i*2+1]);
            mem::Write32(at+8,std::bit_cast<uint32_t>((u<.25f?2.5f:509.5f)/1024));
            mem::Write32(at+12,std::bit_cast<uint32_t>((v<.125f?2.5f:253.5f)/1024));
        }
    }
    __imp__sub_8250D940(ctx,base);
    if(logo)
        for(uint32_t i=0;i<6;++i){mem::Write32(vertices+i*16+8,original[i*2]);mem::Write32(vertices+i*16+12,original[i*2+1]);}
}
