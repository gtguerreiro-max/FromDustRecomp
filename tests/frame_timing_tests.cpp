#include "gpu/frame_timing.h"
#include "gpu/gpu.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>
namespace gpu { Stats g_stats; }
void Check(bool value){if(!value)throw std::runtime_error("Frame timing regression");}
int main(){
    try {
        gpu::FrameTimingHistory history;
        Check(history.Snapshot().count==0);
        uint64_t time=1000000000,draws=0;
        history.Record(time,draws);
        for(unsigned i=0;i<100;++i){time+=i==99?100000000:16000000;draws+=600;history.Record(time,draws);}
        auto sample=history.Snapshot();
        Check(sample.count==100&&std::abs(sample.meanMs-16.84)<.0001&&sample.p95Ms==16&&sample.maxMs==100&&sample.drawsPerFrame==600);
        for(unsigned i=0;i<240;++i){time+=20000000;draws+=30;history.Record(time,draws);}
        sample=history.Snapshot();
        Check(sample.count==240&&sample.meanMs==20&&sample.p95Ms==20&&sample.maxMs==20&&sample.drawsPerFrame==30&&sample.fps==50);
        history.Record(time,draws);Check(history.Snapshot().count==240);
        history.Reset();Check(history.Snapshot().count==0);
        history.Record(time,draws);time+=30000000;history.Record(time,draws+10);
        Check(history.Snapshot().count==1&&history.Snapshot().meanMs==30);
        puts("Frame timing windows, pacing intervals, percentiles and reset: passed");return 0;
    }catch(const std::exception& error){fprintf(stderr,"%s\n",error.what());return 1;}
}
