// Virtual / physical memory and pool allocations.

#include "kernel.h"

using namespace kernel;

namespace
{
    constexpr uint32_t MEM_COMMIT = 0x1000;
    constexpr uint32_t MEM_RESERVE = 0x2000;
    constexpr uint32_t MEM_RELEASE = 0x8000;
    constexpr uint32_t MEM_FREE = 0x10000;
    constexpr uint32_t MEM_PRIVATE = 0x20000;
    constexpr uint32_t PAGE_READWRITE = 0x04;
}

KFUNC(NtAllocateVirtualMemory)
{
    // (base_ptr, size_ptr, allocation_type, protect, debug_memory)
    const uint32_t basePtr = ARG0, sizePtr = ARG1;
    uint32_t address = mem::Read32(basePtr);
    uint32_t size = mem::Read32(sizePtr);
    if (address)
    {
        // Committing (part of) an earlier reservation: everything is already
        // backed, so just round the request to pages.
        mem::Write32(sizePtr, (size + 0xFFF) & ~0xFFFu);
        RETURN(STATUS_SUCCESS);
    }
    size = (size + 0xFFFF) & ~0xFFFFu;
    address = mem::AllocVirtual(size, 0x10000);
    if (!address)
    {
        Log("NtAllocateVirtualMemory: out of memory (0x%X bytes)", size);
        RETURN(STATUS_NO_MEMORY);
    }
    mem::Write32(basePtr, address);
    mem::Write32(sizePtr, size);
    RETURN(STATUS_SUCCESS);
}

KFUNC(NtFreeVirtualMemory)
{
    // (base_ptr, size_ptr, free_type, debug_memory)
    const uint32_t address = mem::Read32(ARG0);
    if (ARG2 & MEM_RELEASE)
        mem::Free(address);
    RETURN(STATUS_SUCCESS);
}

KFUNC(NtQueryVirtualMemory)
{
    // (base, MEMORY_BASIC_INFORMATION*, debug)
    const uint32_t address = ARG0 & ~0xFFFu, info = ARG1;
    const uint32_t size = mem::AllocationSize(ARG0);
    mem::Write32(info + 0x00, address);
    mem::Write32(info + 0x04, address);
    mem::Write32(info + 0x08, PAGE_READWRITE);
    mem::Write32(info + 0x0C, size ? size : 0x1000);
    mem::Write32(info + 0x10, size ? MEM_COMMIT : MEM_FREE);
    mem::Write32(info + 0x14, PAGE_READWRITE);
    mem::Write32(info + 0x18, MEM_PRIVATE);
    RETURN(STATUS_SUCCESS);
}

KFUNC(MmAllocatePhysicalMemoryEx)
{
    // (flags, size, protect, min_address, max_address, alignment)
    const uint32_t size = (ARG1 + 0xFFF) & ~0xFFFu;
    uint32_t alignment = ARG5 ? ARG5 : 0x1000;
    if (alignment < 0x1000)
        alignment = 0x1000;
    const uint32_t address = mem::AllocPhysical(size, alignment);
    if (!address)
        Log("MmAllocatePhysicalMemoryEx: out of memory (0x%X bytes)", size);
    RETURN(address);
}

KFUNC(MmFreePhysicalMemory) { mem::Free(ARG1); RETURN(0); }
KFUNC(MmGetPhysicalAddress) { RETURN(ARG0 & 0x1FFFFFFF); }
KFUNC(MmQueryAddressProtect) { RETURN(PAGE_READWRITE); }
KFUNC(MmSetAddressProtect) { RETURN(0); }
KFUNC(MmQueryAllocationSize) { RETURN(mem::AllocationSize(ARG0)); }

KFUNC(MmQueryStatistics)
{
    // MM_STATISTICS: Length @0, TotalPhysicalPages @4, KernelPages @8, ...
    const uint32_t stats = ARG0;
    const uint32_t length = mem::Read32(stats);
    memset(mem::Translate(stats + 4), 0, length > 4 ? length - 4 : 0);
    mem::Write32(stats + 4, 0x20000); // 512 MB in 4 KB pages
    mem::Write32(stats + 8, 0x1000);
    mem::Write32(stats + 0x0C, 0x10000); // available pages
    RETURN(STATUS_SUCCESS);
}

KFUNC(ExAllocatePoolTypeWithTag)
{
    // (size, tag, pool_type)
    RETURN(mem::AllocVirtual(ARG0 ? ARG0 : 16, 16));
}

KFUNC(ExFreePool) { mem::Free(ARG0); RETURN(0); }

KFUNC(XamAlloc)
{
    // (flags, size, out_ptr)
    const uint32_t address = mem::AllocVirtual(ARG1 ? ARG1 : 16, 16);
    mem::Write32(ARG2, address);
    RETURN(address ? ERROR_SUCCESS : 8 /* ERROR_NOT_ENOUGH_MEMORY */);
}

KFUNC(XamFree) { mem::Free(ARG0); RETURN(ERROR_SUCCESS); }
