#include "frame.h"
#include "renderer.h"
#include "agility_exports.h"
#include <array>
#include <bit>
#include <cstdio>
#include <stdexcept>

using namespace gpu::native;

int main(int argc,char** argv)
{
    if(argc!=2)return 2;
    try
    {
        Renderer renderer;
        if(!renderer.LoadShaderCatalog(argv[1]))return 1;
        Draw clear;
        clear.clear=true;clear.clearFlags=1;
        clear.regs[RB_SURFACE_INFO]=128;
        clear.regs[PA_SC_WINDOW_SCISSOR_BR]=4|(4<<16);
        Draw copy;
        copy.resolve=true;copy.copyDest=0x1000;copy.copySurface=128;
        copy.copyWidth=copy.copyHeight=4;copy.copyDestInfo=6<<7;
        copy.regs[PA_SC_WINDOW_SCISSOR_BR]=4|(4<<16);
        std::array<uint8_t,4> textures{};
        auto render=[&](std::span<const Draw> draws,std::array<uint8_t,3> color,bool continuous=true)
        {
            FrameInput frame;frame.draws=draws;frame.textures=textures;frame.continuous=continuous;
            FrameOutput output;
            if(renderer.RenderFrame(frame,{},&output)||output.width!=4||output.height!=4||output.debugErrors)
                throw std::runtime_error("History frame failed");
            for(size_t at=0;at<output.rgb.size();++at)
                if(output.rgb[at]!=color[at%3])throw std::runtime_error("Frame history lost or stale");
        };
        clear.clearColor=0xFFFF0000;
        std::array<Draw,2> frame{clear,copy};
        render(frame,{255,0,0});
        // No new allocation, clear or draw: the source must retain GPU contents.
        render(std::span<const Draw>(&copy,1),{255,0,0});
        auto depthClear=clear;depthClear.clearFlags=0x10;depthClear.clearDepth=.5f;
        depthClear.regs[RB_DEPTH_INFO]=1;
        auto otherCopy=copy;otherCopy.copyDepth=1;
        std::array<Draw,2> changeDepth{depthClear,otherCopy};
        // Rebinding a different depth buffer must retain the same EDRAM colour.
        render(changeDepth,{255,0,0});
        frame[0].clearColor=0xFF0000FF;
        render(frame,{0,0,255});
        render(std::span<const Draw>(&copy,1),{0,0,255});
        render(std::span<const Draw>(&otherCopy,1),{0,0,255});
        // An isolated frame resets the persistent storage, then may start a new session.
        frame[0].clearColor=0xFF00FF00;
        render(frame,{0,255,0},false);
        render(std::span<const Draw>(&copy,1),{0,255,0});
        if(renderer.CompletedFrames()!=8)throw std::runtime_error("Incorrect completed frame count");
        printf("GPU history: eight frames, retained colour across depth bindings, updates and reset, zero D3D12 errors\n");
        return 0;
    }
    catch(const std::exception& error){fprintf(stderr,"%s\n",error.what());return 1;}
}
