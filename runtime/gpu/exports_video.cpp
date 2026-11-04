// Vd* video driver exports.

#include <cstring>

#include "gpu.h"
#include "native/live_audit.h"

#include "../kernel/kernel.h"

using namespace kernel;

namespace
{
    constexpr uint32_t kWidth = 1280, kHeight = 720;

    uint32_t FloatBits(float f)
    {
        uint32_t bits;
        memcpy(&bits, &f, 4);
        return bits;
    }

    // XVIDEO_MODE (same layout as XGetVideoMode).
    void WriteVideoMode(uint32_t mode)
    {
        memset(mem::Translate(mode), 0, 48);
        mem::Write32(mode + 0x00, kWidth);
        mem::Write32(mode + 0x04, kHeight);
        mem::Write32(mode + 0x08, 0);              // interlaced
        mem::Write32(mode + 0x0C, 1);              // widescreen
        mem::Write32(mode + 0x10, 1);              // high definition
        mem::Write32(mode + 0x14, FloatBits(60.0f));
        mem::Write32(mode + 0x18, 1);              // NTSC-M
    }

    uint32_t MakePacket3(uint32_t opcode, uint32_t count) { return (3u << 30) | ((count - 1) << 16) | (opcode << 8); }
}

KFUNC(VdQueryVideoMode) { WriteVideoMode(ARG0); RETURN(0); }
KFUNC(VdQueryVideoFlags) { RETURN(0x7); } // widescreen, >= 1024 wide, >= 720p
KFUNC(VdSetDisplayMode) { RETURN(0); }
KFUNC(VdInitializeEngines) { RETURN(1); }
KFUNC(VdShutdownEngines) { RETURN(0); }
KFUNC(VdIsHSIOTrainingSucceeded) { RETURN(1); }
KFUNC(VdRetrainEDRAM) { RETURN(0); }
KFUNC(VdRetrainEDRAMWorker) { RETURN(0); }
KFUNC(VdEnableDisableClockGating) { RETURN(0); }
KFUNC(VdCallGraphicsNotificationRoutines) { RETURN(0); }
KFUNC(VdSetSystemCommandBufferGpuIdentifierAddress) { RETURN(0); }

KFUNC(VdGetCurrentDisplayGamma)
{
    // (type_ptr, power_ptr): sRGB-ish 2.2 curve
    mem::Write32(ARG0, 1);
    mem::Write32(ARG1, FloatBits(2.22222233f));
    RETURN(0);
}

KFUNC(VdGetCurrentDisplayInformation)
{
    // X_DISPLAY_INFO (0x58 bytes)
    const uint32_t info = ARG0;
    memset(mem::Translate(info), 0, 0x58);
    *mem::Ptr<be<uint16_t>>(info + 0x00) = uint16_t(kWidth);  // front buffer
    *mem::Ptr<be<uint16_t>>(info + 0x02) = uint16_t(kHeight);
    *mem::Ptr<be<uint16_t>>(info + 0x48) = uint16_t(kWidth);  // display
    *mem::Ptr<be<uint16_t>>(info + 0x4A) = uint16_t(kHeight);
    mem::Write32(info + 0x4C, FloatBits(60.0f));
    *mem::Ptr<be<uint16_t>>(info + 0x56) = uint16_t(kWidth);
    RETURN(0);
}

KFUNC(VdPersistDisplay)
{
    if (ARG1)
        mem::Write32(ARG1, 0);
    RETURN(1);
}

KFUNC(VdGetSystemCommandBuffer)
{
    // (buffer_ptr, size_ptr): no system command buffer.
    memset(mem::Translate(ARG0), 0, 0x94);
    mem::Write32(ARG1, 0xBEEF0000);
    RETURN(0);
}

KFUNC(VdInitializeRingBuffer)
{
    gpu::InitializeRingBuffer(ARG0, ARG1);
    RETURN(0);
}

KFUNC(VdEnableRingBufferRPtrWriteBack)
{
    gpu::EnableReadPointerWriteBack(ARG0, ARG1);
    RETURN(0);
}

KFUNC(VdSetGraphicsInterruptCallback)
{
    gpu::SetInterruptCallback(ARG0, ARG1);
    RETURN(0);
}

KFUNC(VdInitializeScalerCommandBuffer)
{
    // (..., dest_ptr = stack arg 0, dest_count = stack arg 1): fill with type-2 NOPs.
    // In this XDK the count argument is a pointer to the count.
    const uint32_t dest = STACK_ARG(0), countArg = STACK_ARG(1);
    uint32_t count = countArg >= 0x10000 ? mem::Read32(countArg) : countArg;
    if (!dest)
        RETURN(0);
    if (count > 0x200)
        count = 0x200;
    for (uint32_t i = 0; i < count; i++)
        mem::Write32(dest + i * 4, 0x80000000);
    RETURN(count);
}

KFUNC(VdSwap)
{
    // (buffer_ptr, fetch_ptr, ...): buffer_ptr is 64 dwords D3D reserved in the
    // command stream for the swap packet; fetch_ptr is the front buffer's
    // texture fetch constant.
    const uint32_t buffer = ARG0, fetch = ARG1;
    const uint32_t fetchAddress = mem::Read32(fetch + 4);
    const uint32_t frontBuffer = (fetchAddress & 0xFFFFF000u) & 0x1FFFFFFFu;
    gpu::native::audit::Swap(frontBuffer);
    // The presenter reads texture fetch slot 0. The D3D texture header holds
    // a virtual address, so translate that field to the GPU physical address.
    mem::Write32(buffer, (5u << 16) | 0x4800u); // type 0, six fetch dwords
    for (uint32_t i = 0; i < 6; ++i)
        mem::Write32(buffer + 4 + i * 4, i == 1
            ? ((fetchAddress & 0xFFFu) | frontBuffer) : mem::Read32(fetch + i * 4));
    mem::Write32(buffer + 28, MakePacket3(0x64 /* PM4_XE_SWAP */, 4));
    mem::Write32(buffer + 32, 0x53574150u); // SWAP
    mem::Write32(buffer + 36, frontBuffer);
    mem::Write32(buffer + 40, STACK_ARG(0) ? mem::Read32(STACK_ARG(0)) : kWidth);
    mem::Write32(buffer + 44, STACK_ARG(1) ? mem::Read32(STACK_ARG(1)) : kHeight);
    for (uint32_t i = 12; i < 64; i++)
        mem::Write32(buffer + i * 4, 0x80000000);
    RETURN(0);
}
