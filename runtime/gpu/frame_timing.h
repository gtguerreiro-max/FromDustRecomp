#pragma once
#include <array>
#include <cstdint>
#include <mutex>

namespace gpu {
struct FrameTimingSnapshot {
    std::array<float,240> milliseconds{};
    uint32_t count=0;
    double fps=0,meanMs=0,p95Ms=0,maxMs=0,drawsPerFrame=0;
};
// Intervals between actual guest swaps, after frame pacing. These are frame
// delivery timings, not GPU timestamp durations or UI paint timings.
class FrameTimingHistory {
public:
    void Record(uint64_t nanoseconds,uint64_t draws);
    FrameTimingSnapshot Snapshot() const;
    void Reset();
private:
    mutable std::mutex mutex_;
    std::array<float,240> intervals_{};
    std::array<uint64_t,240> draws_{};
    uint32_t count_=0,next_=0;
    uint64_t previousTime_=0,previousDraws_=0;
    bool started_=false;
};
FrameTimingHistory& FrameTimings();
void RecordFrameTiming();
}
