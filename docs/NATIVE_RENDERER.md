# Native renderer: research notes

Goal: replace the Xenia GPU translation (PM4 command stream → D3D12) with a
renderer that draws the game's D3D calls directly, as UnleashedRecomp does.
This would expose real draw calls, render targets and matrices, which DLSS
(motion vectors, jitter) and new lighting need.

## Prior art

- UnleashedRecomp (`UnleashedRecomp/gpu/video.cpp`, GPL-3.0): hooks 42 functions
  of the statically linked Xbox D3D library by guest address (CreateDevice,
  CreateTexture, Lock/Unlock*, SetRenderTarget, SetTexture, SetVertexShader,
  SetPixelShader, SetStreamSource, DrawPrimitive, DrawIndexedPrimitive,
  DrawPrimitiveUP, Clear, StretchRect...). SetRenderState goes through a table of
  16 states. Rendering is queued to a worker thread on the plume RHI
  (D3D12/Vulkan, MIT).
- XenosRecomp (MIT): Xbox 360 shader containers → HLSL → DXIL/SPIR-V.
- COGBox (Gears of War, MIT) and fear360-recomp: own D3D12/Vulkan renderers;
  COGBox ships DLSS/FSR/XeSS. Neither documents its motion vector source.

## Shaders: done (prototype level)

`tools/shader_survey.py` splits the shader containers out of
`data/Cache/fxlist_360.fxl` and `Default.xex` and converts each one with
XenosRecomp and DXC. All 554 unique shaders (368 pixel, 186 vertex) compile to
DXIL after these XenosRecomp changes (`patches/xenosrecomp.patch`):

- 1D textures are fetched as 2D textures of height 1.
- Vertex texture fetch: constant s16+n is vertex sampler n (terrain heightfield).
- Vertex shaders sample with LOD 0 (`XE_SAMPLE` macro, `XENOS_VERTEX_SHADER`).
- Runtime loop constants (`i0`... set by SetShaderConstantI, used for the omni
  and spot light loops) are read from shared constants at c18.
- Inputs sharing one semantic are declared once (fix proposed by local Qwen).

The scan must step by 2 bytes: containers in the .fxl are not 4-byte aligned.
Compiling is not correctness: no shader has been checked against real output.

## Findings about the game

- Deferred renderer (`PS_DustDeferredWater`...): a G-buffer with depth and
  normals exists every frame, usable for SSAO/SSGI even before a native renderer.
- Terrain is displaced in the vertex shader from `g_Sampler_Heightfield`.
  Terrain motion vectors could come from the previous frame's heightfield.
- The Xbox D3D library sits around 0x820D0000-0x820F8000. The game calls 237
  distinct functions there (2387 call sites); some may be CRT helpers.
- `tools/pm4_scan.py` finds 41 functions that build PM4 packets with constant
  headers. They are internal paths (clears, resolves, swap at 820E0AC8). The
  game's draws are predicated packets in engine code (see below).

## Runtime call counts

`.\build_all.ps1 -SkipRecomp -FuncCounters` builds DefaultPPC with a counter in
every function prologue (`PPC_FUNC_PROLOGUE`, `runtime/kernel/func_counters.cpp`).
`FD_FUNC_COUNTS=<s>` writes calls per second for all Default.xex functions to
`build/func_counts.txt`; `FD_FUNC_ARGS=<addr>,...` logs lr and r3..r8 of the
first 64 calls (`build/func_args.txt`). Rebuild without the switch afterwards.

Gameplay, 30 fps. The D3D device is at 0x400C4900 (r3 of most calls).
Tentative identifications from counts and arguments:

| Function | Per frame | Arguments seen | Hypothesis |
|---|---|---|---|
| 820D6848 | 1 | min/max addresses (frame buffer) | atomic {min, max} range update (not Swap; VdSwap is called from 820E0AC8) |
| 820D4528 → 820D3F00 | 265 | index 0-3, surface or 0 | SetRenderTarget (MRT); 820D4528 only forwards |
| 820D0E88 | 200 | rect 0, 0, 0x400, 0x258 | **window/scissor helper** (confirmed: type-0 packet 0x00022080 = registers 0x2080-0x2082, clipped per render target; analysis by local Qwen, checked) |
| 820DA538 | 70 | start, pointer, count | **SetVertexShaderConstantF** (confirmed: copies count float4 to device + (start+120)*16) |
| 820DA610 | 78 | start, pointer, count | **SetPixelShaderConstantF** (confirmed: device + (start+376)*16) |
| 820D63E0, 820D2BC0, 820D2A18 | 97 / 78 | index 0-3, 0x80000000 >> index | per-render-target state |
| 820EA830 | 608 | Xenos register 0x2000-0x2200 | context register block write (internal) |
| 820EABC8 | 618 | register 0x4000 / 0x4400 | constant upload (internal) |
| 820EB9F0 | 72 | packed fetch constant, register 0x47F0 | texture fetch constant (SetTexture path) |

The device object shadows GPU state: shader constants live inside it, and the
setters OR a 64-bit dirty mask at device+8. A native renderer can read the
complete state from the device at draw time, as the GPU commands are built.

The device also owns the command buffer: write cursor at device+48, end at
device+56, and 820D7D88 makes room when it is full. Render target rectangles
are at device+13004 (count at device+12996).

The draw functions sit outside the library range and were found by widening
the counters to all of Default.xex (Xenia: ~848 draws per frame) and by
scanning for predicated type-3 headers (`0xC0032201` = DRAW_INDX, 4 words;
bit 0 of the header is the predicate flag, which the first scan ignored):

| Function | Per frame | Arguments seen | Identification |
|---|---|---|---|
| **8250DDA0** | 598 | type 4 (triangle list), 0, 0, count 0x154B, index buffer object | **DrawIndexedVertices** (scene meshes; loops, can emit several DRAW_INDX) |
| **8250D988** | 42 | type 8 (rect list), 0, 3 vertices | **DrawVertices** (full-screen passes: deferred lighting, post) |
| 8250D480 | 3 | type 8, 3, 16, 0x100 / 0x258 | DrawVertices variant |
| 820EEAD8 | 20 | type 1, pointers (called inside D3D) | internal draw (clears / resolves) |
| **820E0AC8** | 1 | surface 400C0780, 0x40, 0x258 | **Swap** (calls VdSwap) |

All draws come from one small layer of the LyN engine's graphics backend:
callers at 8221C354 (indexed draws), 8221C25C and 8221C080 (rect lists) and
8221D708 (swap), around 0x8221B000-0x8221D800. 82499890 (752 per frame) is a
higher-level engine render-item function, not a draw.

## Next steps

1. Map the engine backend around 0x8221B000-0x8221D800 (its SetTexture,
   SetStreamSource, SetShader wrappers and the draw wrappers above): hooking
   at this level gives whole draws with their state.
2. Hook those functions and record one frame's calls, without drawing.
3. Draw that frame with plume and the converted shaders; compare with Xenia.
