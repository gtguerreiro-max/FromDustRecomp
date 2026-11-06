#include <array>
#include <string>
// Hang diagnostics: dumps the guest call stack of every guest thread by
// unwinding the host stack and mapping recompiled functions back to their
// original guest addresses.

#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <vector>

#include "debug.h"
#include "kernel.h"
#include "thread.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

namespace kernel::debug
{
    namespace
    {
        struct HostRange
        {
            uintptr_t begin, end;
            uint32_t guest;
        };

        std::mutex g_lock;
        std::vector<HANDLE> g_threads;
        std::vector<uint32_t> g_threadIds;
        std::vector<PPCContext* const*> g_contexts;
        std::vector<HostRange> g_ranges; // sorted by begin

        void BuildRanges()
        {
            if (!g_ranges.empty())
                return;
            // Host function start -> guest address, sized with the unwind table.
            const size_t entries = (FD_GUEST_CODE_HIGH - FD_GUEST_CODE_LOW) / 4;
            for (size_t i = 0; i < entries; i++)
            {
                if (!g_ppcFuncTable[i])
                    continue;
                const uintptr_t host = reinterpret_cast<uintptr_t>(g_ppcFuncTable[i]);
                DWORD64 imageBase;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(host, &imageBase, nullptr);
                if (!fn)
                    continue;
                g_ranges.push_back({ imageBase + fn->BeginAddress, imageBase + fn->EndAddress,
                    uint32_t(FD_GUEST_CODE_LOW + i * 4) });
            }
            std::sort(g_ranges.begin(), g_ranges.end(), [](auto& a, auto& b) { return a.begin < b.begin; });
        }

        // Name of a host (runtime) function, via the PDB.
        const char* HostSymbol(uintptr_t rip)
        {
            static bool initialized = SymInitialize(GetCurrentProcess(), nullptr, TRUE);
            static thread_local char buffer[sizeof(SYMBOL_INFO) + 256];
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            if (!initialized || !SymFromAddr(GetCurrentProcess(), rip, &displacement, symbol))
                return "?";
            return symbol->Name;
        }

        uint32_t GuestFromHost(uintptr_t rip)
        {
            auto it = std::upper_bound(g_ranges.begin(), g_ranges.end(), rip, [](uintptr_t v, const HostRange& r) { return v < r.begin; });
            if (it == g_ranges.begin())
                return 0;
            --it;
            return rip < it->end ? it->guest : 0;
        }

        void DumpRegisters(const PPCContext& c)
        {
            const PPCRegister* r[] = { &c.r0, &c.r1, &c.r2, &c.r3, &c.r4, &c.r5, &c.r6, &c.r7, &c.r8, &c.r9, &c.r10,
                &c.r11, &c.r12, &c.r13, &c.r14, &c.r15, &c.r16, &c.r17, &c.r18, &c.r19, &c.r20, &c.r21, &c.r22,
                &c.r23, &c.r24, &c.r25, &c.r26, &c.r27, &c.r28, &c.r29, &c.r30, &c.r31 };
            for (int i = 0; i < 32; i += 4)
                Log("  r%-2d %016llX  r%-2d %016llX  r%-2d %016llX  r%-2d %016llX", i, r[i]->u64, i + 1, r[i + 1]->u64,
                    i + 2, r[i + 2]->u64, i + 3, r[i + 3]->u64);
            Log("  lr  %08llX  ctr %016llX", c.lr, c.ctr.u64);
        }

        void DumpThread(HANDLE thread, uint32_t guestId, PPCContext* const* guestContext)
        {
            if (::SuspendThread(thread) == DWORD(-1))
                return;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context))
            {
                Log("--- guest thread %u ---", guestId);
                if (guestContext && *guestContext)
                    DumpRegisters(**guestContext);
                for (int depth = 0; depth < 48 && context.Rip; depth++)
                {
                    const uint32_t guest = GuestFromHost(context.Rip);
                    if (guest)
                        Log("  #%-2d sub_%08X", depth, guest);
                    else
                        Log("  #%-2d host %s", depth, HostSymbol(context.Rip));

                    DWORD64 imageBase;
                    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                    if (!fn)
                    {
                        // Leaf function: return address is at [rsp].
                        context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                        context.Rsp += 8;
                        continue;
                    }
                    void* handlerData;
                    DWORD64 establisher;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, fn, &context, &handlerData, &establisher, nullptr);
                }
            }
            ::ResumeThread(thread);
        }
    }

    void RegisterCurrentThread(uint32_t guestId, PPCContext* const* context)
    {
        HANDLE handle;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
        std::lock_guard lock(g_lock);
        g_threads.push_back(handle);
        g_threadIds.push_back(guestId);
        g_contexts.push_back(context);
    }

    void DumpAllThreads()
    {
        std::lock_guard lock(g_lock);
        BuildRanges();
        for (size_t i = 0; i < g_threads.size(); i++)
            DumpThread(g_threads[i], g_threadIds[i], g_contexts[i]);
    }

    namespace
    {
        LONG WINAPI CrashHandler(EXCEPTION_POINTERS* info)
        {
            const DWORD code = info->ExceptionRecord->ExceptionCode;
            // C++ exceptions (ThreadExit) and debugger notifications pass through.
            if (code == 0xE06D7363 || code == 0x406D1388 || code < 0x80000000)
                return EXCEPTION_CONTINUE_SEARCH;

            static std::mutex crashLock;
            std::lock_guard crash(crashLock);
            {
                std::lock_guard lock(g_lock);
                BuildRanges();
            }
            CONTEXT context = *info->ContextRecord;
            const uintptr_t faultAddress = info->ExceptionRecord->NumberParameters >= 2 ? info->ExceptionRecord->ExceptionInformation[1] : 0;
            Log("=== crash: exception 0x%08lX at %p, address %p (guest 0x%08llX) ===", code, (void*)context.Rip,
                (void*)faultAddress, (unsigned long long)(faultAddress - uintptr_t(mem::g_base)));
            if (GuestThread* thread = CurrentThread(); thread && thread->ctx)
            {
                Log("  guest thread %u: ctr=%08X lr=%08X", thread->id, thread->ctx->ctr.u32, uint32_t(thread->ctx->lr));
                DumpRegisters(*thread->ctx);
            }
            if (!context.Rip)
            {
                // Call through a null pointer: unwind the call itself first.
                context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                context.Rsp += 8;
            }
            for (int depth = 0; depth < 48 && context.Rip; depth++)
            {
                const uint32_t guest = GuestFromHost(context.Rip);
                if (guest)
                    Log("  #%-2d sub_%08X", depth, guest);
                else
                    Log("  #%-2d host %s", depth, HostSymbol(context.Rip));
                DWORD64 imageBase;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                if (!fn)
                {
                    context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                    context.Rsp += 8;
                    continue;
                }
                void* handlerData;
                DWORD64 establisher;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, fn, &context, &handlerData, &establisher, nullptr);
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    void InstallCrashHandler() { AddVectoredExceptionHandler(1, CrashHandler); }

    void TraceGuestWriter(void* hostContext, uint32_t physicalAddress)
    {
        static std::mutex traceLock;
        static std::vector<uint64_t> seen;
        std::lock_guard trace(traceLock);
        {
            std::lock_guard lock(g_lock);
            BuildRanges();
        }
        CONTEXT context = *static_cast<CONTEXT*>(hostContext);
        uint32_t chain[4] = {};
        for (int depth = 0, found = 0; depth < 32 && context.Rip && found < 4; depth++)
        {
            if (const uint32_t guest = GuestFromHost(context.Rip)) chain[found++] = guest;
            DWORD64 imageBase;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
            if (!fn) { context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp); context.Rsp += 8; continue; }
            void* handlerData; DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, fn, &context, &handlerData, &establisher, nullptr);
        }
        const uint64_t key = (uint64_t(chain[0]) << 32) | chain[1];
        if (std::find(seen.begin(), seen.end(), key) != seen.end() || seen.size() > 256) return;
        seen.push_back(key);
        const GuestThread* thread = CurrentThread();
        Log("write-trace phys %08X thread %u: sub_%08X <- sub_%08X <- sub_%08X <- sub_%08X", physicalAddress,
            thread ? thread->id : 0, chain[0], chain[1], chain[2], chain[3]);
    }

    void TraceGuestChain(const char* tag)
    {
        static std::mutex traceLock;
        static std::vector<std::array<uint32_t, 12>> seen;
        std::lock_guard trace(traceLock);
        {
            std::lock_guard lock(g_lock);
            BuildRanges();
        }
        CONTEXT context{};
        RtlCaptureContext(&context);
        std::array<uint32_t, 12> chain{};
        for (int depth = 0, found = 0; depth < 96 && context.Rip && found < 12; depth++)
        {
            if (const uint32_t guest = GuestFromHost(context.Rip)) chain[found++] = guest;
            DWORD64 imageBase;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
            if (!fn) { context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp); context.Rsp += 8; continue; }
            void* handlerData; DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, fn, &context, &handlerData, &establisher, nullptr);
        }
        if (std::find(seen.begin(), seen.end(), chain) != seen.end() || seen.size() > 64) return;
        seen.push_back(chain);
        const GuestThread* thread = CurrentThread();
        std::string text;
        for (uint32_t f : chain) if (f) { char item[16]; snprintf(item, sizeof(item), " <- %08X", f); text += item; }
        Log("chain %s thread %u:%s", tag, thread ? thread->id : 0, text.c_str());
    }

    // Sampling profiler for the main guest thread: counts, per function, how
    // often it is on top of the stack (self) and anywhere in it (inclusive).
    void StartProfiler(unsigned seconds)
    {
        std::thread([seconds] {
            // FD_PROFILE_DELAY=<seconds> (default 15) skips startup / loading.
            const char* delay = getenv("FD_PROFILE_DELAY");
            Sleep(DWORD((delay ? atoi(delay) : 15) * 1000));
            HANDLE thread;
            {
                std::lock_guard lock(g_lock);
                BuildRanges();
                // FD_PROFILE_THREAD=<guest thread id> picks another thread.
                const char* which = getenv("FD_PROFILE_THREAD");
                const uint32_t wanted = which ? uint32_t(atoi(which)) : 1;
                size_t index = 0;
                for (size_t i = 0; i < g_threadIds.size(); i++)
                {
                    if (g_threadIds[i] == wanted)
                        index = i;
                }
                if (g_threads.empty())
                    return;
                thread = g_threads[index];
                Log("profiling guest thread %u", g_threadIds[index]);
            }
            std::unordered_map<uint64_t, uint32_t> self, inclusive;
            std::map<std::pair<uint64_t, uint64_t>, uint32_t> callers; // (self, its caller)
            uint32_t samples = 0;
            const auto end = GetTickCount64() + seconds * 1000ull;
            timeBeginPeriod(1);
            while (GetTickCount64() < end)
            {
                if (::SuspendThread(thread) == DWORD(-1))
                    break;
                CONTEXT context{};
                context.ContextFlags = CONTEXT_FULL;
                // Walk the stack into a fixed buffer while the thread is
                // suspended and aggregate after resuming it: allocating here
                // deadlocks when the thread was suspended inside the heap.
                uint64_t frames[64];
                int depthCount = 0;
                if (GetThreadContext(thread, &context))
                {
                    for (; depthCount < 64 && context.Rip && context.Rsp >= 0x10000; depthCount++)
                    {
                        // Key: guest address, or host RIP (tagged) for runtime code.
                        const uint32_t guest = GuestFromHost(context.Rip);
                        DWORD64 imageBase = 0;
                        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                        uint64_t key = guest ? guest : (uint64_t(1) << 63) | context.Rip;
                        // Group host samples by function start.
                        if (!guest && fn) key = (uint64_t(1) << 63) | (imageBase + fn->BeginAddress);
                        frames[depthCount] = key;
                        if (!fn)
                        {
                            // A leaf frame: the return address is on top of the stack
                            // (read safely: a sample can land in a prologue / epilogue).
                            DWORD64 ret = 0;
                            SIZE_T got = 0;
                            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(context.Rsp), &ret, sizeof(ret), &got) ||
                                got != sizeof(ret))
                            {
                                ++depthCount;
                                break;
                            }
                            context.Rip = ret;
                            context.Rsp += 8;
                            continue;
                        }
                        void* handlerData;
                        DWORD64 establisher;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, fn, &context, &handlerData, &establisher, nullptr);
                    }
                }
                ::ResumeThread(thread);
                if (depthCount)
                {
                    ++samples;
                    ++self[frames[0]];
                    if (depthCount > 1) ++callers[{frames[0], frames[1]}];
                    for (int i = 0; i < depthCount; ++i)
                        if (std::find(frames, frames + i, frames[i]) == frames + i) ++inclusive[frames[i]];
                }
                Sleep(1);
            }
            timeEndPeriod(1);

            auto report = [&](const char* title, auto& counts) {
                std::vector<std::pair<uint64_t, uint32_t>> sorted(counts.begin(), counts.end());
                std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second > b.second; });
                Log("=== profile (%s), %u samples ===", title, samples);
                // FD_PROFILE_TOP=<n> lists more entries (default 30).
                static const size_t top = getenv("FD_PROFILE_TOP") ? size_t(atoi(getenv("FD_PROFILE_TOP"))) : 30;
                for (size_t i = 0; i < sorted.size() && i < top; i++)
                {
                    const uint64_t key = sorted[i].first;
                    const double pct = 100.0 * sorted[i].second / (samples ? samples : 1);
                    if (key >> 63)
                        Log("  %5.1f%%  host %s", pct, HostSymbol(uintptr_t(key & ~(uint64_t(1) << 63))));
                    else
                        Log("  %5.1f%%  sub_%08llX", pct, (unsigned long long)key);
                }
            };
            report("self", self);
            report("inclusive", inclusive);
            auto name = [](uint64_t key) -> std::string {
                char text[32];
                if (key >> 63) return HostSymbol(uintptr_t(key & ~(uint64_t(1) << 63)));
                snprintf(text, sizeof(text), "sub_%08llX", (unsigned long long)key);
                return text;
            };
            std::vector<std::pair<std::pair<uint64_t, uint64_t>, uint32_t>> pairs(callers.begin(), callers.end());
            std::sort(pairs.begin(), pairs.end(), [](auto& a, auto& b) { return a.second > b.second; });
            Log("=== profile (self <- caller), %u samples ===", samples);
            for (size_t i = 0; i < pairs.size() && i < 30; i++)
            {
                const std::string self = name(pairs[i].first.first), caller = name(pairs[i].first.second);
                Log("  %5.1f%%  %s  <-  %s", 100.0 * pairs[i].second / (samples ? samples : 1), self.c_str(), caller.c_str());
            }
        }).detach();
    }

    void StartWatchdog(unsigned seconds)
    {
        std::thread([seconds] {
            for (;;)
            {
                Sleep(seconds * 1000);
                Log("=== watchdog: guest stacks after %u s ===", seconds);
                DumpAllThreads();
                // FD_PEEK=addr[,addr...]: dump 16 dwords at each guest address.
                if (const char* peek = getenv("FD_PEEK"))
                {
                    for (const char* p = peek; *p;)
                    {
                        char* end;
                        const uint32_t address = uint32_t(strtoul(p, &end, 16));
                        for (uint32_t row = 0; row < 4; row++)
                        {
                            const uint32_t a = address + row * 16;
                            Log("  %08X: %08X %08X %08X %08X", a, mem::Read32(a), mem::Read32(a + 4), mem::Read32(a + 8), mem::Read32(a + 12));
                        }
                        p = *end ? end + 1 : end;
                    }
                }
            }
        }).detach();
    }
}
