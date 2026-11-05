#include "latency.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>

namespace latency
{
namespace
{
using Clock = std::chrono::steady_clock;
struct Pending { Clock::time_point at; uint64_t frame; };
std::mutex g_lock;
std::deque<Pending> g_pending;
std::atomic<uint64_t> g_captured{0};
uint64_t g_delivered = 0;
Stats g_published;
double g_sum = 0, g_min = 0, g_max = 0;
uint32_t g_count = 0;
Clock::time_point g_windowStart = Clock::now(), g_lastLog = Clock::now();
}

void InputChanged()
{
    const uint64_t target = g_captured.load() + 1;
    std::lock_guard lock(g_lock);
    // The earliest change per frame counts; bound the queue if nothing presents.
    if (!g_pending.empty() && g_pending.back().frame == target) return;
    if (g_pending.size() > 64) g_pending.pop_front();
    g_pending.push_back({Clock::now(), target});
}

void FrameCaptured() { ++g_captured; }
uint64_t CapturedFrames() { return g_captured.load(); }

void FrameDelivered(uint64_t frame)
{
    std::lock_guard lock(g_lock);
    g_delivered = (std::max)(g_delivered, frame);
}

void Presented()
{
    const auto now = Clock::now();
    std::lock_guard lock(g_lock);
    while (!g_pending.empty() && g_pending.front().frame <= g_delivered)
    {
        const double ms = std::chrono::duration<double, std::milli>(now - g_pending.front().at).count();
        g_pending.pop_front();
        if (!g_count) g_min = g_max = ms;
        g_min = (std::min)(g_min, ms);
        g_max = (std::max)(g_max, ms);
        g_sum += ms;
        ++g_count;
    }
    if (now - g_windowStart >= std::chrono::seconds(2))
    {
        g_published = g_count ? Stats{g_sum / g_count, g_min, g_max, g_count} : Stats{};
        g_sum = g_min = g_max = 0;
        g_count = 0;
        g_windowStart = now;
        if (g_published.samples && now - g_lastLog >= std::chrono::seconds(10))
        {
            fprintf(stderr, "input latency: %.1f ms average, %.1f-%.1f ms (%u samples)\n", g_published.averageMs,
                g_published.minimumMs, g_published.maximumMs, g_published.samples);
            g_lastLog = now;
        }
    }
}

Stats Get()
{
    std::lock_guard lock(g_lock);
    return g_published;
}
}
