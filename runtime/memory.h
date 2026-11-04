#pragma once

#include <cstddef>
#include <cstdint>

#include "be.h"

// The 4 GB guest address space, mapped as one block of host memory.
namespace mem
{
    extern uint8_t* g_base;

    // Guest regions (see memory.cpp for the layout).
    constexpr uint32_t kVirtualHeapBase = 0x40000000;
    constexpr uint32_t kVirtualHeapEnd  = 0x7F000000;
    constexpr uint32_t kPhysicalBase    = 0xA0000000; // 512 MB, GPU visible
    constexpr uint32_t kPhysicalEnd     = 0xC0000000;
    constexpr uint32_t kPhysicalSize    = 0x20000000;
    // The 4 KB-page alias starts one physical page after A/C.
    constexpr uint32_t PhysicalAddress(uint32_t address) {
        return (address&(kPhysicalSize-1))+(address>=0xE0000000u?0x1000:0);
    }

    bool Init();
    // Apply write watches to every CPU alias of the physical range.
    bool ProtectPhysical(uint32_t offset,uint32_t length,bool readOnly);

    // Allocations return guest addresses; 0 on failure.
    uint32_t AllocVirtual(uint32_t size, uint32_t alignment = 0x10000);
    uint32_t AllocPhysical(uint32_t size, uint32_t alignment = 0x1000);
    void Free(uint32_t address);
    uint32_t AllocationSize(uint32_t address);

    // Small allocations for kernel-owned guest structures (PCRs, stacks...).
    uint32_t AllocSystem(uint32_t size, uint32_t alignment = 16);

    inline void* Translate(uint32_t guest) { return guest ? g_base + guest : nullptr; }
    template<typename T> inline T* Ptr(uint32_t guest) { return reinterpret_cast<T*>(Translate(guest)); }
    inline uint32_t ToGuest(const void* host) { return host ? uint32_t(static_cast<const uint8_t*>(host) - g_base) : 0; }

    inline uint32_t Read32(uint32_t guest) { return *Ptr<be<uint32_t>>(guest); }
    inline void Write32(uint32_t guest, uint32_t value) { *Ptr<be<uint32_t>>(guest) = value; }
}
