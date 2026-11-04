#include "gpu/occlusion_report.h"
#include "gpu/gpu.h"
#include <bit>
#include <cstdio>
#include <stdexcept>

void Check(bool condition)
{
    if (!condition) throw std::runtime_error("Occlusion report regression");
}
int main()
{
    try
    {
        static_assert(std::endian::native == std::endian::little);
        static_assert(gpu::reg::RB_SAMPLE_COUNT_ADDR == 0x2325);
        static_assert(gpu::reg::COHER_STATUS_HOST == 0x0A31);
        static_assert(gpu::reg::D1MODE_VBLANK_VLINE_STATUS == 0x1951);
        gpu::VisibleSampleCounter counter;
        const auto begin = counter.Report();
        Check(begin == std::array<uint32_t, 8>{});
        const auto end = counter.Complete();
        Check(sizeof(end) == 32 && end[4] + end[5] - begin[4] - begin[5] == 100);
        Check(end[0] == end[4] && end[1] == end[5]);
        Check(end[2] == 0 && end[3] == 0 && end[6] == 0 && end[7] == 0);
        const auto bytes = std::bit_cast<std::array<uint8_t, 32>>(end);
        Check(bytes[16] == 50 && bytes[17] == 0 && bytes[18] == 0 && bytes[19] == 0);
        Check(counter.Report() == end); // Inspection must not advance the counter.
        const auto next = counter.Complete();
        Check(next[4] + next[5] - end[4] - end[5] == 100); // Conservative even without direct draws.
        counter.samples=0xFFFFFF9C;
        const auto wrapped=counter.Complete();
        Check(wrapped==std::array<uint32_t,8>{});
        puts("Occlusion query layout, endian order, intervals and wrap: passed");
        return 0;
    }
    catch (const std::exception& error) { fprintf(stderr, "%s\n", error.what()); return 1; }
}
