#include "kernel.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_set>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Mapping tables emitted by XenonRecomp, one per module (see recomp.ps1).
extern PPCFuncMapping PPCFuncMappings[];
extern PPCFuncMapping PPCFuncMappings_GalileoKernel[];
extern PPCFuncMapping PPCFuncMappings_GalileoPathfind_9_9[];
extern PPCFuncMapping PPCFuncMappings_GalileoSimulatorWater[];
extern PPCFuncMapping PPCFuncMappings_GalileoSimulatorWater_9_9[];
extern PPCFuncMapping PPCFuncMappings_GalileoTexture[];
extern PPCFuncMapping PPCFuncMappings_dustai_x360_f[];

extern "C" void** g_ppcFuncTable = nullptr;

namespace kernel
{
    namespace
    {
        std::mutex g_logMutex;
        FILE* g_logFile = nullptr;

        void VLog(const char* prefix, const char* fmt, va_list args)
        {
            char buffer[2048];
            vsnprintf(buffer, sizeof(buffer), fmt, args);

            std::lock_guard lock(g_logMutex);
            if (!g_logFile)
                g_logFile = fopen("FromDust.log", "w");
            fprintf(stdout, "[%5lu] %s%s\n", GetCurrentThreadId(), prefix, buffer);
            if (g_logFile)
            {
                fprintf(g_logFile, "[%5lu] %s%s\n", GetCurrentThreadId(), prefix, buffer);
                fflush(g_logFile);
            }
        }
    }

    void Log(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        VLog("", fmt, args);
        va_end(args);
    }

    void Fatal(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        VLog("FATAL: ", fmt, args);
        va_end(args);
        if (IsDebuggerPresent())
            __debugbreak();
        ExitProcess(1);
    }

    bool g_traceImports = [] {
        const char* value = getenv("FD_TRACE");
        return value && atoi(value) != 0;
    }();

    void TraceEnter(const char* name, const PPCContext& ctx)
    {
        Log("> %s(%08X, %08X, %08X, %08X, %08X) lr=%08X", name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
            ctx.r7.u32, uint32_t(ctx.lr));
    }

    void TraceLeave(const char* name, const PPCContext& ctx) { Log("< %s = %08X", name, ctx.r3.u32); }

    void LogUnimplemented(const char* name)
    {
        static std::mutex mutex;
        static std::unordered_set<std::string> seen;
        {
            std::lock_guard lock(mutex);
            if (!seen.insert(name).second)
                return;
        }
        Log("unimplemented: %s", name);
    }

    bool InitFunctionTable()
    {
        const size_t entries = (FD_GUEST_CODE_HIGH - FD_GUEST_CODE_LOW) / 4;
        g_ppcFuncTable = static_cast<void**>(VirtualAlloc(nullptr, entries * sizeof(void*), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!g_ppcFuncTable)
            return false;

        size_t count = 0;
        for (PPCFuncMapping* table : { PPCFuncMappings, PPCFuncMappings_GalileoKernel, PPCFuncMappings_GalileoPathfind_9_9,
                 PPCFuncMappings_GalileoSimulatorWater, PPCFuncMappings_GalileoSimulatorWater_9_9,
                 PPCFuncMappings_GalileoTexture, PPCFuncMappings_dustai_x360_f })
        {
            for (PPCFuncMapping* m = table; m->host; ++m, ++count)
                g_ppcFuncTable[(uint32_t(m->guest) - FD_GUEST_CODE_LOW) >> 2] = reinterpret_cast<void*>(m->host);
        }
        Log("function table: %zu recompiled functions", count);
        return true;
    }

    PPCFunc* FindFunction(uint32_t address)
    {
        if (address < FD_GUEST_CODE_LOW || address >= FD_GUEST_CODE_HIGH || (address & 3))
            return nullptr;
        return reinterpret_cast<PPCFunc*>(g_ppcFuncTable[(address - FD_GUEST_CODE_LOW) >> 2]);
    }

    void CallGuest(PPCContext& ctx, uint32_t address)
    {
        PPCFunc* fn = FindFunction(address);
        if (!fn)
            Fatal("call to unknown guest function 0x%08X", address);
        fn(ctx, mem::g_base);
    }
}
