#include "device_state.h"
#include <algorithm>
#include <stdexcept>

namespace gpu::native
{
namespace
{
uint32_t Big(std::span<const uint8_t> bytes, size_t offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 4) throw std::runtime_error("Truncated D3D device state");
    return uint32_t(bytes[offset]) << 24 | uint32_t(bytes[offset+1]) << 16 | uint32_t(bytes[offset+2]) << 8 | bytes[offset+3];
}
// From Dust XDK flushes: 8250DDA0 -> 820EA830, 820EA8F0. The
// bool/loop shadow follows the two 4096-byte float banks at device+0x2780.
struct Block { uint32_t first, offset, count; };
constexpr Block blocks[] = {
    {0x2000,0x2880,16}, {0x2100,0x28CC,21}, {0x2180,0x2920,5},
    {0x2200,0x2934,12}, {0x2280,0x2964,21}, {0x2300,0x29B8,38},
    {0x2380,0x2A50,8}, {0x2388,0x2820,24}
};
constexpr uint32_t registers[kRegCount] = {
    0x2000,0x2001,0x2002,0x2081,0x2082,0x2104,0x2200,0x2201,0x2205,0x2102,
    0x210F,0x2110,0x2111,0x2112,0x2113,0x2114,0x2206,0x2003,0x2004,0x2005,
    0x2209,0x220A,0x220B,0x2302,0x210D,0x210C,0x2105,0x2106,0x2107,0x2108
};
}

Draw SnapshotDeviceState(std::span<const uint8_t> device, const PacketState& packet)
{
    if (device.size() < 0x2A70 || !packet.hasScissor) throw std::runtime_error("Incomplete native packet/device state");
    Draw result;
    for (const auto& block : blocks)
        for (uint32_t k=0;k<block.count;++k) result.context[block.first-0x2000+k]=Big(device,block.offset+k*4);
    result.context[0x81]=packet.scissorTL;
    result.context[0x82]=packet.scissorBR;
    result.context[0x102]=packet.indexOffset;
    for (uint32_t k=0;k<kRegCount;++k) result.regs[k]=result.context[registers[k]-0x2000];
    for (uint32_t k=0;k<40;++k) result.boolLoop[k]=Big(device,0x2780+k*4);
    for (uint32_t slot=0;slot<32;++slot)
    {
        auto* fetch=result.textureFetches[slot];
        for (uint32_t k=0;k<6;++k) fetch[k]=Big(device,0x480+slot*24+k*4);
        const bool texture=(fetch[0]&3)==2;
        result.slots[slot]=-1;
        result.bases[slot]=texture ? fetch[1]&0x1FFFF000 : 0;
        result.swizzles[slot]=fetch[3]>>1&0xFFF;
        const uint32_t exponent=fetch[3]>>13&63;
        result.expAdjust[slot]=int32_t((exponent^32)-32);
        result.fetchFormats[slot]=texture ? fetch[1]&63 : UINT32_MAX;
        result.samplerStates[slot]=texture ? (fetch[0]>>10&511)|((fetch[3]>>19&511)<<9) : UINT32_MAX;
    }
    result.hasStencilState=result.hasPixelCenter=result.hasBlendConstants=true;
    result.hasContext=result.hasFetchMetadata=result.hasSamplerStates=true;
    return result;
}
}
