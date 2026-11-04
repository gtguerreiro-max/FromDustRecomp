#pragma once

#include <atomic>
#include <cstdint>

// Xenos GPU: register file + PM4 command processor.
namespace gpu
{
    // Register indices (dword offsets) used by the command processor.
    namespace reg
    {
        constexpr uint32_t CP_RB_WPTR = 0x01C5;
        constexpr uint32_t SCRATCH_UMSK = 0x01DC;
        constexpr uint32_t SCRATCH_ADDR = 0x01DD;
        constexpr uint32_t SCRATCH_REG0 = 0x0578;
        constexpr uint32_t COHER_STATUS_HOST = 0x0A31;
        constexpr uint32_t RB_SAMPLE_COUNT_ADDR = 0x2325;
        constexpr uint32_t D1MODE_VBLANK_VLINE_STATUS = 0x1951;
        constexpr uint32_t kCount = 0x5003;
    }

    // The CPU sees the GPU registers through MMIO at this guest address; the
    // recompiled stores land in plain guest memory, which the CP polls.
    constexpr uint32_t kMmioBase = 0x7FC80000;

    // Converts a GPU (physical) address to a guest virtual one.
    inline uint32_t PhysToGuest(uint32_t address) { return 0xA0000000u | (address & 0x1FFFFFFFu); }

    struct Stats
    {
        std::atomic<uint64_t> frames{ 0 };
        std::atomic<uint64_t> draws{ 0 };
        std::atomic<uint64_t> packets{ 0 };
    };
    extern Stats g_stats;

    void InitializeRingBuffer(uint32_t physAddress, uint32_t sizeLog2);
    void EnableReadPointerWriteBack(uint32_t physAddress, uint32_t blockLog2);
    void SetInterruptCallback(uint32_t callback, uint32_t userData);

    // Starts the command processor and the 60 Hz vblank thread.
    void Start();

    uint32_t ReadRegister(uint32_t index);
    void WriteRegister(uint32_t index, uint32_t value);
}
