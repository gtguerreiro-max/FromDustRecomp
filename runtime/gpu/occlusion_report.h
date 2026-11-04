#pragma once
#include <array>
#include <cstdint>

namespace gpu
{
// EVENT_WRITE_ZPD snapshots free-running counters. D3D subtracts the begin
// report from the end report and sums the A/B lanes, all stored little endian.
// Until native GPU queries are connected, return a conservative positive
// interval on every report, including survey batches with deferred geometry.
// This follows the fake-query path in Xenia; a zero interval would hide objects.
class VisibleSampleCounter
{
public:
    uint32_t samples=0;
    std::array<uint32_t, 8> Complete() { samples+=100;return Report(); }
    std::array<uint32_t, 8> Report() const
    {
        const uint32_t count = uint32_t(samples), b = count >> 1, a = count - b;
        return {a, b, 0, 0, a, b, 0, 0};
    }
};
}
