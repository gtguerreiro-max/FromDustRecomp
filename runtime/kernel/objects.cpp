#include <algorithm>
#include "objects.h"

#include <unordered_map>
#include <vector>

#include "thread.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace kernel
{
    std::mutex g_dispatcherLock;
    std::condition_variable g_dispatcherCv;

    namespace
    {
        std::mutex g_handleLock;
        std::vector<KernelObject*> g_handles; // index = (handle - base) / 4
        constexpr uint32_t kHandleBase = 0xF8000000;

        std::mutex g_guestObjectLock;
        std::unordered_map<uint32_t, KernelObject*> g_guestObjects;

        uint32_t CurrentThreadId()
        {
            GuestThread* thread = CurrentThread();
            return thread ? thread->id : 0;
        }
    }

    Mutant::Mutant(bool initialOwner) : KernelObject(kMutant)
    {
        if (initialOwner)
        {
            owner = CurrentThreadId();
            recursion = 1;
        }
    }

    bool Mutant::IsSignaled() const { return recursion == 0 || owner == CurrentThreadId(); }

    void Mutant::Acquire()
    {
        owner = CurrentThreadId();
        ++recursion;
    }

    uint32_t CreateHandle(KernelObject* object)
    {
        std::lock_guard lock(g_handleLock);
        for (size_t i = 0; i < g_handles.size(); i++)
        {
            if (!g_handles[i])
            {
                g_handles[i] = object;
                return kHandleBase + uint32_t(i) * 4;
            }
        }
        g_handles.push_back(object);
        return kHandleBase + uint32_t(g_handles.size() - 1) * 4;
    }

    KernelObject* LookupHandle(uint32_t handle)
    {
        if (handle == kCurrentThreadHandle)
            return CurrentThread();
        if (handle < kHandleBase)
            return ObjectFromGuest(handle); // some APIs accept object pointers
        std::lock_guard lock(g_handleLock);
        const size_t index = (handle - kHandleBase) / 4;
        return index < g_handles.size() ? g_handles[index] : nullptr;
    }

    bool CloseHandle(uint32_t handle)
    {
        if (handle < kHandleBase)
            return handle == kCurrentThreadHandle || handle == kCurrentProcessHandle;
        KernelObject* object;
        {
            std::lock_guard lock(g_handleLock);
            const size_t index = (handle - kHandleBase) / 4;
            if (index >= g_handles.size() || !g_handles[index])
                return false;
            object = g_handles[index];
            g_handles[index] = nullptr;
        }
        // Threads outlive their handles; everything else is freed when the
        // last handle goes (good enough for a single game).
        if (object->type != kThread && --object->refs == 0)
            delete object;
        return true;
    }

    KernelObject* ObjectFromGuest(uint32_t address)
    {
        if (!address || address >= 0xF0000000)
            return nullptr;
        std::lock_guard lock(g_guestObjectLock);
        auto it = g_guestObjects.find(address);
        if (it != g_guestObjects.end())
            return it->second;

        // Build the host twin from the guest DISPATCHER_HEADER.
        const uint8_t type = *mem::Ptr<uint8_t>(address);
        const int32_t signalState = int32_t(mem::Read32(address + 4));
        KernelObject* object = nullptr;
        switch (type)
        {
        case kNotificationEvent:
        case kSynchronizationEvent:
            object = new Event(type == kNotificationEvent, signalState != 0);
            break;
        case kSemaphore:
            object = new Semaphore(signalState, int32_t(mem::Read32(address + 0x10)));
            break;
        case kMutant:
            object = new Mutant(false);
            break;
        case kNotificationTimer:
        case kSynchronizationTimer:
            object = new Event(type == kNotificationTimer, signalState != 0);
            break;
        default:
            Log("ObjectFromGuest: unsupported object type %u at 0x%08X", type, address);
            return nullptr;
        }
        g_guestObjects[address] = object;
        return object;
    }

    void RegisterGuestObject(uint32_t address, KernelObject* object)
    {
        std::lock_guard lock(g_guestObjectLock);
        g_guestObjects[address] = object;
    }

    bool TimeoutToDeadline(int64_t timeout, Deadline& deadline)
    {
        if (timeout < 0)
        {
            deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(-timeout / 10);
            return true;
        }
        // Absolute (FILETIME based) system time.
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        const int64_t now = (int64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        const int64_t delta = timeout > now ? timeout - now : 0;
        deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(delta / 10);
        return true;
    }

    bool TimeoutToDeadline(uint32_t timeoutPtr, Deadline& deadline)
    {
        if (!timeoutPtr)
            return false;
        return TimeoutToDeadline(int64_t(*mem::Ptr<be<uint64_t>>(timeoutPtr)), deadline);
    }

    uint32_t WaitForObjects(KernelObject* const* objects, uint32_t count, bool waitAll, const Deadline* deadline)
    {
        thread_local Waiter waiter;
        std::unique_lock lock(g_dispatcherLock);
        bool registered = false;
        struct Unregister
        {
            KernelObject* const* objects;
            uint32_t count;
            bool& registered;
            ~Unregister()
            {
                if (!registered)
                    return;
                for (uint32_t i = 0; i < count; i++)
                {
                    auto& list = objects[i]->waiters;
                    list.erase(std::remove(list.begin(), list.end(), &waiter), list.end());
                }
            }
        } unregister{ objects, count, registered };
        for (;;)
        {
            if (waitAll)
            {
                bool all = true;
                for (uint32_t i = 0; i < count && all; i++)
                    all = objects[i]->IsSignaled();
                if (all)
                {
                    for (uint32_t i = 0; i < count; i++)
                        objects[i]->Acquire();
                    return STATUS_SUCCESS;
                }
            }
            else
            {
                for (uint32_t i = 0; i < count; i++)
                {
                    if (objects[i]->IsSignaled())
                    {
                        objects[i]->Acquire();
                        return STATUS_SUCCESS + i;
                    }
                }
            }

            if (!registered)
            {
                for (uint32_t i = 0; i < count; i++)
                    objects[i]->waiters.push_back(&waiter);
                registered = true;
            }
            // Re-check after every wake-up; only give up once the deadline passed.
            if (deadline)
            {
                if (std::chrono::steady_clock::now() >= *deadline)
                    return STATUS_TIMEOUT;
                waiter.cv.wait_until(lock, *deadline);
            }
            else
            {
                waiter.cv.wait(lock);
            }
        }
    }

    void NotifyObject(KernelObject* object)
    {
        if (!object)
            return;
        std::lock_guard lock(g_dispatcherLock);
        for (Waiter* waiter : object->waiters)
            waiter->cv.notify_one();
    }

    void NotifyWaiters() { g_dispatcherCv.notify_all(); }
}
