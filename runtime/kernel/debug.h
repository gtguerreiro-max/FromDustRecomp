#pragma once

#include <cstdint>

struct PPCContext;

namespace kernel::debug
{
    // `context` points at the thread's live PPCContext pointer.
    void RegisterCurrentThread(uint32_t guestId, PPCContext* const* context);
    void DumpAllThreads();

    // Logs the guest call stack when the process crashes.
    void InstallCrashHandler();

    // Periodically dumps guest stacks (enabled with FD_WATCHDOG=<seconds>).
    void StartWatchdog(unsigned seconds);

    // Samples the main guest thread for N seconds (FD_PROFILE=<seconds>).
    void StartProfiler(unsigned seconds);
}
