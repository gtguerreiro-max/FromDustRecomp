// PM4 command processor: consumes the ring buffer the game's D3D writes,
// updates the register file, services fences/waits/interrupts and swaps.
// Native rendering executes the collected engine operations at VdSwap; this
// service handles the guest-visible ring, query reports and completion events.

#include <chrono>
#include <bit>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "gpu.h"
#include "xenia_bridge.h"
#include "occlusion_report.h"
#include "native/live_audit.h"

#include "../kernel/kernel.h"
#include "../kernel/thread.h"
#include "../memory.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace gpu
{
    Stats g_stats;

    namespace
    {
        // PM4 type-3 opcodes.
        enum Opcode : uint32_t
        {
            PM4_NOP = 0x10,
            PM4_REG_RMW = 0x21,
            PM4_DRAW_INDX = 0x22,
            PM4_VIZ_QUERY = 0x23,
            PM4_WAIT_FOR_IDLE = 0x26,
            PM4_IM_LOAD = 0x27,
            PM4_IM_LOAD_IMMEDIATE = 0x2B,
            PM4_SET_CONSTANT = 0x2D,
            PM4_LOAD_ALU_CONSTANT = 0x2F,
            PM4_DRAW_INDX_2 = 0x36,
            PM4_INDIRECT_BUFFER_PFD = 0x37,
            PM4_INVALIDATE_STATE = 0x3B,
            PM4_WAIT_REG_MEM = 0x3C,
            PM4_MEM_WRITE = 0x3D,
            PM4_INDIRECT_BUFFER = 0x3F,
            PM4_COND_WRITE = 0x45,
            PM4_EVENT_WRITE = 0x46,
            PM4_ME_INIT = 0x48,
            PM4_SET_BIN_MASK = 0x50,
            PM4_SET_BIN_SELECT = 0x51,
            PM4_INTERRUPT = 0x54,
            PM4_SET_CONSTANT2 = 0x55,
            PM4_SET_SHADER_CONSTANTS = 0x56,
            PM4_EVENT_WRITE_SHD = 0x58,
            PM4_EVENT_WRITE_EXT = 0x59,
            PM4_EVENT_WRITE_ZPD = 0x5B,
            PM4_CONTEXT_UPDATE = 0x5E,
            PM4_SET_BIN_MASK_LO = 0x60,
            PM4_SET_BIN_MASK_HI = 0x61,
            PM4_SET_BIN_SELECT_LO = 0x62,
            PM4_SET_BIN_SELECT_HI = 0x63,
            PM4_XE_SWAP = 0x64, // written by our VdSwap
        };

        uint32_t g_regs[reg::kCount];
        std::mutex g_regLock;
        VisibleSampleCounter g_visibleSamples;
        std::atomic<uint32_t> g_vblankCounter{0};
        int g_traceVS=-1,g_tracePS=-1;
        uint32_t g_traceDraws=0,g_traceMesh=0,g_traceTerrain=0,g_traceDepth=0;

        uint32_t g_ringBase = 0;     // guest address
        uint32_t g_ringDwords = 0;
        uint32_t g_readPtr = 0;      // in dwords
        uint32_t g_writeBack = 0;    // guest address of the read pointer copy
        uint32_t g_interruptCallback = 0, g_interruptUserData = 0;
        std::mutex g_interruptLock;

        uint32_t ReadDword(uint32_t guest) { return mem::Read32(guest); }

        // Endian-swap a dword for a GPU memory write (low 2 address bits).
        uint32_t GpuSwap(uint32_t value, uint32_t endian)
        {
            switch (endian)
            {
            case 1: return ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF); // 8-in-16
            case 2: return __builtin_bswap32(value);                                   // 8-in-32
            case 3: return (value << 16) | (value >> 16);                              // 16-in-32
            }
            return value;
        }

        // GPU writes are raw stores of the swapped value (CPU then reads BE).
        void GpuWrite(uint32_t address, uint32_t value)
        {
            *mem::Ptr<uint32_t>(PhysToGuest(address & ~3u)) = GpuSwap(value, address & 3);
        }

        // Runs the D3D interrupt handler as if on hardware thread `cpu`: the
        // handler acknowledges per-CPU bits using the PCR's current CPU.
        void DispatchInterrupt(uint32_t source, uint32_t cpu)
        {
            std::lock_guard lock(g_interruptLock);
            if (!g_interruptCallback)
                return;
            const uint32_t pcr = kernel::CurrentThread()->pcrAddress;
            mem::Ptr<uint8_t>(pcr)[kernel::pcr::kCurrentCpu] = uint8_t(cpu);
            kernel::CallGuestFunction(g_interruptCallback, source, g_interruptUserData);
        }

        bool CompareFunction(uint32_t function, uint32_t value, uint32_t reference)
        {
            switch (function & 7)
            {
            case 0: return false;
            case 1: return value < reference;
            case 2: return value <= reference;
            case 3: return value == reference;
            case 4: return value != reference;
            case 5: return value >= reference;
            case 6: return value > reference;
            default: return true;
            }
        }

        void ExecuteBuffer(uint32_t guest, uint32_t dwords);

        // Executes one type-3 packet whose payload starts at `data`.
        void ExecutePacket3(uint32_t opcode, uint32_t data, uint32_t count)
        {
            auto arg = [&](uint32_t i) { return ReadDword(data + i * 4); };
            switch (opcode)
            {
            case PM4_INDIRECT_BUFFER:
            case PM4_INDIRECT_BUFFER_PFD:
                ExecuteBuffer(PhysToGuest(arg(0)), arg(1) & 0xFFFFF);
                break;

            case PM4_WAIT_REG_MEM:
            {
                // wait_info, poll address/register, reference, mask, wait interval
                const uint32_t info = arg(0), poll = arg(1), ref = arg(2), mask = arg(3);
                for (int spins = 0;; spins++)
                {
                    uint32_t value;
                    if (info & 0x10)
                        value = GpuSwap(*mem::Ptr<uint32_t>(PhysToGuest(poll & ~3u)), poll & 3);
                    else
                        value = ReadRegister(poll);
                    if (CompareFunction(info, value & mask, ref))
                        break;
                    if (spins == 300)
                        kernel::Log("gpu: WAIT_REG_MEM stuck: info=%08X %s=%08X value=%08X mask=%08X ref=%08X", info,
                            (info & 0x10) ? "mem" : "reg", poll, value, mask, ref);
                    if (spins > 64)
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    else
                        std::this_thread::yield();
                }
                break;
            }

            case PM4_REG_RMW:
            {
                const uint32_t info = arg(0), andMask = arg(1), orMask = arg(2);
                const uint32_t index = info & 0x1FFF;
                uint32_t value = ReadRegister(index);
                value &= (info & (1u << 31)) ? ReadRegister(andMask & 0x1FFF) : andMask;
                value |= (info & (1u << 30)) ? ReadRegister(orMask & 0x1FFF) : orMask;
                WriteRegister(index, value);
                break;
            }

            case PM4_COND_WRITE:
            {
                // wait_info, poll address/reg, reference, mask, write address/reg, value
                const uint32_t info = arg(0), poll = arg(1), ref = arg(2), mask = arg(3), target = arg(4), value = arg(5);
                uint32_t current = (info & 0x10) ? GpuSwap(*mem::Ptr<uint32_t>(PhysToGuest(poll & ~3u)), poll & 3) : ReadRegister(poll);
                if (CompareFunction(info, current & mask, ref))
                {
                    if (info & 0x100)
                        GpuWrite(target, value);
                    else
                        WriteRegister(target, value);
                }
                break;
            }

            case PM4_EVENT_WRITE_SHD:
            {
                // initiator, address, value (bit 31 of initiator: write a counter instead)
                const uint32_t initiator = arg(0), address = arg(1);
                const uint32_t value = (initiator >> 31) ? g_vblankCounter.load(std::memory_order_relaxed) : arg(2);
                GpuWrite(address, value);
                break;
            }

            case PM4_EVENT_WRITE_EXT:
            {
                // Screen extents for occlusion queries: report "everything visible".
                const uint32_t address = arg(1);
                // Coordinates are in eight-pixel units (8192 / 8), matching
                // Xenos' maximum 2D extent rather than an unscaled bit mask.
                const uint16_t extents[] = { 0, 1024, 0, 1024, 0, 1 };
                if (getenv("FD_GPU_EVENT_TRACE"))
                {
                    static unsigned traces = 0;
                    if (traces++ < 8) kernel::Log("native EXT report: address %08X", address);
                }
                for (int i = 0; i < 6; i++)
                    *mem::Ptr<be<uint16_t>>(PhysToGuest(address & ~3u) + i * 2) = extents[i];
                break;
            }

            case PM4_EVENT_WRITE_ZPD:
            {
                // Eight little-endian lanes, not a big-endian uint64. D3D
                // waits on ZPass / StencilFail completion words and computes
                // interval counts by subtracting successive snapshots.
                const uint32_t address = ReadRegister(reg::RB_SAMPLE_COUNT_ADDR);
                const auto report=address?g_visibleSamples.Complete():g_visibleSamples.Report();
                if (getenv("FD_GPU_EVENT_TRACE"))
                {
                    static unsigned traces = 0;
                    if (traces++ < 8) kernel::Log("native ZPD report: address %08X, pass %u", address,
                        report[4] + report[5]);
                }
                if (address)
                {
                    static_assert(std::endian::native == std::endian::little);
                    auto* destination = mem::Ptr<uint8_t>(PhysToGuest(address & ~3u));
                    memcpy(destination, report.data(), 16);
                    std::atomic_thread_fence(std::memory_order_release);
                    memcpy(destination + 16, report.data() + 4, 16);
                }
                break;
            }

            case PM4_MEM_WRITE:
            {
                const uint32_t address = arg(0);
                for (uint32_t i = 1; i < count; i++)
                    GpuWrite(address + (i - 1) * 4, arg(i));
                break;
            }

            case PM4_SET_CONSTANT:
            {
                // offset_type: index in low 11 bits, type in bits 16..23
                const uint32_t offsetType = arg(0);
                static const uint32_t kBases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
                const uint32_t type = (offsetType >> 16) & 0xFF;
                if (type < 5)
                {
                    const uint32_t base = kBases[type] + (offsetType & 0x7FF);
                    for (uint32_t i = 1; i < count; i++)
                        WriteRegister(base + i - 1, arg(i));
                }
                break;
            }

            case PM4_SET_CONSTANT2:
            case PM4_SET_SHADER_CONSTANTS:
            {
                const uint32_t base = arg(0) & 0xFFFF;
                for (uint32_t i = 1; i < count; i++)
                    WriteRegister(base + i - 1, arg(i));
                break;
            }

            case PM4_LOAD_ALU_CONSTANT:
            {
                // address, offset_type, size (dwords)
                const uint32_t address = PhysToGuest(arg(0) & 0x3FFFFFFF), offsetType = arg(1), size = arg(2);
                static const uint32_t kBases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
                const uint32_t type = (offsetType >> 16) & 0xFF;
                if (type < 5)
                {
                    const uint32_t base = kBases[type] + (offsetType & 0x7FF);
                    for (uint32_t i = 0; i < size; i++)
                        WriteRegister(base + i, ReadDword(address + i * 4));
                }
                break;
            }

            case PM4_DRAW_INDX:
            case PM4_DRAW_INDX_2:
                ++g_stats.draws;
                if(getenv("FD_TRACE_FRAME_COUNTS"))
                {
                    ++g_traceDraws;g_traceMesh+=g_tracePS==86;g_traceTerrain+=g_tracePS==146;g_traceDepth+=g_traceVS==67;
                }
                break;

            case PM4_INTERRUPT:
            {
                // One interrupt per CPU in the mask.
                const uint32_t cpuMask = arg(0);
                for (uint32_t cpu = 0; cpu < 6; cpu++)
                {
                    if (cpuMask & (1u << cpu))
                        DispatchInterrupt(1, cpu);
                }
                break;
            }

            case PM4_XE_SWAP:
                if(getenv("FD_TRACE_FRAME_COUNTS"))
                {
                    if(g_traceDraws>500&&g_stats.frames.load()%60==0)
                        kernel::Log("native CP frame %llu: %u draws, mesh %u terrain %u mesh-depth %u",g_stats.frames.load()+1,g_traceDraws,g_traceMesh,g_traceTerrain,g_traceDepth);
                    g_traceDraws=g_traceMesh=g_traceTerrain=g_traceDepth=0;
                }
                ++g_stats.frames;
                break;

            case PM4_IM_LOAD:
                if(getenv("FD_TRACE_FRAME_COUNTS")&&count>=2)
                {
                    const uint32_t address=arg(0),stage=address&3;
                    const int shader=native::audit::TraceShaderCode(stage,address&~3u);
                    if(stage==0)g_traceVS=shader;else if(stage==1)g_tracePS=shader;
                }
                break;
            case PM4_IM_LOAD_IMMEDIATE:
            case PM4_ME_INIT:
            case PM4_NOP:
            case PM4_INVALIDATE_STATE:
            case PM4_CONTEXT_UPDATE:
            case PM4_WAIT_FOR_IDLE:
            case PM4_VIZ_QUERY:
            case PM4_EVENT_WRITE:
            case PM4_SET_BIN_MASK:
            case PM4_SET_BIN_SELECT:
            case PM4_SET_BIN_MASK_LO:
            case PM4_SET_BIN_MASK_HI:
            case PM4_SET_BIN_SELECT_LO:
            case PM4_SET_BIN_SELECT_HI:
                break;

            default:
            {
                static std::atomic<int> unknown{ 0 };
                if (unknown++ < 32)
                    kernel::Log("gpu: unhandled PM4 opcode 0x%02X (%u dwords)", opcode, count);
                break;
            }
            }
        }

        // Executes a linear command buffer of `dwords` dwords.
        void ExecuteBuffer(uint32_t guest, uint32_t dwords)
        {
            uint32_t i = 0;
            while (i < dwords)
            {
                const uint32_t header = ReadDword(guest + i * 4);
                ++g_stats.packets;
                switch (header >> 30)
                {
                case 0:
                {
                    // Register writes: base index, count, "write one register" flag.
                    const uint32_t index = header & 0x7FFF;
                    const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
                    const bool one = (header >> 15) & 1;
                    for (uint32_t n = 0; n < count; n++)
                        WriteRegister(one ? index : index + n, ReadDword(guest + (i + 1 + n) * 4));
                    i += 1 + count;
                    break;
                }
                case 1:
                    WriteRegister(header & 0x7FF, ReadDword(guest + (i + 1) * 4));
                    WriteRegister((header >> 11) & 0x7FF, ReadDword(guest + (i + 2) * 4));
                    i += 3;
                    break;
                case 2:
                    i += 1;
                    break;
                case 3:
                {
                    const uint32_t opcode = (header >> 8) & 0x7F;
                    const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
                    ExecutePacket3(opcode, guest + (i + 1) * 4, count);
                    i += 1 + count;
                    break;
                }
                }
            }
        }

        // Ring buffer consumer. Packets may wrap around the end of the ring,
        // so a ring-relative reader copies them out first.
        void CommandProcessorThread()
        {
            kernel::AdoptHostThread(0x40000);
            std::vector<uint32_t> scratch;
            for (;;)
            {
                const uint32_t writePtr = mem::Read32(kMmioBase + reg::CP_RB_WPTR * 4) % (g_ringDwords ? g_ringDwords : 1);
                if (!g_ringBase || writePtr == g_readPtr)
                {
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                    continue;
                }

                // Linearize [read, write) into host-visible guest scratch memory.
                const uint32_t available = (writePtr + g_ringDwords - g_readPtr) % g_ringDwords;
                static uint32_t linear = mem::AllocPhysical(g_ringDwords * 4, 0x1000);
                for (uint32_t n = 0; n < available; n++)
                    mem::Write32(linear + n * 4, mem::Read32(g_ringBase + ((g_readPtr + n) % g_ringDwords) * 4));
                ExecuteBuffer(linear, available);

                g_readPtr = writePtr;
                if (g_writeBack)
                    mem::Write32(g_writeBack, g_readPtr);
            }
        }

        void VblankThread()
        {
            kernel::AdoptHostThread(0x10000);
            auto next = std::chrono::steady_clock::now();
            uint64_t lastFrames = 0, ticks = 0;
            for (;;)
            {
                next += std::chrono::microseconds(16667);
                std::this_thread::sleep_until(next);
                ++g_vblankCounter;
                // D3D's vblank handler checks the vblank status register first.
                mem::Write32(kMmioBase + reg::D1MODE_VBLANK_VLINE_STATUS * 4, 1);
                DispatchInterrupt(0, 2);
                if (++ticks % 60 == 0)
                {
                    const uint64_t frames = g_stats.frames.load();
                    kernel::Log("gpu: %llu fps, %llu draws total, %llu packets total", frames - lastFrames,
                        g_stats.draws.load(), g_stats.packets.load());
                    lastFrames = frames;
                }
            }
        }
    }

    uint32_t ReadRegister(uint32_t index)
    {
        if (index >= reg::kCount)
            return 0;
        std::lock_guard lock(g_regLock);
        return g_regs[index];
    }

    void WriteRegister(uint32_t index, uint32_t value)
    {
        if (index >= reg::kCount)
            return;
        {
            std::lock_guard lock(g_regLock);
            g_regs[index] = value;
        }
        // D3D fences: scratch register writes are mirrored to memory when
        // their bit is set in SCRATCH_UMSK.
        if (index >= reg::SCRATCH_REG0 && index < reg::SCRATCH_REG0 + 8)
        {
            const uint32_t slot = index - reg::SCRATCH_REG0;
            if (ReadRegister(reg::SCRATCH_UMSK) & (1u << slot))
                mem::Write32(PhysToGuest(ReadRegister(reg::SCRATCH_ADDR) + slot * 4), value);
        }
        else if (index == reg::COHER_STATUS_HOST)
        {
            // Cache flush requests complete immediately.
            std::lock_guard lock(g_regLock);
            g_regs[index] &= ~0x80000000u;
        }
    }

    void InitializeRingBuffer(uint32_t physAddress, uint32_t sizeLog2)
    {
        if (xenia::Active()) {
            xenia::InitializeRingBuffer(physAddress, sizeLog2);
            return;
        }
        g_ringBase = PhysToGuest(physAddress);
        g_ringDwords = (1u << (sizeLog2 + 3)) / 4;
        g_readPtr = 0;
        kernel::Log("gpu: ring buffer at 0x%08X, %u dwords", g_ringBase, g_ringDwords);
    }

    void EnableReadPointerWriteBack(uint32_t physAddress, uint32_t blockLog2)
    {
        if (xenia::Active()) {
            xenia::EnableReadPointerWriteBack(physAddress, blockLog2);
            return;
        }
        g_writeBack = PhysToGuest(physAddress);
        mem::Write32(g_writeBack, 0);
    }

    void SetInterruptCallback(uint32_t callback, uint32_t userData)
    {
        if (xenia::Active()) {
            xenia::SetInterruptCallback(callback, userData);
            return;
        }
        std::lock_guard lock(g_interruptLock);
        g_interruptCallback = callback;
        g_interruptUserData = userData;
    }

    void Start()
    {
        if (xenia::Active()) return;
        std::thread(CommandProcessorThread).detach();
        std::thread(VblankThread).detach();
    }
}
