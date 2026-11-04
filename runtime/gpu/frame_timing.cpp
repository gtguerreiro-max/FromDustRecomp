#include "frame_timing.h"
#include "gpu.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace gpu {
void FrameTimingHistory::Record(uint64_t nanoseconds,uint64_t draws) {
    std::lock_guard lock(mutex_);
    if(started_&&nanoseconds>previousTime_&&draws>=previousDraws_) {
        intervals_[next_]=float(double(nanoseconds-previousTime_)/1000000.0);
        draws_[next_]=draws-previousDraws_;
        next_=(next_+1)%uint32_t(intervals_.size());
        count_=(std::min)(count_+1,uint32_t(intervals_.size()));
    }
    previousTime_=nanoseconds;previousDraws_=draws;started_=true;
}
FrameTimingSnapshot FrameTimingHistory::Snapshot() const {
    FrameTimingSnapshot result;
    double total=0,draws=0;
    {
        std::lock_guard lock(mutex_);
        result.count=count_;
        const uint32_t first=(next_+uint32_t(intervals_.size())-count_)%uint32_t(intervals_.size());
        for(uint32_t i=0;i<count_;++i) {
            const auto index=(first+i)%uint32_t(intervals_.size());
            result.milliseconds[i]=intervals_[index];
            total+=intervals_[index];draws+=draws_[index];
        }
    }
    if(!result.count)return result;
    result.meanMs=total/result.count;result.fps=1000.0/result.meanMs;
    result.drawsPerFrame=draws/result.count;
    auto sorted=result.milliseconds;
    std::sort(sorted.begin(),sorted.begin()+result.count);
    result.p95Ms=sorted[size_t(std::ceil(result.count*.95))-1];
    result.maxMs=sorted[result.count-1];
    return result;
}
void FrameTimingHistory::Reset() {
    std::lock_guard lock(mutex_);count_=next_=0;started_=false;
}
FrameTimingHistory& FrameTimings() { static FrameTimingHistory history;return history; }
void RecordFrameTiming() {
    const auto now=std::chrono::steady_clock::now().time_since_epoch();
    FrameTimings().Record(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()),g_stats.draws.load(std::memory_order_relaxed));
}
}
