// FD_SCAN_FLOATS=<t1>,<t2>,<t3>,<t4> (seconds after start): diagnostic search
// for game state that follows scripted input. At t1 every plausible
// big-endian float in the guest allocations is recorded; at t2..t4 the same
// words are re-read. Logged: words that moved one way between t1 and t2, back
// between t2 and t3, and stayed put between t3 and t4 (script: move, move
// back, rest), with their neighbours to spot vectors.
#include "kernel.h"
#include "debug.h"
#include "../memory.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace kernel::debug
{
void ArmWriteWatch(uint32_t address);
namespace
{
float ReadFloat(uint32_t address)
{
    uint32_t raw;
    memcpy(&raw, mem::Translate(address), 4);
    raw = _byteswap_ulong(raw);
    float value;
    memcpy(&value, &raw, 4);
    return value;
}
bool Plausible(float value) { return std::isfinite(value) && std::fabs(value) >= 0.05f && std::fabs(value) <= 20000.0f; }
}
void StartFloatScan()
{
    const char* option = getenv("FD_SCAN_FLOATS");
    if (!option) return;
    std::vector<double> times;
    for (const char* p = option; *p;)
    {
        times.push_back(atof(p));
        const char* comma = strchr(p, ',');
        if (!comma) break;
        p = comma + 1;
    }
    if (times.size() != 4) { Log("float scan: FD_SCAN_FLOATS needs four times"); return; }
    const ULONGLONG start = GetTickCount64();
    std::thread([times, start] {
        auto waitUntil = [&](double seconds) {
            const ULONGLONG at = start + ULONGLONG(seconds * 1000);
            while (GetTickCount64() < at) Sleep(5);
        };
        waitUntil(times[0]);
        std::vector<uint32_t> addresses;
        std::vector<float> values[4];
        for (const auto& [address, size] : mem::AllocatedRanges())
            for (uint32_t at = address; at + 4 <= address + size; at += 4)
                if (const float value = ReadFloat(at); Plausible(value)) { addresses.push_back(at); values[0].push_back(value); }
        Log("float scan: %zu candidates at %.0f s", addresses.size(), times[0]);
        for (int snapshot = 1; snapshot < 4; ++snapshot)
        {
            waitUntil(times[snapshot]);
            values[snapshot].resize(addresses.size());
            for (size_t i = 0; i < addresses.size(); ++i) values[snapshot][i] = ReadFloat(addresses[i]);
            Log("float scan: snapshot %d at %.0f s", snapshot + 1, times[snapshot]);
        }
        struct Hit { size_t index; float score; };
        std::vector<Hit> hits;
        for (size_t i = 0; i < addresses.size(); ++i)
        {
            const float a = values[0][i], b = values[1][i], c = values[2][i], d = values[3][i];
            const float there = b - a, back = c - b;
            if (!Plausible(b) || !Plausible(c) || !Plausible(d)) continue;
            if (std::fabs(there) < 0.5f || std::fabs(back) < 0.5f || there * back >= 0) continue;
            // A position moved by the stick: comparable distances both ways.
            if (std::fabs(there) > 1000 || std::fabs(back) > 1000 ||
                std::fabs(std::fabs(there) - std::fabs(back)) > 0.6f * std::max(std::fabs(there), std::fabs(back))) continue;
            if (std::fabs(d - c) > 0.02f * std::max(1.0f, std::fabs(back))) continue;
            hits.push_back({i, std::min(std::fabs(there), std::fabs(back))});
        }
        // CPU heap state first (GPU-visible physical memory holds vertex data).
        std::stable_sort(hits.begin(), hits.end(), [&](const Hit& x, const Hit& y) {
            const bool px = addresses[x.index] >= 0xA0000000u, py = addresses[y.index] >= 0xA0000000u;
            return px != py ? py : x.score > y.score;
        });
        Log("float scan: %zu words moved there and back and then rested", hits.size());
        // A moved vector followed by a unit scale (1, 1, 1): an object transform.
        for (const auto& hit : hits)
        {
            const uint32_t at = addresses[hit.index];
            if (ReadFloat(at + 12) == 1.0f && ReadFloat(at + 16) == 1.0f && ReadFloat(at + 20) == 1.0f)
            {
                Log("float scan: transform candidate %08X = (%.2f %.2f %.2f)", at, ReadFloat(at), ReadFloat(at + 4), ReadFloat(at + 8));
                if (const char* watch = getenv("FD_WATCH_WRITE"); watch && !strcmp(watch, "auto")) { ArmWriteWatch(at); break; }
            }
        }
        for (size_t n = 0; n < hits.size() && n < 120; ++n)
        {
            const size_t i = hits[n].index;
            std::string around;
            for (int k = -3; k <= 3; ++k)
            {
                char text[24];
                snprintf(text, sizeof(text), " %.2f", ReadFloat(addresses[i] + k * 4));
                around += k == 0 ? " [" + std::string(text + 1) + "]" : std::string(text);
            }
            Log("float scan: %08X  %.3f -> %.3f -> %.3f -> %.3f  |%s", addresses[i], values[0][i], values[1][i], values[2][i], values[3][i], around.c_str());
        }
    }).detach();
}
}

// FD_WATCH_WRITE=<hex guest address>@<seconds>: diagnostic hardware write
// watchpoint (debug register 0, 4 bytes) on every thread alive at that time;
// each distinct writer's guest call chain is logged once.
#include <tlhelp32.h>
namespace kernel::debug
{
namespace
{
uintptr_t g_watchHost = 0;
uint32_t g_watchGuest = 0;
LONG WINAPI WatchHandler(EXCEPTION_POINTERS* info)
{
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(info->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    info->ContextRecord->Dr6 = 0;
    TraceGuestWriter(info->ContextRecord, g_watchGuest);
    return EXCEPTION_CONTINUE_EXECUTION;
}
}
void ArmWriteWatch(uint32_t address)
{
        g_watchGuest = address;
        g_watchHost = reinterpret_cast<uintptr_t>(mem::Translate(address));
        AddVectoredExceptionHandler(1, WatchHandler);
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 entry{sizeof(entry)};
        unsigned armed = 0;
        for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
        {
            if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId()) continue;
            HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
            if (!thread) continue;
            if (SuspendThread(thread) != DWORD(-1))
            {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(thread, &context))
                {
                    context.Dr0 = g_watchHost;
                    context.Dr7 = (context.Dr7 & ~0xF0003ull) | 1 | (1ull << 16) | (3ull << 18);
                    if (SetThreadContext(thread, &context)) ++armed;
                }
                ResumeThread(thread);
            }
            CloseHandle(thread);
        }
        CloseHandle(snapshot);
        Log("write watch: %08X armed on %u threads", address, armed);
}
void StartWriteWatch()
{
    const char* option = getenv("FD_WATCH_WRITE");
    if (!option || !strcmp(option, "auto")) return;
    const uint32_t address = uint32_t(strtoul(option, nullptr, 16));
    const char* at = strchr(option, '@');
    const double seconds = at ? atof(at + 1) : 0;
    const ULONGLONG start = GetTickCount64();
    std::thread([address, seconds, start] {
        while (GetTickCount64() < start + ULONGLONG(seconds * 1000)) Sleep(10);
        ArmWriteWatch(address);
    }).detach();
}
}
