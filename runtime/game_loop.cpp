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
#include "game_loop.h"
#include "kernel/kernel.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
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
            g_debt = (std::min)(g_debt + std::chrono::duration<double>(now - g_last).count() * rate, 2.0);
        else
            g_debt = 1.0;
        g_decision = g_debt >= 1.0;
        if (g_decision) g_debt -= 1.0;
    }
    g_last = now;
    g_decidedFor = index;
    return g_decision;
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
