#pragma once
#include "frame.h"

namespace gpu::native
{
// File adapter only. Moving preserves its backing storage; copying is disabled
// because Draw references that storage. View() is rebuilt after any move.
class ReplayFrame
{
public:
    ReplayFrame() = default;
    ReplayFrame(ReplayFrame&&) = default;
    ReplayFrame& operator=(ReplayFrame&&) = default;
    ReplayFrame(const ReplayFrame&) = delete;
    ReplayFrame& operator=(const ReplayFrame&) = delete;
    static ReplayFrame Load(const std::string& directory);
    static void Save(const FrameInput& frame,const std::string& directory);
    FrameInput View() const;
private:
    std::vector<uint8_t> replay, textures, mips, cubes, streams, volumes;
    std::vector<uint8_t> debugPS, depthVS, depthPS, depthFloatPS;
    std::vector<Draw> draws;
    uint32_t frontBufferBase=0;
};
}
