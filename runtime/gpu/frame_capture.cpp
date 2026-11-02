// Frame capture for the native renderer research (docs/NATIVE_RENDERER.md).
// FD_FRAME_CAPTURE=<seconds>: on the first Swap after that delay, records every
// draw of the next frame to frame_capture.bin in the working directory: the
// call's arguments and a snapshot of the D3D device object taken just before
// the draw (the device shadows all GPU state the draw will flush).
//
// Record: u32 magic 'FDRW', u32 kind (0 indexed, 1 rect, 2 rect', 3 swap, 4 resolve/clear),
// u32 lr, u32 r3..r10, u32 snapshot size, then that many bytes of guest memory
// at r3; draws (not the swap) are followed by the referenced objects and by
// an 'FDCB' block with the command bytes the draw wrote.
// All header words are host (little-endian) order; snapshots are guest bytes.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "../kernel/kernel.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace
{
constexpr uint32_t kDeviceSnapshot = 0x8000;
constexpr uint32_t kObjectSnapshot = 0x80;
constexpr uint32_t kCodeSnapshot = 0x400;
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

void RecordTextures(uint32_t device);

void Record(uint32_t kind, PPCContext& ctx)
{
    if (g_state.load() != State::Capturing) return;
    std::lock_guard lock(g_mutex);
    if (!g_file) return;
    const uint32_t header[12] = {0x46445257, kind, uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
        ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, kDeviceSnapshot};
    fwrite(header, sizeof(header), 1, g_file);
    fwrite(mem::Translate(ctx.r3.u32), kDeviceSnapshot, 1, g_file);
    // Objects referenced by the device: pixel shader (+328C), vertex shader (+3290), index buffer
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
    // Shader microcode heads: pixel shader object +0x18, vertex shader object +0x20
    // (IM_LOAD packets confirm the types). Written in that order.
    for (auto [offset, field] : {std::pair{0x328Cu, 0x18u}, std::pair{0x3290u, 0x20u}})
    {
        const uint32_t object = mem::Read32(ctx.r3.u32 + offset);
        const uint32_t code = object ? mem::Read32(object + field) : 0;
        static const uint8_t zero[kCodeSnapshot] = {};
        const uint32_t head[2] = {offset, code};
        fwrite(head, sizeof(head), 1, g_file);
        fwrite(code >= 0xA0000000 && code < 0xC0000000 ? mem::Translate(code) : zero, kCodeSnapshot, 1, g_file);
    }
    RecordTextures(ctx.r3.u32);
    ++g_draws;
}

// Texture contents, once per base address per frame, to textures.bin:
// u32 'FDTX', 6 fetch constant dwords (host order), u32 length, guest bytes.
// The length covers mip 0 with dimensions aligned to 32 (tiling granularity).
std::vector<uint32_t> g_dumpedTextures;
FILE* g_textureFile = nullptr;

uint32_t BitsPerTexel(uint32_t format)
{
    switch (format)
    {
    // xenos::TextureFormat numbers.
    case 2: return 8;                            // k_8
    case 18: return 4;                           // DXT1
    case 19: case 20: case 49: return 8;         // DXT2_3, DXT4_5, DXN
    case 3: case 4: case 5: case 10: case 15: return 16;
    case 26: case 32: case 34: case 37: return 64; // 16_16_16_16 (+float), 32_32 (+float)
    case 35: case 38: return 128;                // 32_32_32_32 (+float)
    default: return 32;
    }
}

void RecordTextures(uint32_t device)
{
    if (!g_textureFile) g_textureFile = fopen("textures.bin", "wb");
    if (!g_textureFile) return;
    for (uint32_t slot = 0; slot < 16; ++slot)
    {
        uint32_t fetch[6];
        for (uint32_t i = 0; i < 6; ++i) fetch[i] = mem::Read32(device + 0x480 + slot * 24 + i * 4);
        if ((fetch[0] & 3) != 2 || !(fetch[1] >> 12)) continue;
        const uint32_t base = fetch[1] & 0xFFFFF000;
        if (std::find(g_dumpedTextures.begin(), g_dumpedTextures.end(), base) != g_dumpedTextures.end()) continue;
        g_dumpedTextures.push_back(base);
        const uint32_t dimension = (fetch[5] >> 9) & 3;
        uint32_t width = 1, height = 1;
        if (dimension == 1 || dimension == 3) { width = (fetch[2] & 0x1FFF) + 1; height = ((fetch[2] >> 13) & 0x1FFF) + 1; }
        else if (dimension == 0) width = (fetch[2] & 0xFFFFFF) + 1;
        else { width = (fetch[2] & 0x7FF) + 1; height = ((fetch[2] >> 11) & 0x7FF) + 1; }
        const uint32_t pitch = std::max(width, ((fetch[0] >> 22) & 0x1FF) << 5);
        const uint64_t bytes = uint64_t((pitch + 31) & ~31u) * ((height + 31) & ~31u) * BitsPerTexel(fetch[1] & 0x3F) / 8;
        const uint32_t length = uint32_t(std::min<uint64_t>(bytes, 16u << 20));
        const uint32_t guest = mem::kPhysicalBase | (base & 0x1FFFFFFF);
        const uint32_t head[1] = {0x46445458};
        fwrite(head, sizeof(head), 1, g_textureFile);
        fwrite(fetch, sizeof(fetch), 1, g_textureFile);
        fwrite(&length, sizeof(length), 1, g_textureFile);
        fwrite(mem::Translate(guest), length, 1, g_textureFile);
    }
}

// Vertex and index buffer contents, once per (address, length) per frame, to
// buffers.bin: u32 'FDBF', u32 physical address, u32 length, guest bytes.
std::vector<std::pair<uint32_t, uint32_t>> g_dumpedBuffers;
uint32_t g_indexDraws = 0;
FILE* g_bufferFile = nullptr;

void DumpBuffer(uint32_t physical, uint32_t length)
{
    physical &= 0x1FFFFFFF;
    if (!length || length > (32u << 20) || physical + length > 0x20000000) return;
    const std::pair key{physical, length};
    if (std::find(g_dumpedBuffers.begin(), g_dumpedBuffers.end(), key) != g_dumpedBuffers.end()) return;
    g_dumpedBuffers.push_back(key);
    if (!g_bufferFile) g_bufferFile = fopen("buffers.bin", "wb");
    if (!g_bufferFile) return;
    const uint32_t head[3] = {0x46444246, physical, length};
    fwrite(head, sizeof(head), 1, g_bufferFile);
    fwrite(mem::Translate(mem::kPhysicalBase | physical), length, 1, g_bufferFile);
}

// Vertex fetch constants in the device copy of registers 0x4800+. Each 6-dword
// slot holds one texture fetch (type 2) or three vertex fetches (type 3).
void RecordVertexBuffers(uint32_t device)
{
    for (uint32_t slot = 0; slot < 32; ++slot)
    {
        const uint32_t base = device + 0x480 + slot * 24;
        if ((mem::Read32(base) & 3) == 2) continue; // texture slot
        for (uint32_t pair = 0; pair < 3; ++pair)
        {
            const uint32_t word0 = mem::Read32(base + pair * 8), word1 = mem::Read32(base + pair * 8 + 4);
            if ((word0 & 3) == 3) DumpBuffer(word0 & ~3u, ((word1 >> 2) & 0xFFFFFF) * 4);
        }
    }
}

// Index buffers: DRAW_INDX packets in the draw's commands (type 3, opcode
// 0x22): payload [viz, count << 16 | flags, index base, index size].
void RecordIndexBuffers(const uint8_t* commands, uint32_t length)
{
    for (uint32_t i = 0; i + 4 <= length;)
    {
        const uint32_t header = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(commands + i));
        const uint32_t type = header >> 30, count = ((header >> 16) & 0x3FFF) + 1;
        if (type == 3 && ((header >> 8) & 0x7F) == 0x22 && count >= 4 && i + 20 <= length)
        {
            const auto word = [&](uint32_t n) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(commands + i + 4 + n * 4)); };
            const uint32_t indices = word(1) >> 16;
            const bool index32 = (word(1) >> 11) & 1, indexed = ((word(1) >> 6) & 3) == 0;
            if (indexed) DumpBuffer(word(2), indices * (index32 ? 4 : 2)), ++g_indexDraws;
        }
        i += type == 2 ? 4 : type == 1 ? 12 : 4 + 4 * count;
    }
}

// After the original draw: the command bytes it wrote (state flush + draw
// packets), from the cursor at device+0x30 before the call to after it.
// Block: u32 'FDCB', u32 length, guest bytes (length 0 if the buffer wrapped).
void RecordCommands(uint32_t device, uint32_t before)
{
    if (g_state.load() != State::Capturing) return;
    std::lock_guard lock(g_mutex);
    if (!g_file) return;
    const uint32_t after = mem::Read32(device + 0x30);
    // The cursor points at the last written word: the draw wrote
    // [before + 4, after + 4). Saved from `before` (one word early) so tools
    // skip 4 bytes, up to and including the last word.
    const uint32_t length = after > before && after - before < 0x40000 ? after - before + 4 : 0;
    const uint32_t head[2] = {0x46444342, length};
    fwrite(head, sizeof(head), 1, g_file);
    if (length)
    {
        fwrite(mem::Translate(before), length, 1, g_file);
        RecordIndexBuffers(static_cast<const uint8_t*>(mem::Translate(before)) + 4, length - 4);
    }
    RecordVertexBuffers(device);
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
        if (g_textureFile) fclose(g_textureFile), g_textureFile = nullptr;
        if (g_bufferFile) fclose(g_bufferFile), g_bufferFile = nullptr;
        g_state = State::Done;
        kernel::Log("frame capture: %u draws written to frame_capture.bin (%u indexed, %zu buffers)", g_draws,
            g_indexDraws, g_dumpedBuffers.size());
    }
}
}

// DrawIndexedVertices (scene meshes).
PPC_FUNC_IMPL(__imp__sub_8250DDA0);
PPC_FUNC(sub_8250DDA0)
{
    const uint32_t device = ctx.r3.u32, cursor = mem::Read32(device + 0x30);
    Record(0, ctx);
    __imp__sub_8250DDA0(ctx, base);
    RecordCommands(device, cursor);
}

// DrawVertices with rect lists (full-screen passes).
PPC_FUNC_IMPL(__imp__sub_8250D988);
PPC_FUNC(sub_8250D988)
{
    const uint32_t device = ctx.r3.u32, cursor = mem::Read32(device + 0x30);
    Record(1, ctx);
    __imp__sub_8250D988(ctx, base);
    RecordCommands(device, cursor);
}

PPC_FUNC_IMPL(__imp__sub_8250D480);
PPC_FUNC(sub_8250D480)
{
    const uint32_t device = ctx.r3.u32, cursor = mem::Read32(device + 0x30);
    Record(2, ctx);
    __imp__sub_8250D480(ctx, base);
    RecordCommands(device, cursor);
}

// Swap (calls VdSwap).
PPC_FUNC_IMPL(__imp__sub_820E0AC8);
PPC_FUNC(sub_820E0AC8)
{
    OnSwap(ctx);
    __imp__sub_820E0AC8(ctx, base);
}

// Shader object initialisation (identified by the local Qwen model): r3 is the
// new object, r4 the microcode, not yet patched with vertex fetch bindings.
// With FD_FRAME_CAPTURE set, each one is appended to shader_inits.bin:
// u32 'FDSI', u32 kind (0 vertex, 1 pixel), u32 object, u32 microcode,
// kCodeSnapshot bytes of microcode.
namespace
{
void RecordShaderInit(uint32_t kind, PPCContext& ctx)
{
    if (!Enabled()) return;
    std::lock_guard lock(g_mutex);
    static FILE* file = fopen("shader_inits.bin", "wb");
    if (!file) return;
    const uint32_t head[4] = {0x46445349, kind, ctx.r3.u32, ctx.r4.u32};
    fwrite(head, sizeof(head), 1, file);
    static const uint8_t zero[kCodeSnapshot] = {};
    const uint32_t code = ctx.r4.u32;
    fwrite(code >= 0x40000000 && code < 0xC0000000 ? mem::Translate(code) : zero, kCodeSnapshot, 1, file);
    fflush(file);
}
}

PPC_FUNC_IMPL(__imp__sub_820DB120);
PPC_FUNC(sub_820DB120)
{
    RecordShaderInit(0, ctx);
    __imp__sub_820DB120(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_820DA850);
PPC_FUNC(sub_820DA850)
{
    RecordShaderInit(1, ctx);
    __imp__sub_820DA850(ctx, base);
}

// D3DDevice_Resolve (820EE9D0, ~50 per frame; writes RB_COPY_* in 820ED880;
// 820EFD40 is D3DDevice_Clear). r6 is the destination texture object.
PPC_FUNC_IMPL(__imp__sub_820EE9D0);
PPC_FUNC(sub_820EE9D0)
{
    const uint32_t device = ctx.r3.u32, cursor = mem::Read32(device + 0x30);
    const uint32_t flags = ctx.r4.u32, texture = ctx.r6.u32;
    Record(4, ctx);
    __imp__sub_820EE9D0(ctx, base);
    RecordCommands(device, cursor);
    // Block: u32 'FDRS', u32 flags (r4), u32 destination texture object (r6),
    // 0x40 bytes of that object (fetch constant at +0x1C).
    if (g_state.load() == State::Capturing)
    {
        std::lock_guard lock(g_mutex);
        if (g_file)
        {
            static const uint8_t zero[0x40] = {};
            const uint32_t head[3] = {0x46445253, flags, texture};
            fwrite(head, sizeof(head), 1, g_file);
            fwrite(texture >= 0x40000000 && texture < 0xC0000000 ? mem::Translate(texture) : zero, 0x40, 1, g_file);
        }
    }
}
