#include "replay_frame.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <stdexcept>

using namespace gpu::native;
void Check(bool condition){if(!condition)throw std::runtime_error("Native replay state regression");}
int main(int argc,char** argv)
{
    if(argc!=2)return 2;
    try
    {
        std::array<uint8_t,4096> constants{};
        std::array<uint8_t,512> fetch{};
        std::array<uint8_t,4> textures{};
        Draw draw;draw.vs="000";draw.constants=draw.psConstants=constants.data();draw.vertexFetch=fetch.data();
        draw.hasContext=draw.hasFetchMetadata=draw.hasSamplerStates=draw.hasPixelCenter=draw.hasBlendConstants=draw.hasStencilState=true;
        draw.cpuTextureMask=0x80080001;
        Draw copy;copy.resolve=true;copy.copyDest=0x12000;copy.copyWidth=copy.copyHeight=4;
        copy.regs[PA_SC_WINDOW_SCISSOR_BR]=4|(4<<16);copy.regs[RB_COLOR_INFO]=0x273;
        const std::array<Draw,2> draws{draw,copy};
        FrameInput input;input.draws=draws;input.textures=textures;input.frontBufferBase=copy.copyDest;input.continuous=true;
        ReplayFrame::Save(input,argv[1]);
        auto restored=ReplayFrame::Load(argv[1]);auto view=restored.View();
        Check(view.draws.size()==2&&view.draws[0].cpuTextureMask==draw.cpuTextureMask);
        Check(view.draws[1].regs[RB_COLOR_INFO]==copy.regs[RB_COLOR_INFO]&&view.draws[1].regs[PA_SC_WINDOW_SCISSOR_BR]==copy.regs[PA_SC_WINDOW_SCISSOR_BR]);
        Check(view.frontBufferBase==copy.copyDest&&!view.continuous); // GPU history is not serialized.
        const auto state=std::filesystem::path(argv[1])/"native-state.bin";
        // A valid FDR9 mutation must reject the old sidecar, not bind it silently.
        const auto replay=std::filesystem::path(argv[1])/"replay.bin";
        {std::fstream file(replay,std::ios::binary|std::ios::in|std::ios::out);file.seekp(15);file.put(1);}
        bool rejected=false;try{ReplayFrame::Load(argv[1]);}catch(const std::exception&){rejected=true;}Check(rejected);
        ReplayFrame::Save(input,argv[1]);
        std::filesystem::resize_file(state,25);
        rejected=false;try{ReplayFrame::Load(argv[1]);}catch(const std::exception&){rejected=true;}Check(rejected);
        std::filesystem::resize_file(state,0);
        rejected=false;try{ReplayFrame::Load(argv[1]);}catch(const std::exception&){rejected=true;}Check(rejected);
        ReplayFrame::Save(input,argv[1]);
        std::filesystem::remove(state);
        auto legacy=ReplayFrame::Load(argv[1]);Check(legacy.View().draws[0].cpuTextureMask==0&&legacy.View().frontBufferBase==0);
        puts("Native replay ownership, resolve state, front buffer, stale/truncated sidecar and legacy adapter: passed");return 0;
    }
    catch(const std::exception& error){fprintf(stderr,"%s\n",error.what());return 1;}
}
