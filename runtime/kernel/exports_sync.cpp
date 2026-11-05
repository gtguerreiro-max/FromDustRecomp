// Threads, waits, events, semaphores, mutants, critical sections, TLS, time.

#include <atomic>
#include <algorithm>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>
#include <thread>

#include "kernel.h"
#include "objects.h"
#include "thread.h"
#include "debug.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

using namespace kernel;

namespace
{
    uint32_t WaitHandles(const uint32_t* handles, uint32_t count, bool waitAll, uint32_t timeoutPtr)
    {
        KernelObject* objects[64];
        if (count > 64)
            return STATUS_INVALID_PARAMETER;
        for (uint32_t i = 0; i < count; i++)
        {
            objects[i] = LookupHandle(handles[i]);
            if (!objects[i])
            {
                Log("wait on invalid handle 0x%08X", handles[i]);
                return STATUS_INVALID_HANDLE;
            }
        }
        Deadline deadline;
        const bool timed = TimeoutToDeadline(timeoutPtr, deadline);
        return WaitForObjects(objects, count, waitAll, timed ? &deadline : nullptr);
    }

    uint32_t SetEvent(KernelObject* object)
    {
        if (!object || (object->type != kNotificationEvent && object->type != kSynchronizationEvent))
            return 0;
        uint32_t previous;
        {
            std::lock_guard lock(g_dispatcherLock);
            auto* event = static_cast<Event*>(object);
            previous = event->signaled;
            event->signaled = true;
        }
        NotifyObject(object);
        return previous;
    }

    uint32_t ResetEvent(KernelObject* object)
    {
        if (!object || (object->type != kNotificationEvent && object->type != kSynchronizationEvent))
            return 0;
        std::lock_guard lock(g_dispatcherLock);
        auto* event = static_cast<Event*>(object);
        const uint32_t previous = event->signaled;
        event->signaled = false;
        return previous;
    }

    GuestThread* ThreadFromGuest(uint32_t pointer)
    {
        KernelObject* object = pointer ? ObjectFromGuest(pointer) : CurrentThread();
        return object && object->type == kThread ? static_cast<GuestThread*>(object) : nullptr;
    }

    // mftb is recompiled as __rdtsc(), so the performance frequency is the TSC's.
    uint64_t MeasureTscFrequency()
    {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        const uint64_t t0 = __rdtsc();
        Sleep(50);
        QueryPerformanceCounter(&b);
        const uint64_t t1 = __rdtsc();
        return uint64_t(double(t1 - t0) * double(f.QuadPart) / double(b.QuadPart - a.QuadPart));
    }
}

// --- Threads -------------------------------------------------------------

KFUNC(ExCreateThread)
{
    const uint32_t handlePtr = ARG0, stackSize = ARG1, threadIdPtr = ARG2, xapiStartup = ARG3;
    const uint32_t startAddress = ARG4, startContext = ARG5, flags = ARG6;
    GuestThread* thread = CreateGuestThread(stackSize, startAddress, startContext, xapiStartup, (flags & 1) != 0);
    if (handlePtr)
        mem::Write32(handlePtr, CreateHandle(thread));
    if (threadIdPtr)
        mem::Write32(threadIdPtr, thread->id);
    RETURN(STATUS_SUCCESS);
}

KFUNC(ExTerminateThread) { ExitCurrentThread(ARG0); }

KFUNC(NtResumeThread)
{
    auto* thread = LookupHandle<GuestThread>(ARG0, kThread);
    if (!thread)
        RETURN(STATUS_INVALID_HANDLE);
    if (ARG1)
        mem::Write32(ARG1, thread->suspendCount);
    ResumeThread(thread);
    RETURN(STATUS_SUCCESS);
}

KFUNC(NtSuspendThread)
{
    auto* thread = LookupHandle<GuestThread>(ARG0, kThread);
    if (!thread)
        RETURN(STATUS_INVALID_HANDLE);
    if (thread != CurrentThread())
        Log("NtSuspendThread on another running thread is not supported (thread %u)", thread->id);
    if (ARG1)
        mem::Write32(ARG1, thread->suspendCount);
    RETURN(STATUS_SUCCESS);
}

KFUNC(KeSetAffinityThread)
{
    // Host scheduling decides; report the previous mask as "all six cores".
    if (ARG2)
        mem::Write32(ARG2, 0x3F);
    RETURN(0x3F);
}

KFUNC(KeSetBasePriorityThread)
{
    GuestThread* thread = ThreadFromGuest(ARG0);
    const int previous = thread ? thread->priority : 0;
    if (thread)
        thread->priority = int32_t(ARG1);
    RETURN(uint32_t(previous));
}

KFUNC(KeQueryBasePriorityThread)
{
    GuestThread* thread = ThreadFromGuest(ARG0);
    RETURN(thread ? uint32_t(thread->priority) : 0);
}

KFUNC(KeSetDisableBoostThread) { RETURN(0); }
KFUNC(KeGetCurrentProcessType) { RETURN(1); } // title process
KFUNC(KeSetCurrentProcessType) { RETURN(0); }
KFUNC(KeEnableFpuExceptions) { RETURN(0); }
KFUNC(KeEnterCriticalRegion) { RETURN(0); }
KFUNC(KeLeaveCriticalRegion) { RETURN(0); }
KFUNC(KiApcNormalRoutineNop) { RETURN(0); }

KFUNC(NtYieldExecution)
{
    WaitTimer timer(1);
    std::this_thread::yield();
    RETURN(STATUS_SUCCESS);
}

KFUNC(KeDelayExecutionThread)
{
    const int64_t interval = int64_t(*mem::Ptr<be<uint64_t>>(ARG2));
    WaitTimer timer(1);
    if (interval == 0)
    {
        std::this_thread::yield();
    }
    else
    {
        Deadline deadline;
        TimeoutToDeadline(interval, deadline);
        std::this_thread::sleep_until(deadline);
    }
    RETURN(STATUS_SUCCESS);
}

KFUNC(ObReferenceObjectByHandle)
{
    // (handle, object_type, out_object) -> guest pointer for the object.
    KernelObject* object = LookupHandle(ARG0);
    if (!object)
        RETURN(STATUS_INVALID_HANDLE);
    uint32_t guest = 0;
    if (object->type == kThread)
        guest = static_cast<GuestThread*>(object)->kthreadAddress;
    else if (ARG0 < 0xF8000000)
        guest = ARG0;
    if (!guest)
    {
        Log("ObReferenceObjectByHandle: no guest object for handle 0x%08X (type %u)", ARG0, object->type);
        RETURN(STATUS_OBJECT_TYPE_MISMATCH);
    }
    if (ARG2)
        mem::Write32(ARG2, guest);
    RETURN(STATUS_SUCCESS);
}

KFUNC(ObDereferenceObject) { RETURN(0); }
KFUNC(ObReferenceObject) { RETURN(0); }

// --- Events / semaphores / mutants ----------------------------------------


// FD_TRACE_SYNC=1: every 10 s, the most frequent (operation, thread, guest
// caller, object) tuples of event / semaphore signals and waits. Used to find
// which thread drives which (for example the per-frame simulation steps).
namespace
{
    struct SyncTrace
    {
        std::mutex lock;
        std::map<std::tuple<char, uint32_t, uint32_t, const void*>, uint32_t> counts;
        uint64_t lastDump = 0;
    };
    void TraceSync(char op, uint32_t lr, const void* object)
    {
        static const bool enabled = getenv("FD_TRACE_SYNC") != nullptr;
        if (!enabled) return;
        static SyncTrace trace;
        const GuestThread* thread = CurrentThread();
        // FD_TRACE_SYNC_CHAIN=<thread id>: guest call chains of that thread's signals.
        static const int chainThread = [] { const char* v = getenv("FD_TRACE_SYNC_CHAIN"); return v ? atoi(v) : -1; }();
        if (thread && int(thread->id) == chainThread && op != 'W')
        {
            char tag[16]; snprintf(tag, sizeof(tag), "%c%p", op, object);
            debug::TraceGuestChain(tag);
        }
        std::lock_guard guard(trace.lock);
        ++trace.counts[{op, thread ? thread->id : 0, lr, object}];
        const uint64_t now = GetTickCount64();
        if (!trace.lastDump) trace.lastDump = now;
        if (now - trace.lastDump < 10000) return;
        std::vector<std::pair<uint32_t, std::tuple<char, uint32_t, uint32_t, const void*>>> sorted;
        for (const auto& [key, count] : trace.counts) sorted.push_back({count, key});
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        Log("=== sync trace (%.1f s) ===", (now - trace.lastDump) / 1000.0);
        for (size_t i = 0; i < sorted.size() && i < 40; i++)
        {
            const auto& [op, id, caller, object] = sorted[i].second;
            Log("  %c thread %2u lr %08X object %p: %u (%.1f/s)", op, id, caller, object, sorted[i].first,
                sorted[i].first * 1000.0 / double(now - trace.lastDump));
        }
        trace.counts.clear();
        trace.lastDump = now;
    }
}

KFUNC(NtCreateEvent)
{
    // (handle_ptr, object_attributes, event_type, initial_state)
    auto* event = new Event(ARG2 == 0, ARG3 != 0);
    mem::Write32(ARG0, CreateHandle(event));
    RETURN(STATUS_SUCCESS);
}

KFUNC(NtSetEvent)
{
    KernelObject* object = LookupHandle(ARG0);
    if (!object)
        RETURN(STATUS_INVALID_HANDLE);
    TraceSync('S', ctx.lr, object);
    const uint32_t previous = SetEvent(object);
    if (ARG1)
        mem::Write32(ARG1, previous);
    RETURN(STATUS_SUCCESS);
}

KFUNC(NtClearEvent)
{
    KernelObject* object = LookupHandle(ARG0);
    if (!object)
        RETURN(STATUS_INVALID_HANDLE);
    ResetEvent(object);
    RETURN(STATUS_SUCCESS);
}

KFUNC(NtPulseEvent)
{
    KernelObject* object = LookupHandle(ARG0);
    if (!object)
        RETURN(STATUS_INVALID_HANDLE);
    const uint32_t previous = SetEvent(object);
    // Give current waiters a chance to wake before the reset.
    std::this_thread::yield();
    ResetEvent(object);
    if (ARG1)
        mem::Write32(ARG1, previous);
    RETURN(STATUS_SUCCESS);
}

KFUNC(KeSetEvent) { KernelObject* object = ObjectFromGuest(ARG0); TraceSync('S', ctx.lr, object); RETURN(SetEvent(object)); }
KFUNC(KeResetEvent) { RETURN(ResetEvent(ObjectFromGuest(ARG0))); }

KFUNC(NtCreateSemaphore)
{
    // (handle_ptr, object_attributes, initial_count, maximum_count)
    auto* semaphore = new Semaphore(int32_t(ARG2), int32_t(ARG3));
    mem::Write32(ARG0, CreateHandle(semaphore));
    RETURN(STATUS_SUCCESS);
}

KFUNC(NtReleaseSemaphore)
{
    auto* semaphore = LookupHandle<Semaphore>(ARG0, kSemaphore);
    if (!semaphore)
        RETURN(STATUS_INVALID_HANDLE);
    TraceSync('R', ctx.lr, semaphore);
    int32_t previous;
    {
        std::lock_guard lock(g_dispatcherLock);
        previous = semaphore->count;
        if (semaphore->count + int32_t(ARG1) > semaphore->limit)
            RETURN(STATUS_SEMAPHORE_LIMIT_EXCEEDED);
        semaphore->count += int32_t(ARG1);
    }
    NotifyObject(semaphore);
    if (ARG2)
        mem::Write32(ARG2, uint32_t(previous));
    RETURN(STATUS_SUCCESS);
}

KFUNC(NtCreateMutant)
{
    // (handle_ptr, object_attributes, initial_owner)
    auto* mutant = new Mutant(ARG2 != 0);
    mem::Write32(ARG0, CreateHandle(mutant));
    RETURN(STATUS_SUCCESS);
}

KFUNC(NtReleaseMutant)
{
    auto* mutant = LookupHandle<Mutant>(ARG0, kMutant);
    if (!mutant)
        RETURN(STATUS_INVALID_HANDLE);
    const uint32_t me = CurrentThread() ? CurrentThread()->id : 0;
    uint32_t previous;
    {
        std::lock_guard lock(g_dispatcherLock);
        if (mutant->owner != me || mutant->recursion == 0)
            RETURN(STATUS_MUTANT_NOT_OWNED);
        previous = mutant->recursion--;
        if (mutant->recursion == 0)
            mutant->owner = 0;
    }
    NotifyObject(mutant);
    if (ARG1)
        mem::Write32(ARG1, previous);
    RETURN(STATUS_SUCCESS);
}

KFUNC(NtDuplicateObject)
{
    // (handle, out_handle_ptr, options)
    KernelObject* object = LookupHandle(ARG0);
    if (!object)
        RETURN(STATUS_INVALID_HANDLE);
    ++object->refs;
    if (ARG1)
        mem::Write32(ARG1, CreateHandle(object));
    RETURN(STATUS_SUCCESS);
}

KFUNC(NtClose) { RETURN(CloseHandle(ARG0) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE); }

// --- Waits ------------------------------------------------------------------

KFUNC(NtWaitForSingleObjectEx)
{
    // (handle, wait_mode, alertable, timeout_ptr)
    const uint32_t handle = ARG0;
    TraceSync('W', ctx.lr, LookupHandle(handle));
    RETURN(WaitHandles(&handle, 1, true, ARG3));
}

KFUNC(NtWaitForMultipleObjectsEx)
{
    // (count, handles_ptr, wait_type, wait_mode, alertable, timeout_ptr); wait_type 0 = all
    uint32_t handles[64];
    const uint32_t count = ARG0 > 64 ? 64 : ARG0;
    for (uint32_t i = 0; i < count; i++)
        handles[i] = mem::Read32(ARG1 + i * 4);
    RETURN(WaitHandles(handles, count, ARG2 == 0, ARG5));
}

KFUNC(NtSignalAndWaitForSingleObjectEx)
{
    // (signal_handle, wait_handle, wait_mode, alertable, timeout_ptr)
    KernelObject* signal = LookupHandle(ARG0);
    if (!signal)
        RETURN(STATUS_INVALID_HANDLE);
    if (signal->type == kNotificationEvent || signal->type == kSynchronizationEvent)
    {
        SetEvent(signal);
    }
    else if (signal->type == kSemaphore)
    {
        std::lock_guard lock(g_dispatcherLock);
        ++static_cast<Semaphore*>(signal)->count;
    }
    else if (signal->type == kMutant)
    {
        std::lock_guard lock(g_dispatcherLock);
        auto* mutant = static_cast<Mutant*>(signal);
        if (mutant->recursion && --mutant->recursion == 0)
            mutant->owner = 0;
    }
    NotifyObject(signal);
    const uint32_t handle = ARG1;
    RETURN(WaitHandles(&handle, 1, true, ARG4));
}

KFUNC(KeWaitForSingleObject)
{
    // (object_ptr, wait_reason, wait_mode, alertable, timeout_ptr)
    KernelObject* object = ObjectFromGuest(ARG0);
    if (!object)
        RETURN(STATUS_INVALID_HANDLE);
    TraceSync('W', ctx.lr, object);
    Deadline deadline;
    const bool timed = TimeoutToDeadline(ARG4, deadline);
    RETURN(WaitForObjects(&object, 1, true, timed ? &deadline : nullptr));
}

KFUNC(KeWaitForMultipleObjects)
{
    // (count, objects_ptr, wait_type, reason, mode, alertable, timeout_ptr, wait_blocks)
    KernelObject* objects[64];
    const uint32_t count = ARG0 > 64 ? 64 : ARG0;
    for (uint32_t i = 0; i < count; i++)
    {
        objects[i] = ObjectFromGuest(mem::Read32(ARG1 + i * 4));
        if (!objects[i])
            RETURN(STATUS_INVALID_HANDLE);
    }
    Deadline deadline;
    const bool timed = TimeoutToDeadline(ARG6, deadline);
    RETURN(WaitForObjects(objects, count, ARG2 == 0, timed ? &deadline : nullptr));
}

// --- Critical sections --------------------------------------------------------
// RTL_CRITICAL_SECTION: 16-byte header, LockCount @0x10, RecursionCount @0x14,
// OwningThread @0x18. The lock word only ever holds -1 (free) or 0 (taken),
// which read the same in either byte order, so it can be used natively with
// WaitOnAddress.
//
// Fairness: the header's SignalState (+4, unused by this implementation of the
// Rtl functions; only the kernel touches it) counts waiting threads. When a
// thread leaves with waiters present, it hands the section over (the lock
// stays taken and the owner becomes kCsHandoff) instead of releasing it, so a
// thread that re-enters every frame (the main thread) cannot starve the others.
// Without this the level loader could wait on the main thread forever.

namespace
{
    constexpr uint32_t kCsWaiters = 0x04, kCsLock = 0x10, kCsRecursion = 0x14, kCsOwner = 0x18;
    constexpr uint32_t kCsHandoff = 0xFFFFFFFE;

    void InitCriticalSection(uint32_t cs)
    {
        memset(mem::Translate(cs), 0, 0x1C);
        mem::Ptr<uint8_t>(cs)[0] = kSynchronizationEvent;
        mem::Write32(cs + kCsLock, 0xFFFFFFFF);
    }

    uint32_t CurrentOwnerTag()
    {
        GuestThread* thread = CurrentThread();
        return thread ? thread->kthreadAddress : 1;
    }

    bool TryEnter(uint32_t cs, uint32_t me)
    {
        auto* lock = mem::Ptr<volatile long>(cs + kCsLock);
        if (_InterlockedCompareExchange(lock, 0, -1) != -1)
            return false;
        mem::Write32(cs + kCsOwner, me);
        mem::Write32(cs + kCsRecursion, 1);
        return true;
    }

    // A section handed over by its last owner: claimed by exactly one waiter.
    bool TryClaimHandoff(uint32_t cs, uint32_t me)
    {
        auto* owner = mem::Ptr<volatile long>(cs + kCsOwner);
        // The owner field is big-endian guest memory (read with mem::Read32).
        const long handoff = long(_byteswap_ulong(kCsHandoff)), mine = long(_byteswap_ulong(me));
        if (_InterlockedCompareExchange(owner, mine, handoff) != handoff)
            return false;
        mem::Write32(cs + kCsRecursion, 1);
        return true;
    }
}

KFUNC(RtlInitializeCriticalSection) { InitCriticalSection(ARG0); RETURN(0); }
KFUNC(RtlInitializeCriticalSectionAndSpinCount) { InitCriticalSection(ARG0); RETURN(STATUS_SUCCESS); }

KFUNC(RtlEnterCriticalSection)
{
    const uint32_t cs = ARG0, me = CurrentOwnerTag();
    if (mem::Read32(cs + kCsOwner) == me)
    {
        mem::Write32(cs + kCsRecursion, mem::Read32(cs + kCsRecursion) + 1);
        RETURN(0);
    }
    for (int spin = 0; spin < 128; spin++)
    {
        if (TryEnter(cs, me)) RETURN(0);
        _mm_pause();
    }
    auto* lock = mem::Ptr<volatile long>(cs + kCsLock);
    auto* waiters = mem::Ptr<volatile long>(cs + kCsWaiters);
    WaitTimer timer(2);
    _InterlockedIncrement(waiters);
    for (int wait = 0; !TryClaimHandoff(cs, me) && !TryEnter(cs, me); wait++)
    {
        long taken = 0;
        WaitOnAddress(lock, &taken, sizeof(taken), 10);
        // Diagnostics: report a critical section held for more than 5 s once.
        if (wait == 500)
        {
            const uint32_t owner = mem::Read32(cs + kCsOwner);
            const GuestThread* self = CurrentThread();
            Log("critical section %08X: thread %u waits 5 s; owner kthread %08X (thread %u), recursion %u",
                cs, self ? self->id : 0, owner, owner > 1 && owner != kCsHandoff ? mem::Read32(owner + 0x14C) : 0,
                mem::Read32(cs + kCsRecursion));
        }
    }
    _InterlockedDecrement(waiters);
    RETURN(0);
}

KFUNC(RtlTryEnterCriticalSection)
{
    const uint32_t cs = ARG0, me = CurrentOwnerTag();
    if (mem::Read32(cs + kCsOwner) == me)
    {
        mem::Write32(cs + kCsRecursion, mem::Read32(cs + kCsRecursion) + 1);
        RETURN(1);
    }
    RETURN(TryEnter(cs, me) ? 1 : 0);
}

KFUNC(RtlLeaveCriticalSection)
{
    const uint32_t cs = ARG0;
    const uint32_t recursion = mem::Read32(cs + kCsRecursion);
    if (recursion > 1)
    {
        mem::Write32(cs + kCsRecursion, recursion - 1);
        RETURN(0);
    }
    mem::Write32(cs + kCsRecursion, 0);
    auto* lock = mem::Ptr<volatile long>(cs + kCsLock);
    auto* waiters = mem::Ptr<volatile long>(cs + kCsWaiters);
    if (*waiters > 0)
    {
        // Hand over: the lock stays taken; one waiter claims the owner slot.
        _InterlockedExchange(mem::Ptr<volatile long>(cs + kCsOwner), long(_byteswap_ulong(kCsHandoff)));
        WakeByAddressAll((void*)lock);
        RETURN(0);
    }
    mem::Write32(cs + kCsOwner, 0);
    _InterlockedExchange(lock, -1);
    // A waiter may have registered between the check and the release.
    if (*waiters > 0) WakeByAddressAll((void*)lock);
    else WakeByAddressSingle((void*)lock);
    RETURN(0);
}

// --- Spin locks (guest u32, native order: only the kernel touches them) ------

namespace
{
    void AcquireSpin(uint32_t lockAddress)
    {
        auto* lock = mem::Ptr<volatile long>(lockAddress);
        const long me = long(CurrentOwnerTag());
        for (int spin = 0; _InterlockedCompareExchange(lock, me, 0) != 0; spin++)
        {
            if (spin < 256)
                _mm_pause();
            else
                std::this_thread::yield();
        }
    }

    void ReleaseSpin(uint32_t lockAddress) { _InterlockedExchange(mem::Ptr<volatile long>(lockAddress), 0); }
}

KFUNC(KfAcquireSpinLock) { AcquireSpin(ARG0); RETURN(0); }
KFUNC(KfReleaseSpinLock) { ReleaseSpin(ARG0); RETURN(0); }
KFUNC(KeAcquireSpinLockAtRaisedIrql) { AcquireSpin(ARG0); RETURN(0); }
KFUNC(KeReleaseSpinLockFromRaisedIrql) { ReleaseSpin(ARG0); RETURN(0); }

// --- TLS ------------------------------------------------------------------------

namespace
{
    std::mutex g_tlsLock;
    std::vector<bool> g_tlsUsed;
}

KFUNC(KeTlsAlloc)
{
    std::lock_guard lock(g_tlsLock);
    for (size_t i = 0; i < g_tlsUsed.size(); i++)
    {
        if (!g_tlsUsed[i])
        {
            g_tlsUsed[i] = true;
            RETURN(uint32_t(i));
        }
    }
    g_tlsUsed.push_back(true);
    RETURN(uint32_t(g_tlsUsed.size() - 1));
}

KFUNC(KeTlsFree)
{
    std::lock_guard lock(g_tlsLock);
    if (ARG0 < g_tlsUsed.size())
        g_tlsUsed[ARG0] = false;
    RETURN(1);
}

KFUNC(KeTlsGetValue)
{
    GuestThread* thread = CurrentThread();
    RETURN(thread && ARG0 < thread->tlsSlots.size() ? thread->tlsSlots[ARG0] : 0);
}

KFUNC(KeTlsSetValue)
{
    GuestThread* thread = CurrentThread();
    if (!thread)
        RETURN(0);
    if (ARG0 >= thread->tlsSlots.size())
        thread->tlsSlots.resize(ARG0 + 1, 0);
    thread->tlsSlots[ARG0] = ARG1;
    RETURN(1);
}

// --- Time -----------------------------------------------------------------------

KFUNC(KeQueryPerformanceFrequency)
{
    static const uint64_t frequency = MeasureTscFrequency();
    RETURN(frequency);
}

KFUNC(KeQuerySystemTime)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    *mem::Ptr<be<uint64_t>>(ARG0) = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    RETURN(0);
}

// --- DPCs: deferred procedure calls run on the calling thread -----------------
// KDPC: type @0, ... DeferredRoutine @0xC, DeferredContext @0x10.

KFUNC(KeInitializeDpc)
{
    // (dpc, routine, context)
    memset(mem::Translate(ARG0), 0, 0x1C);
    mem::Ptr<uint8_t>(ARG0)[0] = 19;
    mem::Write32(ARG0 + 0x0C, ARG1);
    mem::Write32(ARG0 + 0x10, ARG2);
    RETURN(0);
}

KFUNC(KeInsertQueueDpc)
{
    // (dpc, arg1, arg2): routine(dpc, context, arg1, arg2)
    const uint32_t dpc = ARG0, routine = mem::Read32(dpc + 0x0C), context = mem::Read32(dpc + 0x10);
    PPCContext saved = ctx;
    ctx.r3.u64 = dpc;
    ctx.r4.u64 = context;
    ctx.r5.u64 = saved.r4.u64;
    ctx.r6.u64 = saved.r5.u64;
    CallGuest(ctx, routine);
    ctx = saved;
    RETURN(1);
}

KFUNC(KeBugCheck) { Fatal("KeBugCheck(0x%08X)", ARG0); }
KFUNC(KeBugCheckEx) { Fatal("KeBugCheckEx(0x%08X, 0x%08X, 0x%08X, 0x%08X, 0x%08X)", ARG0, ARG1, ARG2, ARG3, ARG4); }
