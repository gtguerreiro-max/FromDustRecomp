#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include <memory>
#include "texture_decode.h"
#include "plume_render_interface.h"

namespace gpu::native
{
using plume::RenderRect;
// Order of the per-draw registers written by replay_pack.py (STATE_REGISTERS).
enum Reg
{
    RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO, PA_SC_WINDOW_SCISSOR_TL, PA_SC_WINDOW_SCISSOR_BR,
    RB_COLOR_MASK, RB_DEPTHCONTROL, RB_BLENDCONTROL0, PA_SU_SC_MODE_CNTL, VGT_INDX_OFFSET,
    VPORT_XSCALE, VPORT_XOFFSET, VPORT_YSCALE, VPORT_YOFFSET, VPORT_ZSCALE, VPORT_ZOFFSET, PA_CL_VTE_CNTL,
    RB_COLOR1_INFO, RB_COLOR2_INFO, RB_COLOR3_INFO, RB_BLENDCONTROL1, RB_BLENDCONTROL2, RB_BLENDCONTROL3,
    PA_SU_VTX_CNTL, RB_STENCILREFMASK, RB_STENCILREFMASK_BF,
    RB_BLEND_RED, RB_BLEND_GREEN, RB_BLEND_BLUE, RB_BLEND_ALPHA,
    kRegCount
};

struct Draw
{
    std::string vs;
    uint32_t primitive = 0, indexCount = 0, index32 = 0, stride = 0;
    const uint8_t* indices = nullptr;
    uint32_t indexBytes = 0;
    const uint8_t* vertices = nullptr;
    uint32_t vertexBytes = 0;
    std::vector<std::string> semantics;
    const uint8_t* constants = nullptr; // 4096 bytes
    uint32_t regs[kRegCount] = {};
    int32_t slots[32] = {}; // texture table index per fetch slot, -1 if none
    std::string ps;                       // empty: depth only
    const uint8_t* psConstants = nullptr; // 4096 bytes
    uint32_t bases[32] = {};              // texture base address per fetch slot
    uint32_t swizzles[32] = {};           // fetch swizzle per slot
    uint32_t boolLoop[40] = {};           // registers 0x4900-0x4927
    const uint8_t* vertexFetch = nullptr; // 32 uint4 g_VertexFetch descriptors (pulled vertex fetches)
    int32_t expAdjust[32] = {};           // texture fetch constant exp_adjust per slot
    uint32_t fetchFormats[32] = {};       // Xenos texture format, needed for depth reinterpretation
    bool hasSamplerStates = false;
    uint32_t samplerStates[32] = {};      // clamp XYZ, mag/min/mip, anisotropy
    bool hasFetchMetadata = false;
    uint32_t textureFetches[32][6] = {};
    bool hasContext = false;
    uint32_t context[1024] = {};
    bool hasPixelCenter = false;
    bool hasBlendConstants = false;
    bool clear = false;
    uint32_t clearFlags = 0, clearColor = 0, clearStencil = 0;
    float clearDepth = 0;
    std::vector<RenderRect> clearRects;
    // "#resolve" records: EDRAM -> texture copy.
    bool resolve = false;
    uint32_t copyDest = 0, copyControl = 0, copySurface = 0, copyColor = 0, copyDepth = 0, copyDestInfo = 0;
    uint32_t copyWidth = 0, copyHeight = 0; // destination texture size
    uint32_t copyX = 0, copyY = 0;
    uint32_t copyMip = 0, copyMipCount = 1;
    bool hasStencilState = false;
    // CPU writes supersede an earlier GPU resolve until the next copy.
    uint32_t cpuTextureMask=0;
};


// Borrowed immutable data. RenderFrame finishes GPU execution before returning;
// producers must keep every referenced byte alive and unchanged for that call.
struct FrameInput
{
    std::span<const Draw> draws;
    std::span<const uint8_t> textures, mips, cubes, streams;
    std::span<const uint8_t> debugPS, depthVS, depthPS, depthFloatPS;
    bool continuous=false;
    uint32_t frontBufferBase=0;
    std::span<const uint64_t> textureVersions;
    std::span<const std::shared_ptr<TextureResource>> resources;
    std::span<const uint8_t> volumes;
};
struct FrameOutput
{
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgb;
    uint64_t gameDraws = 0, debugErrors = 0, debugWarnings = 0;
};
}
