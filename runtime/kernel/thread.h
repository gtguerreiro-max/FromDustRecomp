#pragma once

#include <cstdint>
#include <thread>
#include <vector>

#include "objects.h"

namespace kernel
{
    // Guest per-thread structures (offsets from Xenia's X_KPCR / X_KTHREAD).
    namespace pcr
    {
        constexpr uint32_t kTlsPtr = 0x000;
        constexpr uint32_t kSelf = 0x030;
        constexpr uint32_t kStackBase = 0x070; // high address
        constexpr uint32_t kStackLimit = 0x074; // low address
        constexpr uint32_t kCurrentThread = 0x100;
        constexpr uint32_t kCurrentCpu = 0x10C;
        constexpr uint32_t kDpcActive = 0x150;
        constexpr uint32_t kSize = 0x2D8;
    }

    namespace kthread
    {
        constexpr uint32_t kStackBase = 0x05C;
        constexpr uint32_t kStackLimit = 0x060;
        constexpr uint32_t kThreadId = 0x14C;
        constexpr uint32_t kSize = 0xAB0;
    }

    struct GuestThread : KernelObject
    {
        uint32_t id = 0;
        uint32_t pcrAddress = 0;
        uint32_t kthreadAddress = 0;
        uint32_t stackLow = 0, stackHigh = 0;
        uint32_t tlsStatic = 0;
        std::vector<uint32_t> tlsSlots; // KeTlsAlloc values

        uint32_t startAddress = 0, startContext = 0, xapiStartup = 0;
        uint32_t exitCode = 0;
        bool exited = false;   // dispatcher lock
        int suspendCount = 0;  // dispatcher lock
        int priority = 0;

        std::thread host;
        PPCContext* ctx = nullptr;

        GuestThread() : KernelObject(kThread) {}
        bool IsSignaled() const override { return exited; }
    };

    GuestThread* CurrentThread();

    // Creates (and optionally starts) a guest thread.
    GuestThread* CreateGuestThread(uint32_t stackSize, uint32_t startAddress, uint32_t startContext,
        uint32_t xapiStartup, bool suspended);
    void ResumeThread(GuestThread* thread);

    // Runs the XEX entry point on the calling host thread; returns on exit.
    void RunMainThread(uint32_t entryPoint, uint32_t stackSize);

    // Gives the calling host thread (e.g. the GPU) a guest identity so it can
    // call guest code (interrupt callbacks).
    GuestThread* AdoptHostThread(uint32_t stackSize);
    // Calls a guest function with r3/r4 on the current (adopted) guest thread.
    uint32_t CallGuestFunction(uint32_t address, uint32_t arg0, uint32_t arg1);

    // Unwinds the current guest thread back to its entry (ExTerminateThread).
    [[noreturn]] void ExitCurrentThread(uint32_t exitCode);

    // TLS layout from the main module's XEX header.
    void SetTlsInfo(uint32_t slotCount, uint32_t rawDataAddress, uint32_t dataSize, uint32_t rawDataSize);
}
