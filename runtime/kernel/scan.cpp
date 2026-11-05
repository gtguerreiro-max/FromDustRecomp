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
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace kernel::debug
{
void ArmWriteWatch(uint32_t address, bool readsToo = false);
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
        // DustAi UI_AbstractInterface script block (reflection offsets):
        // v_CursorVirtualPos +0x64, v_joystickValues +0x74, v_PosCursor +0x80,
        // i_ForcePosCursor +0xAC, v_ForcePosCursor +0xB0.
        {
            std::vector<uint32_t> bases;
            for (const auto& hit : hits)
                for (uint32_t field : {0x80u, 0x84u, 0x88u, 0x64u, 0x68u, 0x6Cu})
                {
                    const uint32_t at = addresses[hit.index], baseAddress = at - field;
                    uint32_t force; memcpy(&force, mem::Translate(baseAddress + 0xAC), 4); force = _byteswap_ulong(force);
                    if (force > 1 || std::find(bases.begin(), bases.end(), baseAddress) != bases.end()) continue;
                    bases.push_back(baseAddress);
                    if (bases.size() > 40) break;
                    Log("float scan: ui block? %08X (moved field +%02X)  virtual (%.2f %.2f %.2f)  joystick (%.2f %.2f %.2f)  pos (%.2f %.2f %.2f)  force %u (%.2f %.2f %.2f)",
                        baseAddress, field, ReadFloat(baseAddress + 0x64), ReadFloat(baseAddress + 0x68), ReadFloat(baseAddress + 0x6C),
                        ReadFloat(baseAddress + 0x74), ReadFloat(baseAddress + 0x78), ReadFloat(baseAddress + 0x7C),
                        ReadFloat(baseAddress + 0x80), ReadFloat(baseAddress + 0x84), ReadFloat(baseAddress + 0x88), force,
                        ReadFloat(baseAddress + 0xB0), ReadFloat(baseAddress + 0xB4), ReadFloat(baseAddress + 0xB8));
                }
        }
        // A moved vector followed by a unit scale (1, 1, 1): an object transform.
        for (const auto& hit : hits)
        {
            const uint32_t at = addresses[hit.index];
            if (ReadFloat(at + 12) == 1.0f && ReadFloat(at + 16) == 1.0f && ReadFloat(at + 20) == 1.0f)
            {
                Log("float scan: transform candidate %08X = (%.2f %.2f %.2f)", at, ReadFloat(at), ReadFloat(at + 4), ReadFloat(at + 8));
                if (const char* watch = getenv("FD_WATCH_WRITE"); watch && !strcmp(watch, "auto")) { ArmWriteWatch(at, false); break; }
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
uint32_t g_watchGuest[4] = {};
LONG WINAPI WatchHandler(EXCEPTION_POINTERS* info)
{
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(info->ContextRecord->Dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD64 hit = info->ContextRecord->Dr6 & 0xF;
    info->ContextRecord->Dr6 = 0;
    for (int slot = 0; slot < 4; ++slot)
        if (hit & (1ull << slot)) TraceGuestWriter(info->ContextRecord, g_watchGuest[slot]);
    return EXCEPTION_CONTINUE_EXECUTION;
}
}
void ArmWatchSlot(uint32_t address, bool readsToo, int slot);
void ArmWriteWatch(uint32_t address, bool readsToo) { ArmWatchSlot(address, readsToo, 0); }
void ArmWatchSlot(uint32_t address, bool readsToo, int slot)
{
        static bool installed = false;
        if (!installed) { installed = true; AddVectoredExceptionHandler(1, WatchHandler); }
        g_watchGuest[slot] = address;
        const uintptr_t host = reinterpret_cast<uintptr_t>(mem::Translate(address));
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
                    (&context.Dr0)[slot] = host;
                    const DWORD64 field = 0xFull << (16 + slot * 4), enable = 3ull << (slot * 2);
                    context.Dr7 = (context.Dr7 & ~(field | enable)) | (1ull << (slot * 2)) |
                        ((((readsToo ? 3ull : 1ull)) | (3ull << 2)) << (16 + slot * 4));
                    if (SetThreadContext(thread, &context)) ++armed;
                }
                ResumeThread(thread);
            }
            CloseHandle(thread);
        }
        CloseHandle(snapshot);
        Log("write watch: %08X armed on %u threads (slot %d%s)", address, armed, slot, readsToo ? ", reads too" : "");
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
        ArmWriteWatch(address, getenv("FD_WATCH_READS") != nullptr);
    }).detach();
}
}

// FD_FIND_WORDS=<hex>,<hex>...@<seconds>: logs every aligned big-endian
// occurrence of those words in the guest allocations and module images, with
// the 48 bytes around each (reflection tables, pointers to known strings).
namespace kernel::debug
{
void StartWordSearch()
{
    const char* option = getenv("FD_FIND_WORDS");
    if (!option) return;
    std::vector<uint32_t> words;
    for (const char* p = option; *p && *p != '@';)
    {
        words.push_back(uint32_t(strtoul(p, nullptr, 16)));
        const char* comma = strchr(p, ',');
        if (!comma) break;
        p = comma + 1;
    }
    const char* at = strchr(option, '@');
    const double seconds = at ? atof(at + 1) : 0;
    const ULONGLONG start = GetTickCount64();
    std::thread([words, seconds, start] {
        while (GetTickCount64() < start + ULONGLONG(seconds * 1000)) Sleep(10);
        auto ranges = mem::AllocatedRanges();
        ranges.push_back({0x82000000u, 0x0A000000u}); // module images
        unsigned found = 0;
        for (const auto& [address, size] : ranges)
            for (uint32_t a = address; a + 4 <= address + size && found < 200; a += 4)
            {
                uint32_t raw; memcpy(&raw, mem::Translate(a), 4); raw = _byteswap_ulong(raw);
                if (std::find(words.begin(), words.end(), raw) == words.end()) continue;
                ++found;
                std::string bytes;
                for (int k = -16; k < 32; k += 4)
                {
                    uint32_t w; memcpy(&w, mem::Translate(a + k), 4);
                    char text[12]; snprintf(text, sizeof(text), k == 0 ? " [%08X]" : " %08X", _byteswap_ulong(w)); bytes += text;
                }
                Log("word search: %08X at %08X:%s", raw, a, bytes.c_str());
            }
        Log("word search: %u hits", found);
    }).detach();
}
}

// FD_FIND_CAMERA=<seconds>: diagnostic search for the DustAi camera parameter
// block (reflection offsets: helico focale +0x94, distance to cursor +0x98,
// global focale +0x12C, height +0x130, distance +0x134, sandbox distance
// min / max +0x140 / +0x144); logs every block whose values are consistent.
namespace kernel::debug
{
void StartCameraSearch()
{
    const char* option = getenv("FD_FIND_CAMERA");
    if (!option) return;
    const double seconds = atof(option);
    const ULONGLONG start = GetTickCount64();
    std::thread([seconds, start] {
        while (GetTickCount64() < start + ULONGLONG(seconds * 1000)) Sleep(10);
        auto ranges = mem::AllocatedRanges();
        ranges.push_back({0x8A000000u, 0x00400000u});
        unsigned found = 0;
        auto ok = [](float v, float lo, float hi) { return std::isfinite(v) && v > lo && v < hi; };
        for (const auto& [address, size] : ranges)
            for (uint32_t o = address; o + 0x150 <= address + size && found < 40; o += 4)
            {
                const float helico = ReadFloat(o + 0x98), global = ReadFloat(o + 0x134);
                if (!ok(helico, 5, 2000) || !ok(global, helico * 1.3f + 5, 20000)) continue;
                const float smin = ReadFloat(o + 0x140), smax = ReadFloat(o + 0x144);
                if (!ok(smin, 0, 20000) || !ok(smax, smin * 1.2f + 5, 50000)) continue;
                const float hf = ReadFloat(o + 0x94), gf = ReadFloat(o + 0x12C), gh = ReadFloat(o + 0x130);
                if (!ok(hf, 0.01f, 500) || !ok(gf, 0.01f, 500) || !ok(gh, -1000, 20000)) continue;
                ++found;
                Log("camera search: %08X helico (focale %.3f distance %.2f time-to-global %.2f min height %.2f) global (focale %.3f height %.2f distance %.2f min height %.2f) sandbox %.2f..%.2f",
                    o, hf, helico, ReadFloat(o + 0x90), ReadFloat(o + 0x8C), gf, gh, global, ReadFloat(o + 0x128), smin, smax);
            }
        Log("camera search: %u candidates", found);
    }).detach();
}
}

// FD_SCAN_INTS=<t1>,<t2>,<t3>,<t4>: diagnostic search for state toggled by
// scripted input (for example a camera mode): 32-bit words with
// t1 == t3 == t4 and t2 different (press, press back, rest).
namespace kernel::debug
{
void StartIntScan()
{
    const char* option = getenv("FD_SCAN_INTS");
    if (!option) return;
    double t[4]{};
    if (sscanf(option, "%lf,%lf,%lf,%lf", &t[0], &t[1], &t[2], &t[3]) != 4) { Log("int scan: FD_SCAN_INTS needs four times"); return; }
    const ULONGLONG start = GetTickCount64();
    std::thread([t0 = t[0], t1 = t[1], t2 = t[2], t3 = t[3], start] {
        const double times[4] = {t0, t1, t2, t3};
        auto waitUntil = [&](double s) { while (GetTickCount64() < start + ULONGLONG(s * 1000)) Sleep(5); };
        // One raw copy at t1; at t2 only the words that changed are kept.
        std::vector<std::vector<uint8_t>> first;
        waitUntil(times[0]);
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
        for (const auto& range : mem::AllocatedRanges()) if (range.first < 0xA0000000u) ranges.push_back(range);
        ranges.push_back({0x8A000000u, 0x00400000u});
        for (const auto& [address, size] : ranges) first.emplace_back(mem::Ptr<uint8_t>(address), mem::Ptr<uint8_t>(address) + size);
        std::vector<uint32_t> addresses;
        std::vector<uint32_t> values[4];
        waitUntil(times[1]);
        for (size_t r = 0; r < ranges.size(); ++r)
            for (uint32_t o = 0; o + 4 <= ranges[r].second; o += 4)
            {
                uint32_t before; memcpy(&before, first[r].data() + o, 4); before = _byteswap_ulong(before);
                const uint32_t now = mem::Read32(ranges[r].first + o);
                if (before != now && before <= 0xFFFF && now <= 0xFFFF) { addresses.push_back(ranges[r].first + o); values[0].push_back(before); values[1].push_back(now); }
            }
        first.clear(); first.shrink_to_fit();
        for (int k = 2; k < 4; ++k)
        {
            waitUntil(times[k]);
            values[k].resize(addresses.size());
            for (size_t i = 0; i < addresses.size(); ++i) values[k][i] = mem::Read32(addresses[i]);
        }
        unsigned hits = 0;
        uint32_t page = 0, perPage = 0;
        for (size_t i = 0; i < addresses.size() && hits < 200; ++i)
        {
            const uint32_t a = values[0][i], b = values[1][i], c = values[2][i], d = values[3][i];
            if (a != c || c != d || a == b || a > 16 || b > 16) continue;
            // At most two hits per 4 KB page (skips toggled buffers).
            if ((addresses[i] >> 12) != page) { page = addresses[i] >> 12; perPage = 0; }
            if (++perPage > 2) continue;
            ++hits;
            Log("int scan: %08X  %u -> %u -> %u -> %u   (%08X %08X [%08X] %08X %08X)", addresses[i], a, b, c, d,
                mem::Read32(addresses[i] - 8), mem::Read32(addresses[i] - 4), mem::Read32(addresses[i]), mem::Read32(addresses[i] + 4), mem::Read32(addresses[i] + 8));
        }
        Log("int scan: %u hits of %zu words", hits, addresses.size());
    }).detach();
}
}
