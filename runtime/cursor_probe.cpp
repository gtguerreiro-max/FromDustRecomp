// FD_PROBE_STICK=<seconds>: diagnostic. dustai sub_8A1FA470 reads the pad
// stick (IO_JoystickStickGet, with the invert-axis options) into this+32..40.
// After the delay, a read/write hardware watchpoint on that vector logs the
// guest code that consumes it (the cursor movement).
#include "kernel/kernel.h"
#include <cstdlib>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace kernel::debug { void ArmWriteWatch(uint32_t address, bool readsToo); }

namespace { const ULONGLONG g_processStart = GetTickCount64(); }
PPC_FUNC_IMPL(__imp__sub_8A1FA470);
PPC_FUNC(sub_8A1FA470)
{
    static const char* probe = getenv("FD_PROBE_STICK");
    static const ULONGLONG start = g_processStart;
    static bool armed = false, seen = false;
    if (probe && !seen) { seen = true; kernel::Log("stick probe: first call, object %08X", ctx.r3.u32); }
    if (probe && !armed && GetTickCount64() - start >= ULONGLONG(atof(probe) * 1000))
    {
        armed = true;
        const uint32_t stick = ctx.r3.u32 + 32;
        kernel::Log("stick probe: input object %08X, stick vector %08X", ctx.r3.u32, stick);
        std::thread([stick] { kernel::debug::ArmWriteWatch(stick, true); }).detach();
    }
    __imp__sub_8A1FA470(ctx, base);
}

// Mouse pointing: dustai sub_8A1F4298 is the Breath cursor update. It adds
// the stick displacement (camera relative) to the cursor position, a vec3 at
// +1744 of the global cursor object (pointer at 0x8A295318), projects it on
// the terrain and stores it back. After it runs, the cursor is moved to the
// point of its height plane under the mouse; the next update re-projects it.
#include "pointer.h"
#include "input.h"
#include <cmath>
#include <cstring>
namespace
{
float ReadFloat(uint32_t address) { uint32_t v = mem::Read32(address); float f; memcpy(&f, &v, 4); return f; }
void WriteFloat(uint32_t address, float value) { uint32_t v; memcpy(&v, &value, 4); mem::Write32(address, v); }
}
PPC_FUNC_IMPL(__imp__sub_8A1F4298);
PPC_FUNC(sub_8A1F4298)
{
    __imp__sub_8A1F4298(ctx, base);
    const uint32_t cursor = mem::Read32(0x8A295318);
    if (!cursor || cursor < 0x40000000u || cursor >= 0xC0000000u) return;
    const float height = ReadFloat(cursor + 1752);
    float x, y;
    if (!std::isfinite(height) || !pointer::Target(height, x, y)) return;
    static const bool trace = getenv("FD_TRACE_POINTER") != nullptr;
    if (trace)
    {
        static ULONGLONG last = 0;
        if (GetTickCount64() - last > 500) {
            last = GetTickCount64();
            float sx = -1, sy = -1, mx = -1, my = -1;
            pointer::Project(ReadFloat(cursor + 1744), ReadFloat(cursor + 1748), height, sx, sy);
            pcinput::MousePosition(mx, my);
            kernel::Log("pointer: t %.1f cursor %08X (%.2f %.2f %.2f) on screen (%.3f %.3f), mouse (%.3f %.3f) -> (%.2f %.2f)", (GetTickCount64() - g_processStart) / 1000.0, cursor,
                ReadFloat(cursor + 1744), ReadFloat(cursor + 1748), height, sx, sy, mx, my, x, y);
        }
    }
    static const bool dry = getenv("FD_POINTER_DRY") != nullptr;
    if (dry) return;
    WriteFloat(cursor + 1744, x);
    WriteFloat(cursor + 1748, y);
}
