#include "memory.h"

#include <cstdio>
#include <map>
#include <mutex>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Guest address space layout:
//   0x40000000 - 0x7F000000  virtual heap (NtAllocateVirtualMemory, XamAlloc)
//   0x7F000000 - 0x80000000  system heap (kernel structures, thread stacks)
//   0x82000000 - 0x8A300000  XEX images (Default.xex + DLLs)
//   0xA0000000 - 0xC0000000  physical memory (MmAllocatePhysicalMemoryEx)
//   0x7FC80000               GPU registers (inside the system heap range,
//                            but the heap stops short of them)
namespace mem
{
    uint8_t* g_base = nullptr;

    namespace
    {
        constexpr uint32_t kSystemHeapBase = 0x7F000000;
        constexpr uint32_t kSystemHeapEnd  = 0x7FC00000;

        // First-fit allocator over a guest range. Plenty for a game that
        // allocates its big pools once at startup.
        class RangeAllocator
        {
        public:
            RangeAllocator(uint32_t begin, uint32_t end) { free_[begin] = end - begin; }

            uint32_t Alloc(uint32_t size, uint32_t alignment)
            {
                std::lock_guard lock(mutex_);
                size = (size + 0xF) & ~0xFu;
                for (auto it = free_.begin(); it != free_.end(); ++it)
                {
                    const uint32_t start = it->first;
                    const uint32_t len = it->second;
                    const uint32_t aligned = (start + alignment - 1) & ~(alignment - 1);
                    if (aligned + uint64_t(size) > uint64_t(start) + len)
                        continue;

                    free_.erase(it);
                    if (aligned > start)
                        free_[start] = aligned - start;
                    if (aligned + size < start + len)
                        free_[aligned + size] = start + len - (aligned + size);
                    used_[aligned] = size;
                    return aligned;
                }
                return 0;
            }

            bool Owns(uint32_t address)
            {
                std::lock_guard lock(mutex_);
                return used_.count(address) != 0;
            }

            uint32_t Size(uint32_t address)
            {
                std::lock_guard lock(mutex_);
                auto it = used_.find(address);
                return it == used_.end() ? 0 : it->second;
            }

            void Free(uint32_t address)
            {
                std::lock_guard lock(mutex_);
                auto it = used_.find(address);
                if (it == used_.end())
                    return;
                uint32_t start = address, len = it->second;
                used_.erase(it);
                memset(g_base + start, 0, len);

                // Coalesce with neighbours.
                auto next = free_.lower_bound(start);
                if (next != free_.end() && next->first == start + len)
                {
                    len += next->second;
                    next = free_.erase(next);
                }
                if (next != free_.begin())
                {
                    auto prev = std::prev(next);
                    if (prev->first + prev->second == start)
                    {
                        prev->second += len;
                        return;
                    }
                }
                free_[start] = len;
            }

        private:
            std::mutex mutex_;
            std::map<uint32_t, uint32_t> free_;
            std::map<uint32_t, uint32_t> used_;
        };

        RangeAllocator* g_virtual;
        RangeAllocator* g_physical;
        RangeAllocator* g_system;
    }

    bool Init()
    {
        // Committing the whole range up front is cheap: pages only get backed
        // by RAM when first touched, and it keeps guest accesses fault-free.
        g_base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!g_base)
        {
            fprintf(stderr, "Failed to reserve 4 GB of guest memory (error %lu)\n", GetLastError());
            return false;
        }

        g_virtual = new RangeAllocator(kVirtualHeapBase, kVirtualHeapEnd);
        g_physical = new RangeAllocator(kPhysicalBase, kPhysicalEnd);
        g_system = new RangeAllocator(kSystemHeapBase, kSystemHeapEnd);
        return true;
    }

    uint32_t AllocVirtual(uint32_t size, uint32_t alignment) { return g_virtual->Alloc(size, alignment < 16 ? 16 : alignment); }
    uint32_t AllocPhysical(uint32_t size, uint32_t alignment) { return g_physical->Alloc(size, alignment < 16 ? 16 : alignment); }
    uint32_t AllocSystem(uint32_t size, uint32_t alignment) { return g_system->Alloc(size, alignment < 16 ? 16 : alignment); }

    void Free(uint32_t address)
    {
        for (auto* heap : { g_virtual, g_physical, g_system })
        {
            if (heap->Owns(address))
            {
                heap->Free(address);
                return;
            }
        }
    }

    uint32_t AllocationSize(uint32_t address)
    {
        for (auto* heap : { g_virtual, g_physical, g_system })
        {
            if (uint32_t size = heap->Size(address))
                return size;
        }
        return 0;
    }
}
