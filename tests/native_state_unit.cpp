#include "device_state.h"
#include "viewport.h"
#include <cstdio>
#include <stdexcept>

using namespace gpu::native;
void Big(std::vector<uint8_t>& data,size_t offset,uint32_t value){for(uint32_t i=0;i<4;++i)data[offset+i]=uint8_t(value>>(24-i*8));}
int main()
{
    try
    {
        std::vector<uint8_t> device(0x3294);Big(device,0x28D4,0xDEADBEEF);Big(device,0x2944,0x10000);Big(device,0x2780,0x55);
        auto state=SnapshotDeviceState(device,{0x00020001,0x00200010,15,true});
        if(state.regs[VGT_INDX_OFFSET]!=15||state.regs[PA_SC_WINDOW_SCISSOR_TL]!=0x00020001||state.context[0x204]!=0x10000||state.boolLoop[0]!=0x55)
            throw std::runtime_error("Packet state did not override stale device shadow");
        bool rejected=false;try{SnapshotDeviceState(device,{});}catch(const std::runtime_error&){rejected=true;}
        if(!rejected)throw std::runtime_error("Missing scissor state was accepted");
        rejected=false;try{SnapshotDeviceState(std::span(device).first(1024),{0,0,0,true});}catch(const std::runtime_error&){rejected=true;}
        if(!rejected)throw std::runtime_error("Truncated device state was accepted");
        Draw viewport;viewport.hasPixelCenter=true;viewport.regs[PA_CL_VTE_CNTL]=0x43F;
        viewport.regs[VPORT_XSCALE]=std::bit_cast<uint32_t>(128.0f);viewport.regs[VPORT_XOFFSET]=std::bit_cast<uint32_t>(128.0f);
        viewport.regs[VPORT_YSCALE]=std::bit_cast<uint32_t>(-128.0f);viewport.regs[VPORT_YOFFSET]=std::bit_cast<uint32_t>(128.0f);
        viewport.regs[VPORT_ZSCALE]=std::bit_cast<uint32_t>(-1.0f);viewport.regs[VPORT_ZOFFSET]=std::bit_cast<uint32_t>(1.0f);
        auto normal=BuildViewport(viewport,256,256);
        if(normal.format!=0x400||normal.host.width!=256||normal.host.minDepth!=1||normal.host.maxDepth!=0||normal.halfPixel[0]!=1.0f/256)
            throw std::runtime_error("Enabled viewport changed its coordinate/depth mapping");
        viewport.regs[PA_CL_VTE_CNTL]=0x400;
        auto pixels=BuildViewport(viewport,256,128);
        if(pixels.format!=0x80000400||pixels.xy[0]!=2.0f/256||pixels.xy[1]!=-2.0f/128||pixels.xy[2]!=-1||pixels.xy[3]!=1||pixels.z[0]!=1||pixels.z[1]!=0)
            throw std::runtime_error("Disabled viewport did not map pixel coordinates to the host target");
        viewport.regs[PA_CL_VTE_CNTL]=0x43F;viewport.regs[VPORT_YSCALE]=std::bit_cast<uint32_t>(128.0f);
        auto flipped=BuildViewport(viewport,256,256);
        if(!(flipped.format&0x80000000)||flipped.xy[1]!=-1)throw std::runtime_error("Positive viewport Y scale was not preserved");
        puts("Native state packet precedence and incomplete-input checks passed");return 0;
    }
    catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}
}
