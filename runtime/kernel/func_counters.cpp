// Call counters for Default.xex guest functions, filled by
// FD_FUNC_COUNTERS builds of DefaultPPC (see ppc_overrides.h). With
// FD_FUNC_COUNTS=<delay seconds>, calls made during the 10 s after the delay
// are written to func_counts.txt in the working directory, per second.
// FD_FUNC_ARGS=<hex address>[,...] also logs lr and r3..r8 of the first 64
// calls to those functions in that window (func_args.txt).

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "kernel.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace
{
constexpr uint32_t kLow = 0x820D0000, kHigh = 0x82620000; // all of Default.xex code
constexpr uint32_t kSlots = (kHigh - kLow) / 4;
std::atomic<uint64_t> g_counts[kSlots];
std::atomic<bool> g_window{false};
std::vector<uint32_t> g_argIds; // fixed before the window opens
std::mutex g_argMutex;
FILE* g_argFile = nullptr;
std::vector<int> g_argLogged;

void Report(unsigned delaySeconds)
{
    Sleep(delaySeconds * 1000);
    {
        std::lock_guard lock(g_argMutex);
        g_argFile = g_argIds.empty() ? nullptr : fopen("func_args.txt", "w");
    }
    std::vector<uint64_t> before(kSlots);
    for (uint32_t i = 0; i < kSlots; ++i) before[i] = g_counts[i].load(std::memory_order_relaxed);
    g_window = true;
    constexpr unsigned kWindow = 10;
    Sleep(kWindow * 1000);
    g_window = false;
    {
        std::lock_guard lock(g_argMutex);
        if (g_argFile) fclose(g_argFile);
        g_argFile = nullptr;
    }
    std::vector<std::pair<uint64_t, uint32_t>> rows;
    for (uint32_t i = 0; i < kSlots; ++i)
    {
        const uint64_t calls = g_counts[i].load(std::memory_order_relaxed) - before[i];
        if (calls) rows.emplace_back(calls, kLow + i * 4);
    }
    std::sort(rows.rbegin(), rows.rend());
    if (FILE* f = fopen("func_counts.txt", "w"))
    {
        fprintf(f, "# calls per second over %u s, functions %08X-%08X\n", kWindow, kLow, kHigh);
        for (auto& [calls, address] : rows) fprintf(f, "%08X %12.1f\n", address, calls / double(kWindow));
        fclose(f);
    }
    kernel::Log("func counts: %zu functions written to func_counts.txt", rows.size());
}
}

extern "C" uint32_t FdRegisterFunc(const char* name)
{
    static std::once_flag start;
    std::call_once(start, [] {
        const char* delay = getenv("FD_FUNC_COUNTS");
        if (!delay || !*delay) return;
        if (const char* args = getenv("FD_FUNC_ARGS"))
            for (const char* p = args; p && *p; p = strchr(p, ','), p = p ? p + 1 : nullptr)
                g_argIds.push_back((uint32_t(strtoul(p, nullptr, 16)) - kLow) / 4);
        g_argLogged.assign(g_argIds.size(), 0);
        std::thread(Report, unsigned(atoi(delay))).detach();
    });
    const char* hex = strstr(name, "sub_");
    if (!hex) return ~0u;
    const uint32_t address = uint32_t(strtoul(hex + 4, nullptr, 16));
    return address >= kLow && address < kHigh ? (address - kLow) / 4 : ~0u;
}

extern "C" void FdCountFunc(uint32_t id, const void* context)
{
    g_counts[id].fetch_add(1, std::memory_order_relaxed);
    if (g_argIds.empty() || !g_window.load(std::memory_order_relaxed)) return;
    const auto it = std::find(g_argIds.begin(), g_argIds.end(), id);
    if (it == g_argIds.end()) return;
    std::lock_guard lock(g_argMutex);
    int& logged = g_argLogged[it - g_argIds.begin()];
    if (!g_argFile || logged >= 64) return;
    ++logged;
    const auto& ctx = *static_cast<const PPCContext*>(context);
    fprintf(g_argFile, "%08X lr=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X\n", kLow + id * 4,
        uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
}
