// FD_PROBE_STICK=<seconds>: diagnostic. dustai sub_8A1FA470 reads the pad
// stick (IO_JoystickStickGet, with the invert-axis options) into this+32..40.
// After the delay, a read/write hardware watchpoint on that vector logs the
// guest code that consumes it (the cursor movement).
#include "kernel/kernel.h"
#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace kernel::debug { void ArmWriteWatch(uint32_t address, bool readsToo); void ArmWatchSlot(uint32_t address, bool readsToo, int slot); }

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
#include "camera_assist.h"
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
    const uint32_t self = ctx.r3.u32;
    __imp__sub_8A1F4298(ctx, base);
    const uint32_t cursor = mem::Read32(0x8A295318);
    if (!cursor || cursor < 0x40000000u || cursor >= 0xC0000000u) return;
    // FD_PROBE_RB=<seconds>: diagnostic. The pad input object (the one this
    // update reads the sticks from) keeps, per button i (17), the mask at
    // +496+4i and pressed / just pressed / just released flags at +284 / +352
    // / +420 + 4i. Log the masks, then watch the RB (mask 32 or 0x200) flags.
    static const char* probeRb = getenv("FD_PROBE_RB");
    static bool rbArmed = false;
    if (probeRb && !rbArmed && GetTickCount64() - g_processStart >= ULONGLONG(atof(probeRb) * 1000))
    {
        rbArmed = true;
        const uint32_t holder = mem::Read32(0x8A282690 + 12), input = holder ? mem::Read32(holder + 68) : 0;
        std::string masks;
        int rb = -1;
        for (int i = 0; input && i < 17; ++i)
        {
            const uint32_t mask = mem::Read32(input + 496 + i * 4);
            masks += " " + std::to_string(mask);
            if (mask == 32 || mask == 0x200) rb = i;
        }
        kernel::Log("rb probe: input %08X masks%s, rb index %d", input, masks.c_str(), rb);
        if (rb >= 0)
            std::thread([input, rb] {
                kernel::debug::ArmWatchSlot(input + 352 + rb * 4, true, 0);
                kernel::debug::ArmWatchSlot(input + 420 + rb * 4, true, 1);
                kernel::debug::ArmWatchSlot(input + 284 + rb * 4, true, 2);
            }).detach();
    }
    // FD_PROBE_CURSOR=<seconds>: diagnostic read/write watchpoint on the
    // cursor vector (+1744), logging every guest function touching it.
    static const char* probeCursor = getenv("FD_PROBE_CURSOR");
    static bool cursorArmed = false;
    if (probeCursor && !cursorArmed && GetTickCount64() - g_processStart >= ULONGLONG(atof(probeCursor) * 1000))
    {
        cursorArmed = true;
        kernel::Log("cursor probe: object %08X, vector %08X", cursor, cursor + 1744);
        std::thread([cursor] { kernel::debug::ArmWriteWatch(cursor + 1744, true); }).detach();
    }
    const float height = ReadFloat(cursor + 1752);
    float x, y;
    if (!std::isfinite(height)) return;
    // The global view check measures the camera's distance to what it
    // follows: the edge scrolling focus when that holds the camera.
    float focusX = ReadFloat(cursor + 1744), focusY = ReadFloat(cursor + 1748), focusZ = height;
    pointer::Focus(focusX, focusY, focusZ);
    if (float distance; pointer::CameraDistance(focusX, focusY, focusZ, distance))
        camera::Update(distance);
    static const bool traceCamera = getenv("FD_TRACE_CAMERA_DISTANCE") != nullptr;
    if (traceCamera)
    {
        static ULONGLONG last = 0;
        float distance;
        if (GetTickCount64() - last > 500 && pointer::CameraDistance(ReadFloat(cursor + 1744), ReadFloat(cursor + 1748), height, distance))
        {
            last = GetTickCount64();
            kernel::Log("camera distance: t %.1f %.1f", (GetTickCount64() - g_processStart) / 1000.0, distance);
        }
    }
    const float liveX = ReadFloat(cursor + 1744), liveY = ReadFloat(cursor + 1748);
    bool moved = pcinput::MouseAbsolute() ? pointer::Target(height, x, y) : pointer::Grab(liveX, liveY, height, x, y);
    if (!moved)
    {
        if (!pointer::Active()) { pointer::EdgeScroll(liveX, liveY, x, y, height); return; }
        // Grabbed but idle: keep the position (no snapping to villagers);
        // edge scrolling may still carry it while it rests at an edge.
        x = liveX; y = liveY;
    }
    // The game's own limit (flag 0x80 of +380): within radius +340 of +304.
    if (mem::Read32(self + 380) & 0x80)
    {
        const float cx = ReadFloat(self + 304), cy = ReadFloat(self + 308), radius = ReadFloat(self + 340);
        const float ox = x - cx, oy = y - cy, distance = std::sqrt(ox * ox + oy * oy);
        if (std::isfinite(radius) && radius > 0 && distance > radius) { x = cx + ox * radius / distance; y = cy + oy * radius / distance; }
    }
    // Edge scrolling: on screen, carried by the camera at the edges.
    if (pointer::EdgeScroll(liveX, liveY, x, y, height) && !moved && (x != liveX || y != liveY)) moved = true;
    if (!moved)
    {
        if (ReadFloat(self + 280) == 0.0f && ReadFloat(self + 284) == 0.0f && ReadFloat(self + 288) == 0.0f)
            WriteFloat(self + 280, 1e-6f);
        return;
    }
    static const bool trace = getenv("FD_TRACE_POINTER") != nullptr;
    if (trace)
    {
        static ULONGLONG last = 0;
        if (GetTickCount64() - last > 500) {
            last = GetTickCount64();
            float sx = -1, sy = -1, mx = -1, my = -1;
            pointer::Project(ReadFloat(cursor + 1744), ReadFloat(cursor + 1748), height, sx, sy);
            pcinput::MousePosition(mx, my);
            float fx = 0, fy = 0, fz = 0;
            const bool focus = pointer::Focus(fx, fy, fz);
            kernel::Log("pointer: t %.1f cursor %08X (%.2f %.2f %.2f) on screen (%.3f %.3f), mouse (%.3f %.3f) -> (%.2f %.2f), focus %s(%.2f %.2f)", (GetTickCount64() - g_processStart) / 1000.0, cursor,
                ReadFloat(cursor + 1744), ReadFloat(cursor + 1748), height, sx, sy, mx, my, x, y, focus ? "" : "off ", fx, fy);
        }
    }
    static const bool dry = getenv("FD_POINTER_DRY") != nullptr;
    if (dry) return;
    WriteFloat(cursor + 1744, x);
    WriteFloat(cursor + 1748, y);
    // sub_8A1F5BF0 snaps the cursor to a nearby object (villagers) while the
    // left stick copy at +280 is zero; a negligible non-zero value keeps the
    // pointed position.
    if (ReadFloat(self + 280) == 0.0f && ReadFloat(self + 284) == 0.0f && ReadFloat(self + 288) == 0.0f)
        WriteFloat(self + 280, 1e-6f);
}

// FD_TRACE_RB=1: diagnostic. sub_8A1A1FC8 is an AI-callable check for an RB
// tap (RB released while LB / LT / RT are not held); log who asks when it
// reports one (the camera's switch to the global view).
namespace kernel::debug { void TraceGuestChain(const char* tag); }
namespace
{
void TraceShoulder(const char* name, uint32_t result)
{
    static const bool trace = getenv("FD_TRACE_RB") != nullptr;
    if (!trace) return;
    static std::atomic<int> calls{0};
    if (calls++ < 3) kernel::Log("shoulder probe: %s called (result %u)", name, result);
    if (result) kernel::debug::TraceGuestChain(name);
}
}
PPC_FUNC_IMPL(__imp__sub_8A1A1FC8);
PPC_FUNC(sub_8A1A1FC8)
{
    __imp__sub_8A1A1FC8(ctx, base);
    TraceShoulder("rb tap (32)", ctx.r3.u32);
}
PPC_FUNC_IMPL(__imp__sub_8A1A1F00);
PPC_FUNC(sub_8A1A1F00)
{
    __imp__sub_8A1A1F00(ctx, base);
    TraceShoulder("shoulder tap (16)", ctx.r3.u32);
}

// FD_PROBE_RB (continued): sub_8A043680 is a per-button component: +20 points
// at its button's "pressed" flag in the pad input object (just pressed +68,
// just released +136), +28 is the action whose virtual methods it calls. Log
// the RB component's action and vtable once.
PPC_FUNC_IMPL(__imp__sub_8A043680);
PPC_FUNC(sub_8A043680)
{
    static const char* probeRb = getenv("FD_PROBE_RB");
    if (probeRb)
    {
        static std::atomic<int> logged{0};
        const uint32_t holder = mem::Read32(0x8A282690 + 12), input = holder ? mem::Read32(holder + 68) : 0;
        const uint32_t flags = mem::Read32(ctx.r3.u32 + 20), action = mem::Read32(ctx.r3.u32 + 28);
        if (input && flags >= input + 284 && flags < input + 284 + 17 * 4 && logged < 17)
        {
            static uint32_t seen[17] = {};
            const uint32_t index = (flags - input - 284) / 4;
            if (!seen[index]) {
                seen[index] = 1; ++logged;
                const uint32_t vtable = action ? mem::Read32(action) : 0;
                std::string methods;
                for (int k = 0; vtable && k < 20; ++k) { char text[16]; snprintf(text, sizeof(text), " %08X", mem::Read32(vtable + k * 4)); methods += text; }
                kernel::Log("rb probe: button %u (mask %u) component %08X action %08X vtable %08X:%s", index,
                    mem::Read32(input + 496 + index * 4), ctx.r3.u32, action, vtable, methods.c_str());
            }
        }
    }
    __imp__sub_8A043680(ctx, base);
}

// Edge scrolling: dustai sub_8A1FF3B0 is the camera's follow node. It copies
// the cursor vector (+1744 of the global cursor object) into its target
// (this +16, the previous one at +28). Found by giving each reader of the
// vector (a read watchpoint, FD_PROBE_CURSOR) a frozen copy for a few seconds
// while the Breath moved: only with this one the Breath left the screen
// centre. While edge scrolling holds the camera, the node sees the focus
// point instead (swapped in around the call, the Breath's own position is
// restored right after).
PPC_FUNC_IMPL(__imp__sub_8A1FF3B0);
PPC_FUNC(sub_8A1FF3B0)
{
    const uint32_t cursor = mem::Read32(0x8A295318);
    float fx, fy, fz;
    if (!cursor || cursor < 0x40000000u || cursor >= 0xC0000000u || !pointer::Focus(fx, fy, fz))
    {
        __imp__sub_8A1FF3B0(ctx, base);
        return;
    }
    const uint32_t live[3] = {mem::Read32(cursor + 1744), mem::Read32(cursor + 1748), mem::Read32(cursor + 1752)};
    WriteFloat(cursor + 1744, fx);
    WriteFloat(cursor + 1748, fy);
    WriteFloat(cursor + 1752, fz);
    __imp__sub_8A1FF3B0(ctx, base);
    for (int k = 0; k < 3; ++k) mem::Write32(cursor + 1744 + 4 * k, live[k]);
}
