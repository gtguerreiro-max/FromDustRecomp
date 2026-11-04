#include "device_state.h"
#include "replay_frame.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

using namespace gpu::native;
std::vector<uint8_t> Read(const std::string& path)
{
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file) throw std::runtime_error("Missing state fixture");
    const auto size=file.tellg();file.seekg(0);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if(!file.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))throw std::runtime_error("Truncated state fixture");
    return bytes;
}
struct Reader
{
    std::span<const uint8_t> data;
    std::span<const uint8_t> Take(size_t n) { if(n>data.size())throw std::runtime_error("Truncated state case");auto bytes=data.first(n);data=data.subspan(n);return bytes; }
    uint32_t Word(){auto b=Take(4);uint32_t w;memcpy(&w,b.data(),4);return w;}
};
int main(int argc,char**argv)
{
    if(argc<2)return 2;
    try
    {
        uint64_t checked=0;
        for(int i=1;i<argc;++i)
        {
            auto frame=ReplayFrame::Load(argv[i]);auto view=frame.View();
            auto bytes=Read(std::string(argv[i])+"/state-tests.bin");Reader cases{bytes};
            if(cases.Word()!=0x54534446||cases.Word()!=1)throw std::runtime_error("Invalid state fixture");
            for(uint32_t count=cases.Word();count;--count)
            {
                const uint32_t record=cases.Word(),indexOffset=cases.Word(),size=cases.Word();
                const auto device=cases.Take(size);const auto& expected=view.draws[record];
                const auto actual=SnapshotDeviceState(device,{expected.regs[PA_SC_WINDOW_SCISSOR_TL],expected.regs[PA_SC_WINDOW_SCISSOR_BR],indexOffset,true});
                if(memcmp(actual.regs,expected.regs,sizeof(actual.regs))||actual.context[0x204]!=expected.context[0x204]||
                    memcmp(actual.boolLoop,expected.boolLoop,sizeof(actual.boolLoop))||
                    memcmp(actual.bases,expected.bases,sizeof(actual.bases))||memcmp(actual.swizzles,expected.swizzles,sizeof(actual.swizzles))||
                    memcmp(actual.expAdjust,expected.expAdjust,sizeof(actual.expAdjust))||memcmp(actual.fetchFormats,expected.fetchFormats,sizeof(actual.fetchFormats))||
                    memcmp(actual.samplerStates,expected.samplerStates,sizeof(actual.samplerStates))||memcmp(actual.textureFetches,expected.textureFetches,sizeof(actual.textureFetches)))
                {fprintf(stderr,"%s record %u: state mismatch\n",argv[i],record);return 1;}
                ++checked;
            }
        }
        printf("Native device state: %llu draws match pipeline state, clip control, bool/loop banks and fetch descriptors\n",checked);
        return 0;
    }
    catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}
}
