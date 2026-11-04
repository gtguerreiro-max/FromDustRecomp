#pragma once
#include <cstdint>
#include <span>
#include <vector>
#include <stdexcept>

namespace gpu::native
{
inline uint32_t CommandWord(std::span<const uint8_t> bytes,size_t at)
{
    if(at>bytes.size()||bytes.size()-at<4)throw std::runtime_error("Truncated native command word");
    return uint32_t(bytes[at])<<24|uint32_t(bytes[at+1])<<16|uint32_t(bytes[at+2])<<8|bytes[at+3];
}
struct CommandPacket
{
    uint32_t header;
    std::span<const uint8_t> payload;
};
// The XDK embeds non-executable constant / inline data between packets. Recover
// at a validated suffix, as the independent saved-frame adapter does, rather
// than treating those bytes as an incomplete GPU packet.
inline std::vector<CommandPacket> CommandPackets(std::span<const uint8_t> bytes)
{
    if(bytes.size()%4)throw std::runtime_error("Unaligned native command block");
    const size_t count=bytes.size()/4;
    std::vector<uint32_t> sizes(count);
    std::vector<bool> suffix(count+1,false);suffix[count]=true;
    for(size_t i=count;i--;)
    {
        const auto header=CommandWord(bytes,i*4),type=header>>30;
        uint32_t size=type==2?1:type==1?3:((header>>16&0x3FFF)+2);
        if(!header||header==0x0BADF00D)size=1;
        bool valid=i+size<=count;
        if(type==0&&header&&header!=0x0BADF00D)
            valid=valid&&(header&0x7FFF)+(header&0x8000?1:size-1)<=0x5003;
        if(type==3)
        {
            const auto opcode=header>>8&127;
            switch(opcode)
            {
            case 0x10:case 0x21:case 0x22:case 0x23:case 0x25:case 0x26:case 0x27:
            case 0x2B:case 0x2D:case 0x2E:case 0x2F:case 0x34:case 0x36:case 0x37:
            case 0x3B:case 0x3C:case 0x3D:case 0x3E:case 0x3F:
            case 0x44:case 0x45:case 0x46:case 0x47:case 0x48:case 0x4A:case 0x4B:case 0x4C:
            case 0x50:case 0x54:case 0x55:case 0x56:case 0x58:case 0x59:
            case 0x5B:case 0x5C:case 0x5D:case 0x5E:case 0x64:break;
            default:valid=false;
            }
        }
        if(valid){sizes[i]=size;suffix[i]=suffix[i+size];}
    }
    std::vector<CommandPacket> result;
    for(size_t at=0;at<count;)
    {
        if(!sizes[at]){do{++at;}while(at<count&&!suffix[at]);continue;}
        result.push_back({CommandWord(bytes,at*4),bytes.subspan((at+1)*4,(sizes[at]-1)*4)});
        at+=sizes[at];
    }
    return result;
}
}
