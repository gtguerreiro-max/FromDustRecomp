// Frame capture for the native renderer research (docs/NATIVE_RENDERER.md).
// FD_FRAME_CAPTURE=<seconds>: on the first Swap after that delay, records every
// draw of the next frame to frame_capture.bin in the working directory: the
// call's arguments and a snapshot of the D3D device object taken just before
// the draw (the device shadows all GPU state the draw will flush).
//
// Record: u32 magic 'FDRW', u32 kind (0 indexed, 1 rect, 2 rect', 3 swap),
// u32 lr, u32 r3..r10, u32 snapshot size, then that many bytes of guest memory
// at r3; draws (not the swap) are followed by the referenced objects.
// All header words are host (little-endian) order; snapshots are guest bytes.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "../kernel/kernel.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace
{
constexpr uint32_t kDeviceSnapshot = 0x8000;
constexpr uint32_t kObjectSnapshot = 0x80;
constexpr uint32_t kObjectOffsets[] = {0x328C, 0x3290, 0x2F58, 0x318C, 0x3190, 0x3194, 0x3198, 0x319C, 0x31A0, 0x31A4,
    0x31F8, 0x31FC, 0x3200, 0x3204, 0x3208, 0x320C, 0x3210, 0x3214, 0x3218, 0x321C, 0x3220, 0x3224, 0x3228, 0x322C,
    0x3230, 0x3234};
enum class State { Waiting, Armed, Capturing, Done };
std::atomic<State> g_state{State::Waiting};
std::mutex g_mutex;
FILE* g_file = nullptr;
ULONGLONG g_armAt = 0;
uint32_t g_draws = 0;

bool Enabled()
{
    static const unsigned delay = [] {
        const char* value = getenv("FD_FRAME_CAPTURE");
        return value && *value ? unsigned(atoi(value)) : 0u;
    }();
    if (!delay) return false;
    if (!g_armAt) g_armAt = GetTickCount64() + delay * 1000ull;
    return true;
}

void Record(uint32_t kind, PPCContext& ctx)
{
    if (g_state.load() != State::Capturing) return;
    std::lock_guard lock(g_mutex);
    if (!g_file) return;
    const uint32_t header[12] = {0x46445257, kind, uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
        ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, kDeviceSnapshot};
    fwrite(header, sizeof(header), 1, g_file);
    fwrite(mem::Translate(ctx.r3.u32), kDeviceSnapshot, 1, g_file);
    // Objects referenced by the device: shaders (+328C, +3290), index buffer
    // (+2F58), render targets (+318C..+31A4) and textures (+31F8..+3234).
    // Each: u32 device offset, u32 pointer, kObjectSnapshot bytes (zero if null).
    for (uint32_t offset : kObjectOffsets)
    {
        const uint32_t pointer = mem::Read32(ctx.r3.u32 + offset);
        static const uint8_t zero[kObjectSnapshot] = {};
        const uint32_t head[2] = {offset, pointer};
        fwrite(head, sizeof(head), 1, g_file);
        fwrite(pointer >= 0x40000000 && pointer < 0xC0000000 ? mem::Translate(pointer) : zero, kObjectSnapshot, 1, g_file);
    }
    // Shader microcode heads: vertex shader object +0x18, pixel shader object +0x20.
    for (auto [offset, field] : {std::pair{0x328Cu, 0x18u}, std::pair{0x3290u, 0x20u}})
    {
        const uint32_t object = mem::Read32(ctx.r3.u32 + offset);
        const uint32_t code = object ? mem::Read32(object + field) : 0;
        static const uint8_t zero[kObjectSnapshot] = {};
        const uint32_t head[2] = {offset, code};
        fwrite(head, sizeof(head), 1, g_file);
        fwrite(code >= 0xA0000000 && code < 0xC0000000 ? mem::Translate(code) : zero, kObjectSnapshot, 1, g_file);
    }
    ++g_draws;
}

// Frame boundary: arm at the first swap after the delay, capture until the next.
void OnSwap(PPCContext& ctx)
{
    if (!Enabled()) return;
    std::lock_guard lock(g_mutex);
    const State state = g_state.load();
    if (state == State::Waiting && GetTickCount64() >= g_armAt)
    {
        g_file = fopen("frame_capture.bin", "wb");
        g_state = g_file ? State::Capturing : State::Done;
    }
    else if (state == State::Capturing)
    {
        const uint32_t header[12] = {0x46445257, 3, uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
            ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, kDeviceSnapshot};
        fwrite(header, sizeof(header), 1, g_file);
        fwrite(mem::Translate(ctx.r3.u32), kDeviceSnapshot, 1, g_file);
        fclose(g_file);
        g_file = nullptr;
        g_state = State::Done;
        kernel::Log("frame capture: %u draws written to frame_capture.bin", g_draws);
    }
}
}

// DrawIndexedVertices (scene meshes).
PPC_FUNC_IMPL(__imp__sub_8250DDA0);
PPC_FUNC(sub_8250DDA0)
{
    Record(0, ctx);
    __imp__sub_8250DDA0(ctx, base);
}

// DrawVertices with rect lists (full-screen passes).
PPC_FUNC_IMPL(__imp__sub_8250D988);
PPC_FUNC(sub_8250D988)
{
    Record(1, ctx);
    __imp__sub_8250D988(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_8250D480);
PPC_FUNC(sub_8250D480)
{
    Record(2, ctx);
    __imp__sub_8250D480(ctx, base);
}

// Swap (calls VdSwap).
PPC_FUNC_IMPL(__imp__sub_820E0AC8);
PPC_FUNC(sub_820E0AC8)
{
    OnSwap(ctx);
    __imp__sub_820E0AC8(ctx, base);
}
