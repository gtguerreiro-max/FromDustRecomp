// Frame capture for the native renderer research (docs/NATIVE_RENDERER.md).
// FD_FRAME_CAPTURE=<seconds>: on the first Swap after that delay, records every
// draw of the next frame to frame_capture.bin in the working directory: the
// call's arguments and a snapshot of the D3D device object taken just before
// the draw (the device shadows all GPU state the draw will flush).
//
// Record: u32 magic 'FDRW', u32 kind (0 indexed, 1 rect, 2 rect', 3 swap, 4 resolve, 5 clear),
// u32 lr, u32 r3..r10, u32 snapshot size, then that many bytes of guest memory
// at r3; draws (not the swap) are followed by the referenced objects and by
// an 'FDCB' block with the command bytes the draw wrote.
// All header words are host (little-endian) order; snapshots are guest bytes.

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "../kernel/kernel.h"
#include "native/live_audit.h"
#ifdef FD_NATIVE_RENDERER
#include "native/texture_tables.h"
#include "native/frame_builder.h"
#include "native/renderer.h"
#include "native/replay_frame.h"
#include <map>
#include <memory>
#include <filesystem>
#include <stdexcept>
#endif
#ifdef FD_XENIA_GPU
#include "xenia/gpu/texture_util.h"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#if __has_include("../../third_party/renderdoc_dl/RenderDoc_1.47_64/renderdoc_app.h")
#include "../../third_party/renderdoc_dl/RenderDoc_1.47_64/renderdoc_app.h"
#define FD_HAVE_RENDERDOC 1
#endif

// Called by Xenia's D3D12 command processor (FROMDUST_GPU_SHIM) at every draw and swap.
extern "C" void (*fd_gpu_event_hook)(int event, const uint32_t* registers) = nullptr;
extern "C" void (*fd_gpu_texture_hook)(uint32_t mask, const uint32_t* registers) = nullptr;

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
uint32_t g_frameIndexed = 0;

unsigned MinimumIndexedDraws()
{
    static const unsigned count = [] {
        const char* value = getenv("FD_CAPTURE_MIN_INDEXED");
        return value ? unsigned(std::max(atoi(value), 0)) : 0u;
    }();
    return count;
}

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

// BeginVertices-style draws (kind 2) reserve command buffer space that the
// caller fills with vertices afterwards; the draw packet follows at
// EndVertices. At the next recorded call (or the swap) the region is saved
// again, now complete. Block: u32 'FDUP', u32 guest address, u32 length, bytes.
uint32_t g_upDevice = 0, g_upBefore = 0;
void RecordVertexBuffers(uint32_t device);
void DumpBuffer(uint32_t address, uint32_t length);

void FlushPendingVertices()
{
    if (!g_upDevice || !g_file) return;
    const uint32_t after = mem::Read32(g_upDevice + 0x30);
    const uint32_t length = after > g_upBefore && after - g_upBefore < 0x80000 ? after - g_upBefore + 4 : 0;
    const uint32_t head[3] = {0x50554446, g_upBefore, length};
    fwrite(head, sizeof(head), 1, g_file);
    if (length) fwrite(mem::Translate(g_upBefore), length, 1, g_file);
    // The vertices themselves (a vertex ring, through fetch constant 95, which
    // the region sets with a type-0 write of registers 0x48BA-0x48BF).
    RecordVertexBuffers(g_upDevice);
    for (uint32_t at = 4; at + 28 <= length; at += 4)
        if (mem::Read32(g_upBefore + at) == 0x000548BA)
        {
            const uint32_t word0 = mem::Read32(g_upBefore + at + 20), word1 = mem::Read32(g_upBefore + at + 24);
            if ((word0 & 3) == 3) DumpBuffer(word0 & ~3u, ((word1 >> 2) & 0xFFFFFF) * 4);
        }
    g_upDevice = 0;
}

void Record(uint32_t kind, PPCContext& ctx)
{
    if (g_state.load() != State::Capturing) return;
    std::lock_guard lock(g_mutex);
    if (!g_file) return;
    FlushPendingVertices();
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
uint32_t g_referenceFetch[6] = {};

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

void RecordTexture(const uint32_t* fetch, FILE* file, std::vector<uint32_t>& dumped)
{
    if (!file || (fetch[0] & 3) != 2 || !(fetch[1] >> 12)) return;
    const uint32_t base = fetch[1] & 0xFFFFF000;
    if (std::find(dumped.begin(), dumped.end(), base) != dumped.end()) return;
    const uint32_t dimension = (fetch[5] >> 9) & 3;
    uint32_t width = 1, height = 1;
    if (dimension == 1 || dimension == 3) { width = (fetch[2] & 0x1FFF) + 1; height = ((fetch[2] >> 13) & 0x1FFF) + 1; }
    else if (dimension == 0) width = (fetch[2] & 0xFFFFFF) + 1;
    else { width = (fetch[2] & 0x7FF) + 1; height = ((fetch[2] >> 11) & 0x7FF) + 1; }
    const uint32_t pitch = std::max(width, ((fetch[0] >> 22) & 0x1FF) << 5);
    const uint32_t format = fetch[1] & 0x3F;
    const bool compressed = format == 18 || format == 19 || format == 20 || format == 49;
    const uint32_t block = compressed ? 4 : 1;
    const uint32_t blockBytes = compressed ? (format == 18 ? 8 : 16) : BitsPerTexel(format) / 8;
    // Storage aligns to 32 blocks, including for BC textures. Cube faces
    // have a separate, 4 KB-aligned stride, not tightly packed texels.
    const uint64_t faceBytes = uint64_t(((pitch + block - 1) / block + 31) & ~31u) *
        (((height + block - 1) / block + 31) & ~31u) * blockBytes;
    const uint64_t bytes = dimension == 3 ? ((faceBytes + 4095) & ~uint64_t(4095)) * 6 : faceBytes;
    const uint32_t length = uint32_t(std::min<uint64_t>(bytes, 16u << 20));
    if (uint64_t(base & 0x1FFFFFFF) + length > 0x20000000) return;
    dumped.push_back(base);
    const uint32_t guest = mem::kPhysicalBase | (base & 0x1FFFFFFF);
    const uint32_t head[1] = {0x46445458};
    fwrite(head, sizeof(head), 1, file);
    fwrite(fetch, 6 * sizeof(uint32_t), 1, file);
    fwrite(&length, sizeof(length), 1, file);
    fwrite(mem::Translate(guest), length, 1, file);
}

void RecordTextures(uint32_t device)
{
    if (!g_textureFile) g_textureFile = fopen("textures.bin", "wb");
    for (uint32_t slot = 0; slot < 16; ++slot)
    {
        uint32_t fetch[6];
        for (uint32_t i = 0; i < 6; ++i) fetch[i] = mem::Read32(device + 0x480 + slot * 24 + i * 4);
        RecordTexture(fetch, g_textureFile, g_dumpedTextures);
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
    uint32_t lastIndexAddress = 0;
    for (uint32_t i = 0; i + 4 <= length;)
    {
        const uint32_t header = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(commands + i));
        const uint32_t type = header >> 30, count = ((header >> 16) & 0x3FFF) + 1;
        if (type == 3 && ((header >> 8) & 0x7F) == 0x22 && count >= 4 && i + 20 <= length)
        {
            const auto word = [&](uint32_t n) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(commands + i + 4 + n * 4)); };
            const uint32_t indices = word(1) >> 16;
            const bool index32 = (word(1) >> 11) & 1, indexed = ((word(1) >> 6) & 3) == 0;
            if (indexed)
            {
                lastIndexAddress = word(2);
                DumpBuffer(lastIndexAddress, indices * (index32 ? 4 : 2));
                ++g_indexDraws;
            }
        }
        i += type == 2 ? 4 : type == 1 ? 12 : 4 + 4 * count;
    }
    // Inline data may resemble a legal PM4 header and hide the actual final
    // draw from the sequential parser. The completed D3D block ends with its
    // real draw; recover that packet independently, as the replay packer does.
    for (uint32_t end = length & ~3u; end >= 20; end -= 4)
    {
        const uint8_t* packet = commands + end - 20;
        const auto word = [&](uint32_t n) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(packet + n * 4)); };
        const uint32_t header = word(0), initiator = word(2);
        if (header >> 30 != 3 || ((header >> 8) & 0x7F) != 0x22 || ((header >> 16) & 0x3FFF) != 3 ||
            ((initiator >> 6) & 3) != 0 || !(initiator >> 16) ||
            ((initiator & 0x3F) != 4 && (initiator & 0x3F) != 8)) continue;
        if (word(3) != lastIndexAddress)
        {
            DumpBuffer(word(3), (initiator >> 16) * ((initiator & 0x800) ? 4 : 2));
            ++g_indexDraws;
        }
        break;
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

// GPU side (Xenia's command processor thread): the register file at every
// draw of the captured frame, i.e. between the GPU's processing of the swap
// that started the capture and the next one. xenia_regs.bin: per draw, u32
// 'FDXR', then registers 0x2000-0x23FF and 0x4900-0x4927 (bool/loop constants).
std::atomic<uint32_t> g_cpuSwaps{0}, g_gpuSwaps{0}, g_gpuCaptureSwap{0};
FILE* g_gpuFile = nullptr;
FILE* g_gpuTextureFile = nullptr;
FILE* g_gpuMipFile = nullptr;
std::vector<uint32_t> g_dumpedGpuTextures;
std::vector<uint32_t> g_dumpedGpuMips;
uint32_t g_gpuDraws = 0;
std::vector<uint8_t> g_gpuSubmitted;
uint64_t g_gpuRegisterHash = 14695981039346656037ull;

#ifdef FD_NATIVE_RENDERER
// Transitional validation on the GPU consuming thread, after full resolve
// readback. This is not a CPU draw-hook snapshot or native presentation.
struct NativeTextureCapture
{
    gpu::native::TextureTables tables;
    struct Resource { uint32_t index; bool mips; std::array<uint32_t,6> lastFetch; };
    std::map<uint32_t,Resource> resources;
    uint32_t mipLevels=0,failures=0;
    std::vector<std::string> errors;
};
std::unique_ptr<NativeTextureCapture> g_nativeTextures;
void RenderCapturedNativeFrame(NativeTextureCapture& capture,const gpu::native::PackedTextures& packed)
{
    const char* enabled=getenv("FD_NATIVE_FRAME_CAPTURE");
    if(!enabled||atoi(enabled)==0)return;
    const auto* batch=gpu::native::audit::CapturedGeometry();
    const uint64_t serial=g_gpuCaptureSwap.load();
    gpu::native::FrameOutput output;
    bool passed=false;std::string error;
    try
    {
        const auto* catalog=gpu::native::audit::CapturedCatalog();
        if(capture.failures||!batch||!catalog)throw std::runtime_error("Missing completed native frame inputs");
        if(batch->serial!=serial)throw std::runtime_error("CPU and GPU native input frame serials differ");
        std::map<uint32_t,uint32_t> resources;
        for(const auto& [base,resource]:capture.resources)
            if(!resources.emplace(base&0x1FFFFFFF,resource.index).second)
                throw std::runtime_error("Ambiguous native frame texture address");
        auto draws=gpu::native::BuildFrameDraws(*batch,*catalog,capture.tables,resources);
        gpu::native::FrameInput frame{draws,packed.textures,packed.mips,packed.cubes,batch->streams.bytes};
        std::filesystem::create_directories("native-live-frame");
        gpu::native::ReplayFrame::Save(frame,"native-live-frame");
        gpu::native::Renderer renderer;
        if(!renderer.LoadShaderCatalog(getenv("FD_NATIVE_SHADER_CATALOG")))throw std::runtime_error("Cannot load the native frame shader catalog");
        if(renderer.RenderFrame(frame,"native-live-frame",&output))throw std::runtime_error("Native live frame rendering failed");
        passed=output.width&&output.height&&output.gameDraws&&!output.debugErrors;
        if(!passed)throw std::runtime_error("Native live frame has empty output or device errors");
    }
    catch(const std::exception& exception){error=exception.what();}
    if(FILE* file=fopen("native-frame-report.json","wb"))
    {
        fprintf(file,"{\n  \"mode\": \"cpu-engine-frame-native-render\",\n"
            "  \"scope\": \"cpu-engine-input-and-gpu-consumer-textures\",\n"
            "  \"native_interactive\": false,\n  \"complete\": true,\n  \"passed\": %s,\n"
            "  \"cpu_frame_serial\": %llu,\n  \"gpu_frame_serial\": %llu,\n"
            "  \"ordered_commands\": %zu,\n  \"vertex_bytes\": %llu,\n  \"stream_bytes\": %zu,\n"
            "  \"game_draws\": %llu,\n  \"debug_errors\": %llu,\n  \"error\": \"%s\"\n}\n",
            passed?"true":"false",batch?batch->serial:0,serial,batch?batch->commands.size():0,
            batch?[&]{uint64_t bytes=0;for(const auto& c:batch->commands)bytes+=c.geometry.vertices.size();return bytes;}():0,
            batch?batch->streams.bytes.size():0,output.gameDraws,output.debugErrors,error.c_str());
        fclose(file);
    }
    kernel::Log("native live frame: CPU/GPU %llu/%llu, %llu draws, %ux%u, %s %s",batch?batch->serial:0,serial,
        output.gameDraws,output.width,output.height,passed?"pass":"FAIL",error.c_str());
}
const gpu::native::TextureMemoryReader g_nativeTextureReader=[](uint32_t address,uint32_t size)->std::span<const uint8_t> {
    if(!size||uint64_t(address)+size>0x20000000)return {};
    return{mem::Ptr<uint8_t>(mem::kPhysicalBase|address),size};
};
void RecordNativeTexture(const uint32_t* fetch)
{
    if(!g_nativeTextures||(fetch[0]&3)!=2||!(fetch[1]>>12))return;
    auto& capture=*g_nativeTextures;
    // Keep the same first-use-per-address scope as gpu_textures.bin/gpu_mips.bin.
    // Immutable versions for every later mutation remain a live-frame task.
    const uint32_t base=fetch[1]&0xFFFFF000;
    try
    {
        auto found=capture.resources.find(base);
        if(found!=capture.resources.end())
        {
            if(found->second.mips||std::equal(found->second.lastFetch.begin(),found->second.lastFetch.end(),fetch))return;
            std::copy(fetch,fetch+6,found->second.lastFetch.begin());
        }
        const std::span<const uint32_t,6> descriptor(fetch,6);
        const auto storage=gpu::native::DescribeTextureStorage(descriptor);
        if(found==capture.resources.end())
        {
            const auto index=capture.tables.Capture(descriptor,g_nativeTextureReader);
            NativeTextureCapture::Resource resource{index,!storage.mips.empty(),{}};
            std::copy(fetch,fetch+6,resource.lastFetch.begin());capture.resources.emplace(base,resource);
            capture.mipLevels+=uint32_t(storage.mips.size());
        }
        else if(!found->second.mips&&!storage.mips.empty())
        {
            const auto snapshot=gpu::native::SnapshotTexture(descriptor,g_nativeTextureReader);
            capture.tables.Mips(found->second.index,descriptor,snapshot.mips,storage.mips);
            found->second.mips=true;capture.mipLevels+=uint32_t(storage.mips.size());
        }
    }
    catch(const std::exception& error)
    {
        ++capture.failures;
        if(capture.errors.size()<8)capture.errors.push_back(error.what());
    }
}
void FinishNativeTextures()
{
    if(!g_nativeTextures)return;
    auto& capture=*g_nativeTextures;
    try
    {
        const auto packed=capture.tables.Pack();
        const auto save=[](const char* path,const std::vector<uint8_t>& bytes) {
            FILE* file=fopen(path,"wb");
            if(!file)return false;
            const bool written=fwrite(bytes.data(),1,bytes.size(),file)==bytes.size();
            const int closed=fclose(file);return written&&!closed;
        };
        const bool textures=save("native-textures.rgba",packed.textures);
        const bool cubes=save("native-cubes.rgba",packed.cubes);
        const bool mips=save("native-mips.rgba",packed.mips);
        if(!textures||!cubes||!mips)++capture.failures;
        RenderCapturedNativeFrame(capture,packed);
    }
    catch(const std::exception& error){++capture.failures;capture.errors.push_back(error.what());}
    if(FILE* file=fopen("native-texture-report.json","wb"))
    {
        fprintf(file,"{\n  \"mode\": \"native-memory-texture-producer\",\n"
            "  \"scope\": \"gpu-consumer-first-use-per-address\",\n  \"native_rendering\": false,\n"
            "  \"complete\": true,\n  \"passed\": %s,\n  \"register_hash\": \"%016llX\",\n"
            "  \"textures\": %zu,\n  \"mip_levels\": %u,\n  \"failures\": %u\n}\n",
            !capture.failures&&!capture.resources.empty()?"true":"false",g_gpuRegisterHash,
            capture.resources.size(),capture.mipLevels,capture.failures);
        fclose(file);
    }
    kernel::Log("native texture producer: %zu resources, %u mip levels, %u failures (GPU consumer diagnostic)",
        capture.resources.size(),capture.mipLevels,capture.failures);
    for(const auto& error:capture.errors)kernel::Log("native texture producer: %s",error.c_str());
    g_nativeTextures.reset();
}
#endif

void HashGpuRegisters(const uint32_t* words, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        g_gpuRegisterHash = (g_gpuRegisterHash ^ words[i]) * 1099511628211ull;
}

void RecordMipTexture(const uint32_t* fetch)
{
#ifdef FD_XENIA_GPU
    if (!g_gpuMipFile || (fetch[0] & 3) != 2 || ((fetch[5] >> 9) & 3) != 1) return;
    const uint32_t base = fetch[1] & 0x1FFFF000;
    if (std::find(g_dumpedGpuMips.begin(), g_dumpedGpuMips.end(), base) != g_dumpedGpuMips.end()) return;
    xe::gpu::xenos::xe_gpu_texture_fetch_t decoded;
    memcpy(&decoded, fetch, sizeof(decoded));
    uint32_t width, height, depth, basePage, mipPage, minMip, maxMip;
    xe::gpu::texture_util::GetSubresourcesFromFetchConstant(decoded, &width, &height, &depth,
        &basePage, &mipPage, &minMip, &maxMip);
    if (!maxMip || !mipPage) return;
    ++width;
    ++height;
    const auto format = static_cast<xe::gpu::xenos::TextureFormat>(fetch[1] & 63);
    const bool packed = (fetch[5] >> 11) & 1;
    const auto layout = xe::gpu::texture_util::GetGuestTextureLayout(
        xe::gpu::xenos::DataDimension::k2DOrStacked, (fetch[0] >> 22) & 511, width, height, 1,
        (fetch[0] >> 31) != 0, format, packed, basePage != 0, maxMip);
    const uint32_t address = (mipPage << 12) & 0x1FFFFFFF;
    const uint32_t length = layout.mips_total_extent_bytes;
    if (!length || length > (16u << 20) || uint64_t(address) + length > 0x20000000) return;
    g_dumpedGpuMips.push_back(base);
    fwrite(&base, 4, 1, g_gpuMipFile);
    fwrite(fetch, 4, 6, g_gpuMipFile);
    fwrite(&maxMip, 4, 1, g_gpuMipFile);
    fwrite(&length, 4, 1, g_gpuMipFile);
    for (uint32_t mip = 1; mip <= maxMip; ++mip)
    {
        const uint32_t stored = std::min(mip, layout.packed_level);
        uint32_t x = 0, y = 0, z = 0;
        if (packed) xe::gpu::texture_util::GetPackedMipOffset(width, height, 1, format, mip, x, y, z);
        const uint32_t info[4] = {layout.mips[stored].row_pitch_bytes, layout.mip_offsets_bytes[stored], x, y};
        fwrite(info, sizeof(info), 1, g_gpuMipFile);
    }
    fwrite(mem::Translate(mem::kPhysicalBase | address), length, 1, g_gpuMipFile);
#endif
}
#ifdef FD_HAVE_RENDERDOC
RENDERDOC_API_1_1_2* g_renderdocCapture = nullptr;
#endif

void OnGpuTextures(uint32_t mask, const uint32_t* registers)
{
    if (!g_gpuTextureFile) return;
    for (uint32_t slot = 0; slot < 32; ++slot)
        if (mask & (1u << slot))
        {
            RecordTexture(registers + 0x4800 + slot * 6, g_gpuTextureFile, g_dumpedGpuTextures);
            RecordMipTexture(registers + 0x4800 + slot * 6);
#ifdef FD_NATIVE_RENDERER
            RecordNativeTexture(registers + 0x4800 + slot * 6);
#endif
        }
}

void OnGpuEvent(int event, const uint32_t* registers)
{
    if (event == 2)
    {
        const uint32_t swap = ++g_gpuSwaps, start = g_gpuCaptureSwap.load();
        if (start && swap == start)
        {
            g_gpuFile = fopen("xenia_regs.bin", "wb");
            g_gpuTextureFile = fopen("gpu_textures.bin", "wb");
            g_gpuMipFile = fopen("gpu_mips.bin", "wb");
            if (g_gpuMipFile) { const uint64_t placeholder[2] = {}; fwrite(placeholder, sizeof(placeholder), 1, g_gpuMipFile); }
            g_dumpedGpuTextures.clear();
            g_dumpedGpuMips.clear();
            g_gpuDraws = 0;
            g_gpuSubmitted.clear();
            g_gpuRegisterHash = 14695981039346656037ull;
#ifdef FD_NATIVE_RENDERER
            const char* nativeTextures=getenv("FD_NATIVE_TEXTURE_CAPTURE");
            if(nativeTextures&&atoi(nativeTextures)!=0)g_nativeTextures=std::make_unique<NativeTextureCapture>();
#endif
#ifdef FD_HAVE_RENDERDOC
            // Launched under RenderDoc (renderdoccmd capture): capture Xenia's
            // D3D12 work for this same frame, which starts after this swap's present.
            if (HMODULE rdoc = GetModuleHandleA("renderdoc.dll"))
            {
                auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(rdoc, "RENDERDOC_GetAPI"));
                RENDERDOC_API_1_1_2* api = nullptr;
                if (getApi && getApi(eRENDERDOC_API_Version_1_1_2, reinterpret_cast<void**>(&api)) == 1 && api)
                {
                    api->StartFrameCapture(nullptr, nullptr);
                    g_renderdocCapture = api;
                    kernel::Log("frame capture: RenderDoc capture started");
                }
            }
#endif
        }
        else if (start && swap == start + 1 && g_gpuFile)
        {
            fclose(g_gpuFile);
            g_gpuFile = nullptr;
            if (g_gpuMipFile)
            {
                const uint32_t header[2] = {0x31504D46, uint32_t(g_dumpedGpuMips.size())}; // FMP1
                fseek(g_gpuMipFile, 0, SEEK_SET);
                fwrite(header, sizeof(header), 1, g_gpuMipFile);
                fwrite(&g_gpuRegisterHash, 8, 1, g_gpuMipFile);
                fclose(g_gpuMipFile);
                g_gpuMipFile = nullptr;
            }
            // Match these submission flags to the exact register stream, not
            // merely another frame with the same number of guest draw calls.
            if (FILE* submitted = fopen("gpu_draws.bin", "wb"))
            {
                const uint32_t header[2] = {0x45474446, uint32_t(g_gpuSubmitted.size())}; // FDGE
                fwrite(header, sizeof(header), 1, submitted);
                fwrite(&g_gpuRegisterHash, sizeof(g_gpuRegisterHash), 1, submitted);
                fwrite(g_gpuSubmitted.data(), 1, g_gpuSubmitted.size(), submitted);
                fclose(submitted);
            }
            if (g_gpuTextureFile) fclose(g_gpuTextureFile);
            g_gpuTextureFile = nullptr;
#ifdef FD_NATIVE_RENDERER
            FinishNativeTextures();
#endif
#ifdef FD_HAVE_RENDERDOC
            if (g_renderdocCapture)
            {
                const uint32_t saved = g_renderdocCapture->EndFrameCapture(nullptr, nullptr);
                kernel::Log("frame capture: RenderDoc capture ended (%u)", saved);
                g_renderdocCapture = nullptr;
            }
#endif
            kernel::Log("frame capture: %u GPU draws written to xenia_regs.bin", g_gpuDraws);
            // Full resolve readback has completed before this GPU swap. Save
            // the captured frame's final colour copy, rather than a screenshot
            // selected by a separate wall-clock timer.
            std::lock_guard lock(g_mutex);
            if ((g_referenceFetch[0] & 3) == 2 && (g_referenceFetch[1] & 0x3F) == 6)
            {
                const uint32_t width = (g_referenceFetch[2] & 0x1FFF) + 1;
                const uint32_t height = ((g_referenceFetch[2] >> 13) & 0x1FFF) + 1;
                const uint32_t pitch = std::max(width, ((g_referenceFetch[0] >> 22) & 0x1FF) << 5);
                const uint32_t length = ((pitch + 31) & ~31u) * ((height + 31) & ~31u) * 4;
                const uint32_t physical = g_referenceFetch[1] & 0x1FFFF000;
                if (physical + uint64_t(length) <= 0x20000000)
                    if (FILE* file = fopen("frame_reference.bin", "wb"))
                    {
                        const uint32_t magic = 0x46445458;
                        fwrite(&magic, 4, 1, file);
                        fwrite(g_referenceFetch, sizeof(g_referenceFetch), 1, file);
                        fwrite(&length, 4, 1, file);
                        fwrite(mem::Translate(mem::kPhysicalBase | physical), length, 1, file);
                        fclose(file);
                        kernel::Log("frame capture: synchronized reference %ux%u written", width, height);
                    }
            }
        }
        return;
    }
    if (!g_gpuFile) return;
    if (event == 1)
    {
        if (!g_gpuSubmitted.empty()) g_gpuSubmitted.back() = 1;
        return;
    }
    if (event != 0) return;
    // Capture on the consuming thread, after preceding full resolve/memexport
    // readbacks. The game thread can otherwise snapshot an older memory version.
    const uint32_t magic = 0x32584446; // FDX2: includes the actual GPU float constant banks
    HashGpuRegisters(&magic, 1);
    HashGpuRegisters(registers + 0x2000, 0x400);
    HashGpuRegisters(registers + 0x4900, 0x28);
    HashGpuRegisters(registers + 0x4000, 0x800);
    fwrite(&magic, 4, 1, g_gpuFile);
    fwrite(registers + 0x2000, 4, 0x400, g_gpuFile);
    fwrite(registers + 0x4900, 4, 0x28, g_gpuFile);
    fwrite(registers + 0x4000, 4, 0x800, g_gpuFile);
    ++g_gpuDraws;
    g_gpuSubmitted.push_back(0);
}

// Frame boundary: arm at the first swap after the delay, capture until the next.
void OnSwap(PPCContext& ctx)
{
    if (!Enabled()) return;
    std::lock_guard lock(g_mutex);
    fd_gpu_event_hook = OnGpuEvent; // before the GPU processes this swap
    fd_gpu_texture_hook = OnGpuTextures;
    ++g_cpuSwaps;
    const State state = g_state.load();
    // Optional scene gate: a menu or transition may still be visible after the
    // wall-clock delay under a debugger. Wait for the requested scene complexity.
    const uint32_t indexed = g_frameIndexed;
    g_frameIndexed = 0;
    if (state == State::Waiting && GetTickCount64() >= g_armAt && indexed >= MinimumIndexedDraws())
    {
        g_file = fopen("frame_capture.bin", "wb");
        g_state = g_file ? State::Capturing : State::Done;
        // The GPU processes this swap later: its frame starts there.
        if (g_file) {
            g_gpuCaptureSwap = g_cpuSwaps.load();
            gpu::native::audit::ArmFrame(g_cpuSwaps.load());
        }
    }
    else if (state == State::Capturing)
    {
        FlushPendingVertices();
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
    gpu::native::audit::Draw(ctx.r3.u32, gpu::native::audit::DrawKind::Indexed,ctx.r5.u32,ctx.r4.u32,ctx.r7.u32);
    if (MinimumIndexedDraws() && g_state.load() == State::Waiting) ++g_frameIndexed;
    const uint32_t device = ctx.r3.u32, cursor = mem::Read32(device + 0x30);
    Record(0, ctx);
    __imp__sub_8250DDA0(ctx, base);
    gpu::native::audit::DrawEnd(device, gpu::native::audit::DrawKind::Indexed);
    RecordCommands(device, cursor);
}

// DrawVertices with rect lists (full-screen passes).
PPC_FUNC_IMPL(__imp__sub_8250D988);
PPC_FUNC(sub_8250D988)
{
    gpu::native::audit::Draw(ctx.r3.u32, gpu::native::audit::DrawKind::Vertices,ctx.r5.u32,ctx.r4.u32,ctx.r6.u32);
    const uint32_t device = ctx.r3.u32, cursor = mem::Read32(device + 0x30);
    Record(1, ctx);
    __imp__sub_8250D988(ctx, base);
    gpu::native::audit::DrawEnd(device, gpu::native::audit::DrawKind::Vertices);
    RecordCommands(device, cursor);
}

PPC_FUNC_IMPL(__imp__sub_8250D480);
PPC_FUNC(sub_8250D480)
{
    gpu::native::audit::Draw(ctx.r3.u32, gpu::native::audit::DrawKind::BeginVertices,0,ctx.r4.u32,ctx.r5.u32);
    const uint32_t device = ctx.r3.u32, cursor = mem::Read32(device + 0x30);
    Record(2, ctx);
    __imp__sub_8250D480(ctx, base);
    gpu::native::audit::DrawEnd(device, gpu::native::audit::DrawKind::BeginVertices);
    RecordCommands(device, cursor);
    if (g_state.load() == State::Capturing)
    {
        std::lock_guard lock(g_mutex);
        g_upDevice = device;
        g_upBefore = cursor;
    }
}

// Swap (calls VdSwap).
PPC_FUNC_IMPL(__imp__sub_820E0AC8);
PPC_FUNC(sub_820E0AC8)
{
    gpu::native::audit::Swap();
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
    gpu::native::audit::ShaderInit(0, ctx.r3.u32, ctx.r4.u32);
    RecordShaderInit(0, ctx);
    __imp__sub_820DB120(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_820DA850);
PPC_FUNC(sub_820DA850)
{
    gpu::native::audit::ShaderInit(1, ctx.r3.u32, ctx.r4.u32);
    RecordShaderInit(1, ctx);
    __imp__sub_820DA850(ctx, base);
}

// D3DDevice_Resolve (820EE9D0, ~50 per frame; writes RB_COPY_* in 820ED880;
// 820EFD40 is D3DDevice_Clear). r6 is the destination texture object.
PPC_FUNC_IMPL(__imp__sub_820EE9D0);
PPC_FUNC(sub_820EE9D0)
{
    gpu::native::audit::BeforeOtherCall();
    const uint32_t device = ctx.r3.u32, cursor = mem::Read32(device + 0x30);
    const uint32_t flags = ctx.r4.u32, texture = ctx.r6.u32;
    gpu::native::audit::ResolveBegin(device,flags,texture);
    Record(4, ctx);
    __imp__sub_820EE9D0(ctx, base);
    gpu::native::audit::ResolveEnd(device);
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
            if (texture >= 0x40000000 && texture < 0xC0000000 && (flags & 7) < 4)
                for (uint32_t i = 0; i < 6; ++i)
                    g_referenceFetch[i] = mem::Read32(texture + 0x1C + i * 4);
        }
    }
}

// D3DDevice_Clear: device, rectangle count/pointer, flags, ARGB colour,
// floating depth in f1, and stencil in r9. Preserve these before D3D
// temporarily replaces the shaders and GPU state to implement the clear.
PPC_FUNC_IMPL(__imp__sub_820EFD40);
PPC_FUNC(sub_820EFD40)
{
    gpu::native::audit::BeforeOtherCall();
    const uint32_t device = ctx.r3.u32, cursor = mem::Read32(device + 0x30);
    const float depth = float(ctx.f1.f64);
    gpu::native::audit::Clear(device,ctx.r6.u32,ctx.r7.u32,depth,ctx.r9.u32,ctx.r4.u32,ctx.r5.u32);
    std::vector<uint8_t> rectangles;
    if (g_state.load() == State::Capturing && ctx.r5.u32 && ctx.r4.u32 <= 256)
    {
        const auto* rects = static_cast<const uint8_t*>(mem::Translate(ctx.r5.u32));
        rectangles.assign(rects, rects + size_t(ctx.r4.u32) * 16);
    }
    Record(5, ctx);
    __imp__sub_820EFD40(ctx, base);
    RecordCommands(device, cursor);
    if (g_state.load() == State::Capturing)
    {
        std::lock_guard lock(g_mutex);
        if (g_file)
        {
            const uint32_t head[3] = {0x4C434446, std::bit_cast<uint32_t>(depth), uint32_t(rectangles.size() / 16)};
            fwrite(head, sizeof(head), 1, g_file);
            if (!rectangles.empty()) fwrite(rectangles.data(), rectangles.size(), 1, g_file);
        }
    }
}
