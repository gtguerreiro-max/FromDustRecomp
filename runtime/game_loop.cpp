// The game's per-frame component update (sub_825803A8): a manager with a list
// of {component, flags} entries calls each flagged component's Update (vtable
// slot 5) once per frame, then a post-update. Three components are registered
// in play; the world (dustai sub_8A237C20) kicks the GalileoKernel simulation
// jobs (water, terrain, AI) once per call, so the world speed follows the
// frame rate. The camera is updated elsewhere, every rendered frame.
//
// Decoupled mode (simulation rate > 0): the world component runs from a
// real-time accumulator at that fixed rate; everything else, including the
// camera and the render, runs every frame. Button / trigger changes reach the
// game only before an update that steps the world, so presses are not lost
// in frames where the world logic does not run.
//
// - FD_SIM_HZ=<hz>: simulation rate (overrides the setting).
// - FD_SIM_HZ_AFTER=<s>: decouple only that many seconds after start.
// - FD_SIM_GATE=<hex>[,<hex>...]: the fixed-rate Update functions (default 8A237C20).
// - FD_TRACE_COMPONENTS=1: every 10 s, each component's Update, calls and time.
// - FD_TRACE_SIM_STEPS=1: every 10 s, how many frames each world step stayed
//   on screen, and the frame intervals.
// - FD_NO_SIM_SNAP=1: no snapping of frame intervals (see Decide).
// - FD_NO_EVEN_PACE=1: no even pacing (see PaceLoop).
#include "game_loop.h"
#include "settings.h"
#include "kernel/kernel.h"
#include "kernel/objects.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

PPC_FUNC_IMPL(__imp__sub_825801A8);
PPC_FUNC_IMPL(__imp__sub_825803A8);

namespace
{
struct ComponentStats { uint64_t calls = 0; double ms = 0; uint32_t object = 0; };
std::map<uint32_t, ComponentStats> g_stats; // Update function -> stats
uint64_t g_lastDump = 0;

std::atomic<double> g_setting{0.0};
std::mutex g_lock;
uint64_t g_updates = 0, g_decidedFor = UINT64_MAX;
bool g_decision = true;
double g_debt = 0;
std::chrono::steady_clock::time_point g_last{};
std::atomic<int> g_frameLimit{0}, g_evenRate{0};
std::atomic<float> g_evenWorkMs{0};
bool g_dumpFrames = false;

double Rate()
{
    static const double forced = [] { const char* v = getenv("FD_SIM_HZ"); return v ? atof(v) : -1.0; }();
    static const auto from = std::chrono::steady_clock::now() +
        std::chrono::seconds([] { const char* v = getenv("FD_SIM_HZ_AFTER"); return v ? atoi(v) : 0; }());
    const double rate = forced >= 0 ? forced : g_setting.load();
    return rate > 0 && std::chrono::steady_clock::now() >= from ? rate : 0.0;
}

// Whether update number `index` steps the world (decided once, g_lock held).
bool Decide(uint64_t index)
{
    if (g_decidedFor == index) return g_decision;
    const double rate = Rate();
    const auto now = std::chrono::steady_clock::now();
    if (rate <= 0)
    {
        g_decision = true;
        g_debt = 0;
    }
    else
    {
        if (g_last.time_since_epoch().count())
        {
            double steps = std::chrono::duration<double>(now - g_last).count() * rate;
            // A frame interval within 10% of 1/k world steps counts as exactly
            // that: at 60 FPS the timing jitter around 0.5 otherwise makes
            // the world step after 1, 2 or 3 frames, an uneven motion.
            static const bool noSnap = getenv("FD_NO_SIM_SNAP") != nullptr;
            for (int k = 1; k <= 4 && !noSnap; ++k)
                if (std::abs(steps * k - 1.0) < 0.1) { steps = 1.0 / k; break; }
            g_debt = (std::min)(g_debt + steps, 2.0);
        }
        else
            g_debt = 1.0;
        // A tiny tolerance keeps snapped halves (0.5 + 0.5) from missing 1.0.
        g_decision = g_debt >= 1.0 - 1e-6;
        if (g_decision) g_debt = (std::max)(g_debt - 1.0, 0.0);
    }
    static const bool traceSteps = getenv("FD_TRACE_SIM_STEPS") != nullptr;
    if (traceSteps && rate > 0)
    {
        static uint64_t sinceStep = 0, held[5] = {}, intervals[6] = {};
        static auto since = now;
        if (g_last.time_since_epoch().count())
        {
            const double ms = std::chrono::duration<double, std::milli>(now - g_last).count();
            ++intervals[ms < 12 ? 0 : ms < 15.5 ? 1 : ms < 18 ? 2 : ms < 25 ? 3 : ms < 35 ? 4 : 5];
        }
        ++sinceStep;
        if (g_decision) { ++held[(std::min)(sinceStep, uint64_t(5)) - 1]; sinceStep = 0; }
        if (now - since >= std::chrono::seconds(10))
        {
            kernel::Log("sim steps: frames per step 1:%llu 2:%llu 3:%llu 4:%llu 5+:%llu | frame ms <12:%llu 12-15:%llu 15-18:%llu 18-25:%llu "
                        "25-35:%llu 35+:%llu", held[0], held[1], held[2], held[3], held[4], intervals[0], intervals[1], intervals[2],
                        intervals[3], intervals[4], intervals[5]);
            std::fill(std::begin(held), std::end(held), 0);
            std::fill(std::begin(intervals), std::end(intervals), 0);
            since = now;
        }
    }
    g_last = now;
    g_decidedFor = index;
    return g_decision;
}

// Even pacing. With the world at 30 steps per second, a frame rate between
// even rates (say 45 or 55 FPS under a 60 limit) shows some world steps for
// one frame and others for two: water and people move in small jerks while
// the camera is smooth. The loop holds the fastest rate that divides the
// world rate (120, 60 or 30, none above the frame limit) its frames fit in.
// It sleeps here, at the start of the frame's update, so the rest of the
// frame (with the game's own vblank wait) follows at an even cadence.
void PaceLoop(double rate)
{
    using namespace std::chrono;
    using clock = steady_clock;
    static clock::time_point next, woke;
    static int level = -1, frames = 0, missed = 0, fitting = 0, calmWindows = 0;
    static double workSum = 0;
    static const bool off = getenv("FD_NO_EVEN_PACE") != nullptr;
    const auto now = clock::now();
    if (rate <= 0 || off || !settings::Get().evenPacing)
    {
        g_evenRate = 0;
        g_evenWorkMs = 0;
        woke = {};
        level = -1;
        return;
    }
    const auto period = duration_cast<nanoseconds>(duration<double>(1.0 / rate));
    const nanoseconds even[3] = {period / 4, period / 2, period};
    const int limit = g_frameLimit.load();
    int fastest = 0;
    while (limit > 0 && fastest < 2 && even[fastest] + microseconds(200) < nanoseconds(1000000000LL / limit)) ++fastest;
    if (level < 0) level = fastest, frames = missed = fitting = calmWindows = 0, workSum = 0;
    level = std::clamp(level, fastest, 2);
    // FD_EVEN_PACE_FIXED=<120|60|30>: hold that rate (diagnostics).
    static const int fixed = [] { const char* v = getenv("FD_EVEN_PACE_FIXED"); return v ? atoi(v) : 0; }();
    if (fixed) level = fixed >= 120 ? 0 : fixed >= 60 ? 1 : 2;
    // A frame's own time, without this wait: windows of 30 frames, >= 4 over
    // the interval -> the next slower rate; 3 windows with every frame well
    // inside the faster interval -> faster again.
    if (woke.time_since_epoch().count())
    {
        const auto work = now - woke;
        // FD_TRACE_SIM_STEPS: the last frames' own times, "s" = a world step.
        static const bool trace = getenv("FD_TRACE_SIM_STEPS") != nullptr;
        static float recent[40] = {}, recentWait[40][4] = {};
        static uint64_t lastWaits[4] = {};
        static bool recentStep[40] = {};
        static int recentAt = 0;
        if (trace)
        {
            recent[recentAt % 40] = float(duration<double, std::milli>(work).count());
            recentStep[recentAt % 40] = g_decision;
            const uint64_t* waits = kernel::ThreadWaits();
            for (int k = 0; k < 4; ++k)
            {
                recentWait[recentAt % 40][k] = float((waits[k] - lastWaits[k]) / 1e6);
                lastWaits[k] = waits[k];
            }
            if (++recentAt % 600 == 0 || g_dumpFrames)
            {
                std::string text;
                char item[64];
                for (int i = 0; i < 40; ++i)
                {
                    const int at = (recentAt + i) % 40;
                    snprintf(item, sizeof(item), " %.1f%s(%.1f/%.1f/%.1f/%.1f)", recent[at], recentStep[at] ? "s" : "",
                        recentWait[at][0], recentWait[at][1], recentWait[at][2], recentWait[at][3]);
                    text += item;
                }
                kernel::Log("even pacing at %d FPS, frame times:%s", g_evenRate.load(), text.c_str());
                g_dumpFrames = false;
            }
        }
        ++frames;
        workSum += duration<double, std::milli>(work).count();
        if (work > even[level] - microseconds(500)) ++missed;
        if (level > fastest && work < even[level - 1] * 85 / 100) ++fitting;
        // A long gap (loading, a pause) is not a frame.
        if (work > seconds(1)) frames = missed = fitting = 0, workSum = 0;
        if (frames >= 30)
        {
            const int before = level;
            if (missed >= 4 && level < 2) ++level, calmWindows = 0;
            else if (level > fastest && fitting == frames) { if (++calmWindows >= 3) --level, calmWindows = 0; }
            else calmWindows = 0;
            g_evenWorkMs = float(workSum / frames);
            if (level != before) g_dumpFrames = true;
            if (level != before)
                kernel::Log("even pacing: %.0f FPS (frames take %.1f ms without the wait, %d of %d over %.1f ms)",
                    1e9 / double(even[level].count()), workSum / frames, missed, frames,
                    duration<double, std::milli>(even[before]).count());
            frames = missed = fitting = 0;
            workSum = 0;
        }
    }
    g_evenRate = int(1e9 / double(even[level].count()) + 0.5);
    if (now < next) std::this_thread::sleep_until(next);
    // Missed deadlines are dropped, not caught up with a burst.
    next = (std::max)(next, now) + even[level];
    woke = clock::now();
}

const std::vector<uint32_t>& Gated()
{
    static const std::vector<uint32_t> list = [] {
        std::vector<uint32_t> result;
        const char* v = getenv("FD_SIM_GATE");
        const std::string text = v && *v ? v : "8A237C20";
        for (size_t at = 0; at < text.size();)
        {
            const size_t comma = text.find(',', at);
            result.push_back(uint32_t(strtoul(text.substr(at, comma - at).c_str(), nullptr, 16)));
            at = comma == std::string::npos ? text.size() : comma + 1;
        }
        return result;
    }();
    return list;
}
}

namespace gameloop
{
void SetSimulationRate(double hz) { g_setting = (std::max)(hz, 0.0); }
void SetFrameLimit(int fps) { g_frameLimit = (std::max)(fps, 0); }
int EvenPaceRate() { return g_evenRate.load(); }
float EvenPaceWorkMs() { return g_evenWorkMs.load(); }
double SimulationRate() { return Rate(); }
bool InputMayChange()
{
    std::lock_guard lock(g_lock);
    return Rate() <= 0 || Decide(g_updates);
}
}

PPC_FUNC(sub_825803A8)
{
    static const bool trace = getenv("FD_TRACE_COMPONENTS") != nullptr;
    bool worldSteps;
    PaceLoop(Rate());
    {
        std::lock_guard lock(g_lock);
        worldSteps = Decide(g_updates);
        ++g_updates;
    }
    if (!trace && worldSteps)
    {
        __imp__sub_825803A8(ctx, base);
        return;
    }
    const uint32_t manager = ctx.r3.u32;
    PPC_STORE_U32(manager + 64, 1);
    for (uint32_t i = 0; i < PPC_LOAD_U32(manager + 12); ++i)
    {
        const uint32_t entry = PPC_LOAD_U32(manager + 8) + i * 8;
        if (!(PPC_LOAD_U32(entry + 4) & 0x10)) continue;
        const uint32_t object = PPC_LOAD_U32(entry);
        const uint32_t update = PPC_LOAD_U32(PPC_LOAD_U32(object) + 20);
        if (!worldSteps && std::find(Gated().begin(), Gated().end(), update) != Gated().end()) continue;
        const auto start = std::chrono::steady_clock::now();
        ctx.r3.u64 = object;
        PPC_CALL_INDIRECT_FUNC(update);
        if (trace)
        {
            auto& stats = g_stats[update];
            ++stats.calls;
            stats.object = object;
            stats.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }
    }
    PPC_STORE_U32(manager + 64, 0);
    ctx.r3.u64 = manager;
    __imp__sub_825801A8(ctx, base);
    if (!trace) return;
    const uint64_t now = GetTickCount64();
    if (!g_lastDump) g_lastDump = now;
    if (now - g_lastDump >= 10000)
    {
        kernel::Log("=== component updates (%.1f s) ===", (now - g_lastDump) / 1000.0);
        for (const auto& [update, stats] : g_stats)
            kernel::Log("  update sub_%08X object %08X: %llu calls, %.2f ms per call", update, stats.object,
                (unsigned long long)stats.calls, stats.calls ? stats.ms / double(stats.calls) : 0.0);
        g_stats.clear();
        g_lastDump = now;
    }
}
