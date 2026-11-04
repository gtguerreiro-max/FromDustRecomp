#pragma once
#include "frame.h"
#include <bit>
#include <cmath>

namespace gpu::native
{
struct NativeViewport
{
    plume::RenderViewport host;
    float xy[4]={1,1,0,0},z[2]={1,0},halfPixel[2]={0,0};
    uint32_t format=0;
};
inline NativeViewport BuildViewport(const Draw& draw,uint32_t width,uint32_t height)
{
    const auto value=[&](Reg reg){return std::bit_cast<float>(draw.regs[reg]);};
    const float xs=value(VPORT_XSCALE),xo=value(VPORT_XOFFSET),ys=value(VPORT_YSCALE),yo=value(VPORT_YOFFSET);
    const float zs=value(VPORT_ZSCALE),zo=value(VPORT_ZOFFSET);
    const uint32_t control=draw.regs[PA_CL_VTE_CNTL];
    NativeViewport result;
    result.format=control&0x700;
    const bool direct=(control&63)!=63||!(xs>0)||!(ys<0);
    if(direct)
    {
        // Disabled guest transforms make NDC coordinates directly represent
        // pixels. Use the whole host target and apply the guest transform in VS.
        result.host=plume::RenderViewport(0,0,float(width),float(height),0,1);
        result.xy[0]=2*(control&1?xs:1)/width;
        result.xy[1]=-2*(control&4?ys:1)/height;
        result.xy[2]=2*(control&2?xo:0)/width-1;
        result.xy[3]=1-2*(control&8?yo:0)/height;
        result.z[0]=control&16?zs:1;result.z[1]=control&32?zo:0;
        result.format|=0x80000000;
    }
    else result.host=plume::RenderViewport(xo-std::fabs(xs),yo-std::fabs(ys),2*xs,-2*ys,zo,zo+zs);
    if(draw.hasPixelCenter&&!(draw.regs[PA_SU_VTX_CNTL]&1))
    {
        result.halfPixel[0]=direct?1.0f/width:0.5f/xs;
        result.halfPixel[1]=direct?-1.0f/height:0.5f/ys;
    }
    return result;
}
}
