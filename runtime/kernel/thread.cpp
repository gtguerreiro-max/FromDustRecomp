#include "thread.h"

#include <atomic>

#include "debug.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace kernel
{
    namespace
    {
        thread_local GuestThread* t_current = nullptr;
        std::atomic<uint32_t> g_nextThreadId{ 1 };

        uint32_t g_tlsSlotCount = 64;
        uint32_t g_tlsRawData = 0, g_tlsDataSize = 0, g_tlsRawSize = 0;

        struct ThreadExit
        {
            uint32_t code;
        };

        void SetupGuestState(GuestThread* thread, uint32_t stackSize)
        {
            stackSize = (stackSize + 0xFFFF) & ~0xFFFFu;
            if (stackSize < 0x10000)
                stackSize = 0x10000;
            thread->stackLow = mem::AllocVirtual(stackSize, 0x10000);
            thread->stackHigh = thread->stackLow + stackSize;

            // Static TLS (__declspec(thread) data) is reached through 0(r13).
            const uint32_t tlsSize = ((g_tlsDataSize + 15) & ~15u) + g_tlsSlotCount * 4;
            thread->tlsStatic = mem::AllocSystem(tlsSize ? tlsSize : 16);
            if (g_tlsRawSize)
                memcpy(mem::Translate(thread->tlsStatic), mem::Translate(g_tlsRawData), g_tlsRawSize);

            thread->kthreadAddress = mem::AllocSystem(kthread::kSize);
            mem::Ptr<uint8_t>(thread->kthreadAddress)[0] = kThread;
            mem::Write32(thread->kthreadAddress + kthread::kStackBase, thread->stackHigh);
            mem::Write32(thread->kthreadAddress + kthread::kStackLimit, thread->stackLow);
            mem::Write32(thread->kthreadAddress + kthread::kThreadId, thread->id);

            thread->pcrAddress = mem::AllocSystem(pcr::kSize, 0x1000);
            mem::Write32(thread->pcrAddress + pcr::kTlsPtr, thread->tlsStatic);
            mem::Write32(thread->pcrAddress + pcr::kSelf, thread->pcrAddress);
            mem::Write32(thread->pcrAddress + pcr::kStackBase, thread->stackHigh);
            mem::Write32(thread->pcrAddress + pcr::kStackLimit, thread->stackLow);
            mem::Write32(thread->pcrAddress + pcr::kCurrentThread, thread->kthreadAddress);
            mem::Ptr<uint8_t>(thread->pcrAddress)[pcr::kCurrentCpu] = uint8_t(thread->id % 6);

            // Ke* thread APIs receive the KTHREAD pointer.
            RegisterGuestObject(thread->kthreadAddress, thread);
        }

        void InitContext(GuestThread* thread, PPCContext& ctx)
        {
            ctx.r1.u64 = (thread->stackHigh - 0x100) & ~0xFull;
            ctx.r13.u64 = thread->pcrAddress;
            ctx.fpscr.loadFromHost();
            thread->ctx = &ctx;
        }

        void RunGuest(GuestThread* thread, uint32_t function, uint32_t arg0, uint32_t arg1)
        {
            PPCContext ctx{};
            InitContext(thread, ctx);
            ctx.r3.u64 = arg0;
            ctx.r4.u64 = arg1;
            try
            {
                CallGuest(ctx, function);
                thread->exitCode = ctx.r3.u32;
            }
            catch (const ThreadExit& e)
            {
                thread->exitCode = e.code;
            }
        }

        void ThreadMain(GuestThread* thread)
        {
            t_current = thread;
            debug::RegisterCurrentThread(thread->id, &thread->ctx);
            {
                // Wait while suspended (CREATE_SUSPENDED / NtSuspendThread).
                std::unique_lock lock(g_dispatcherLock);
                g_dispatcherCv.wait(lock, [&] { return thread->suspendCount == 0; });
            }

            if (thread->xapiStartup)
                RunGuest(thread, thread->xapiStartup, thread->startAddress, thread->startContext);
            else
                RunGuest(thread, thread->startAddress, thread->startContext, 0);

            {
                std::lock_guard lock(g_dispatcherLock);
                thread->exited = true;
            }
            NotifyObject(thread);
            Log("thread %u exited with 0x%X", thread->id, thread->exitCode);
        }
    }

    GuestThread* CurrentThread() { return t_current; }

    void SetTlsInfo(uint32_t slotCount, uint32_t rawDataAddress, uint32_t dataSize, uint32_t rawDataSize)
    {
        g_tlsSlotCount = slotCount ? slotCount : 64;
        g_tlsRawData = rawDataAddress;
        g_tlsDataSize = dataSize;
        g_tlsRawSize = rawDataSize;
    }

    GuestThread* CreateGuestThread(uint32_t stackSize, uint32_t startAddress, uint32_t startContext,
        uint32_t xapiStartup, bool suspended)
    {
        auto* thread = new GuestThread();
        thread->id = g_nextThreadId++;
        thread->startAddress = startAddress;
        thread->startContext = startContext;
        thread->xapiStartup = xapiStartup;
        thread->suspendCount = suspended ? 1 : 0;
        SetupGuestState(thread, stackSize ? stackSize : 0x40000);

        Log("thread %u created: start=0x%08X ctx=0x%08X xapi=0x%08X%s", thread->id, startAddress, startContext,
            xapiStartup, suspended ? " (suspended)" : "");
        thread->host = std::thread(ThreadMain, thread);
        thread->host.detach();
        return thread;
    }

    void ResumeThread(GuestThread* thread)
    {
        {
            std::lock_guard lock(g_dispatcherLock);
            if (thread->suspendCount > 0)
                --thread->suspendCount;
        }
        NotifyWaiters();
    }

    void RunMainThread(uint32_t entryPoint, uint32_t stackSize)
    {
        auto* thread = new GuestThread();
        thread->id = g_nextThreadId++;
        thread->startAddress = entryPoint;
        SetupGuestState(thread, stackSize);
        t_current = thread;
        debug::RegisterCurrentThread(thread->id, &thread->ctx);

        Log("main thread %u: entry 0x%08X, stack 0x%08X-0x%08X", thread->id, entryPoint, thread->stackLow, thread->stackHigh);
        RunGuest(thread, entryPoint, 0, 0);
        Log("main thread returned 0x%X", thread->exitCode);
    }

    GuestThread* AdoptHostThread(uint32_t stackSize)
    {
        auto* thread = new GuestThread();
        thread->id = g_nextThreadId++;
        SetupGuestState(thread, stackSize);
        t_current = thread;
        debug::RegisterCurrentThread(thread->id, &thread->ctx);
        Log("host thread adopted as guest thread %u", thread->id);
        return thread;
    }

    uint32_t CallGuestFunction(uint32_t address, uint32_t arg0, uint32_t arg1)
    {
        GuestThread* thread = t_current;
        PPCContext ctx{};
        PPCContext* outer = thread->ctx;
        InitContext(thread, ctx);
        if (outer)
            ctx.r1.u64 = (outer->r1.u64 - 0x200) & ~0xFull; // nested call: stay below the caller's frame
        ctx.r3.u64 = arg0;
        ctx.r4.u64 = arg1;
        CallGuest(ctx, address);
        thread->ctx = outer;
        return ctx.r3.u32;
    }

    void ExitCurrentThread(uint32_t exitCode)
    {
        throw ThreadExit{ exitCode };
    }
}
