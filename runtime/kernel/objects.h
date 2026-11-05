#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

#include "kernel.h"

namespace kernel
{
    // Every waitable object is guarded by one dispatcher lock, like the NT
    // kernel; waiters sleep on a shared condition variable and re-check.
    extern std::mutex g_dispatcherLock;
    extern std::condition_variable g_dispatcherCv;

    // Guest DISPATCHER_HEADER types.
    enum ObjectType : uint8_t
    {
        kNotificationEvent = 0,
        kSynchronizationEvent = 1,
        kMutant = 2,
        kSemaphore = 5,
        kThread = 6,
        kNotificationTimer = 8,
        kSynchronizationTimer = 9,
        kFile = 0x80, // host-only types from here on
        kOther,
    };

    // A thread blocked in WaitForObjects; registered on every object it waits on.
    struct Waiter
    {
        std::condition_variable cv;
    };

    struct KernelObject
    {
        ObjectType type;
        std::atomic<int> refs{ 1 };
        std::vector<Waiter*> waiters; // dispatcher lock

        explicit KernelObject(ObjectType t) : type(t) {}
        virtual ~KernelObject() = default;

        // Called with the dispatcher lock held.
        virtual bool IsSignaled() const { return false; }
        virtual void Acquire() {} // consume the signal after a successful wait
    };

    struct Event : KernelObject
    {
        bool signaled;
        Event(bool manualReset, bool initial) : KernelObject(manualReset ? kNotificationEvent : kSynchronizationEvent), signaled(initial) {}
        bool IsSignaled() const override { return signaled; }
        void Acquire() override { if (type == kSynchronizationEvent) signaled = false; }
    };

    struct Semaphore : KernelObject
    {
        int32_t count, limit;
        Semaphore(int32_t initial, int32_t max) : KernelObject(kSemaphore), count(initial), limit(max) {}
        bool IsSignaled() const override { return count > 0; }
        void Acquire() override { --count; }
    };

    struct Mutant : KernelObject
    {
        uint32_t owner = 0; // guest thread id
        uint32_t recursion = 0;
        explicit Mutant(bool initialOwner);
        bool IsSignaled() const override;
        void Acquire() override;
    };

    // Handles ------------------------------------------------------------
    constexpr uint32_t kCurrentProcessHandle = 0xFFFFFFFF;
    constexpr uint32_t kCurrentThreadHandle = 0xFFFFFFFE;

    uint32_t CreateHandle(KernelObject* object); // takes a reference
    KernelObject* LookupHandle(uint32_t handle);  // borrowed pointer
    bool CloseHandle(uint32_t handle);

    template<typename T>
    T* LookupHandle(uint32_t handle, ObjectType type)
    {
        KernelObject* object = LookupHandle(handle);
        return object && object->type == type ? static_cast<T*>(object) : nullptr;
    }

    // Objects that live in guest memory (KEVENT etc. passed by pointer) get a
    // host twin created on first use from their dispatcher header.
    KernelObject* ObjectFromGuest(uint32_t address);
    void RegisterGuestObject(uint32_t address, KernelObject* object);

    // Waits ---------------------------------------------------------------
    using Deadline = std::chrono::steady_clock::time_point;

    // Converts an NT timeout (100 ns units, negative = relative; pointer may be
    // null for infinite) into a deadline. Returns false for infinite.
    bool TimeoutToDeadline(uint32_t timeoutPtr, Deadline& deadline);
    bool TimeoutToDeadline(int64_t timeout100ns, Deadline& deadline);

    // Time the calling thread spent blocked (ns), per kind: 0 kernel object
    // waits, 1 KeDelayExecutionThread / yields, 2 critical sections, 3 the
    // native renderer's hand-over. Diagnostics (FD_TRACE_SIM_STEPS).
    uint64_t* ThreadWaits();
    struct WaitTimer
    {
        explicit WaitTimer(int kind) : kind(kind), start(std::chrono::steady_clock::now()) {}
        ~WaitTimer() { ThreadWaits()[kind] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()); }
        int kind;
        std::chrono::steady_clock::time_point start;
    };

    // Returns STATUS_SUCCESS + index, or STATUS_TIMEOUT.
    uint32_t WaitForObjects(KernelObject* const* objects, uint32_t count, bool waitAll, const Deadline* deadline);

    // After changing an object's state (dispatcher lock released), wake only
    // the threads waiting on that object.
    void NotifyObject(KernelObject* object);
    // Wakes threads blocked on the global condition (suspended thread starts).
    void NotifyWaiters();
}
