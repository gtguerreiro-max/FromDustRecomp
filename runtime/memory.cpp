#include "memory.h"

#include <cstdio>
#include <array>
#include <map>
#include <vector>
#include <mutex>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Guest address space layout:
//   0x40000000 - 0x7F000000  virtual heap (NtAllocateVirtualMemory, XamAlloc)
//   0x7F000000 - 0x80000000  system heap (kernel structures, thread stacks)
//   0x82000000 - 0x8A300000  XEX images (Default.xex + DLLs)
//   0xA0000000 - 0xC0000000  physical memory (MmAllocatePhysicalMemoryEx)
//   0xC0000000 - 0xE0000000  alternate CPU view of the same physical memory
//   0xE0000000 - 0x100000000 alternate CPU view of the same physical memory
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

            std::vector<std::pair<uint32_t, uint32_t>> Used()
            {
                std::lock_guard lock(mutex_);
                return {used_.begin(), used_.end()};
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
        // PPC code uses direct base+address accesses. Reserve a contiguous
        // address interval, then map A/C/E onto one page-file section: XDK
        // query readbacks deliberately access a different alias from GPU writes.
        auto* candidate=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
        if(!candidate)
        {
            fprintf(stderr, "Failed to reserve 4 GB of guest memory (error %lu)\n", GetLastError());
            return false;
        }
        if(!VirtualFree(candidate,0,MEM_RELEASE))return false;
        void* ram=VirtualAlloc(candidate,kPhysicalBase,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        HANDLE section=ram?CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,kPhysicalSize,nullptr):nullptr;
        std::array<void*,3> views{};
        bool mapped=ram==candidate&&section;
        for(size_t alias=0;mapped&&alias<2;++alias)
        {
            void* wanted=candidate+uint64_t(kPhysicalBase)+alias*uint64_t(kPhysicalSize);
            views[alias]=MapViewOfFileEx(section,FILE_MAP_ALL_ACCESS,0,0,kPhysicalSize,wanted);
            mapped=views[alias]==wanted;
        }
        // Map the E alias at its real 4 KB file offset. Replacing a placeholder
        // permits page alignment without changing direct PPC memory accesses.
        using Allocate2=PVOID(WINAPI*)(HANDLE,PVOID,SIZE_T,ULONG,ULONG,void*,ULONG);
        using Map3=PVOID(WINAPI*)(HANDLE,HANDLE,PVOID,ULONG64,SIZE_T,ULONG,ULONG,void*,ULONG);
        auto kernel=GetModuleHandleW(L"KernelBase.dll");
        auto allocate2=reinterpret_cast<Allocate2>(GetProcAddress(kernel,"VirtualAlloc2"));
        auto map3=reinterpret_cast<Map3>(GetProcAddress(kernel,"MapViewOfFile3"));
        void* ePlaceholder=nullptr;bool eSplit=false;
        if(mapped&&allocate2&&map3)
        {
            void* wanted=candidate+0xE0000000ull;
            ePlaceholder=allocate2(GetCurrentProcess(),wanted,kPhysicalSize,MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,nullptr,0);
            mapped=ePlaceholder==wanted;
            if(mapped)
            {
                eSplit=VirtualFree(wanted,kPhysicalSize-0x1000,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)!=0;
                mapped=eSplit;
                if(mapped){views[2]=map3(section,GetCurrentProcess(),wanted,0x1000,kPhysicalSize-0x1000,MEM_REPLACE_PLACEHOLDER,PAGE_READWRITE,nullptr,0);mapped=views[2]==wanted;}
            }
        }
        else if(mapped){mapped=false;SetLastError(ERROR_PROC_NOT_FOUND);}
        if(!mapped)
        {
            const DWORD error=GetLastError();
            for(void* view:views)if(view)UnmapViewOfFile(view);
            if(ePlaceholder&&!views[2])VirtualFree(ePlaceholder,0,MEM_RELEASE);
            if(eSplit)VirtualFree(candidate+0xFFFFF000ull,0,MEM_RELEASE);
            if(section)CloseHandle(section);
            if(ram)VirtualFree(ram,0,MEM_RELEASE);
            fprintf(stderr,"Failed to map shared physical guest aliases (error %lu)\n",error);
            return false;
        }
        CloseHandle(section); // The mapped views retain the section.
        g_base=candidate;

        g_virtual = new RangeAllocator(kVirtualHeapBase, kVirtualHeapEnd);
        g_physical = new RangeAllocator(kPhysicalBase, kPhysicalEnd);
        g_system = new RangeAllocator(kSystemHeapBase, kSystemHeapEnd);
        return true;
    }

    bool ProtectPhysical(uint32_t offset,uint32_t length,bool readOnly)
    {
        if(!g_base||!length||uint64_t(offset)+length>kPhysicalSize)return false;
        const uint32_t first=offset&~0xFFFu;
        const uint32_t size=uint32_t((uint64_t(offset)+length+0xFFF)&~0xFFFull)-first;
        for(uint64_t alias=kPhysicalBase;alias<0xE0000000ull;alias+=kPhysicalSize)
        {
            DWORD old;
            if(!VirtualProtect(g_base+alias+first,size,readOnly?PAGE_READONLY:PAGE_READWRITE,&old))return false;
        }
        const uint32_t end=first+size;
        if(end>0x1000)
        {
            const uint32_t eFirst=first<0x1000?0x1000:first;DWORD old;
            if(!VirtualProtect(g_base+0xE0000000ull+eFirst-0x1000,end-eFirst,readOnly?PAGE_READONLY:PAGE_READWRITE,&old))return false;
        }
        return true;
    }

    uint32_t AllocVirtual(uint32_t size, uint32_t alignment) { return g_virtual->Alloc(size, alignment < 16 ? 16 : alignment); }
    std::vector<std::pair<uint32_t, uint32_t>> AllocatedRanges()
    {
        auto ranges = g_virtual->Used();
        for (auto& range : g_physical->Used()) ranges.push_back(range);
        for (auto& range : g_system->Used()) ranges.push_back(range);
        return ranges;
    }
    uint32_t AllocPhysical(uint32_t size, uint32_t alignment) { return g_physical->Alloc(size, alignment < 16 ? 16 : alignment); }
    uint32_t AllocSystem(uint32_t size, uint32_t alignment) { return g_system->Alloc(size, alignment < 16 ? 16 : alignment); }

    void Free(uint32_t address)
    {
        if(address>=kPhysicalBase)address=kPhysicalBase|PhysicalAddress(address);
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
        if(address>=kPhysicalBase)address=kPhysicalBase|PhysicalAddress(address);
        for (auto* heap : { g_virtual, g_physical, g_system })
        {
            if (uint32_t size = heap->Size(address))
                return size;
        }
        return 0;
    }
}
