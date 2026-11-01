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

## Engine backend

`tools/callgraph.py 8221A000 8221E000` (rates from func_counts.txt): 8221C270
(505/frame) draws meshes through DrawIndexedVertices; 8221C170 / 8221C0F8 /
8221C000 draw rect lists; 8221D630 presents (Swap). 8221B6E0 (299/frame),
8221CB48 (109) and 8221C7D0 (35) set per-index and render states. Texture and
shader setters are not calls: Xbox D3D inlines them as writes to the device.

## Frame capture: the device layout

`FD_FRAME_CAPTURE=<s>` (runtime/gpu/frame_capture.cpp) hooks the draws and Swap
and writes one frame to `build/frame_capture.bin`: each draw's arguments, a
32 KB snapshot of the device and the objects it points to.
`tools/analyze_capture.py` lists the device dwords that change between draws.
One gameplay frame: 634 draws (590 indexed, 44 rect lists).

| Device offset | Content | Evidence |
|---|---|---|
| +0x0000 | dirty mask (64-bit at +0x08) | ORed by setters |
| +0x0030 | command buffer cursor | new value at every draw |
| +0x0780 | vertex shader constants c0.. | SetVertexShaderConstantF target |
| +0x1780 | pixel shader constants c0.. | SetPixelShaderConstantF target |
| +0x2F58 | index buffer object | equals the draw's r8 |
| +0x318C..+0x31A4 | render target / depth surfaces | SetRenderTarget offsets 0x3194/0x3198 |
| +0x31F8..+0x3234 | 16 texture objects (samplers) | type-3 objects with fetch constants |
| +0x328C | pixel shader object | type 7, microcode at object+0x18, container header 0x102A1100 at +0x28 |
| +0x3290 | vertex shader object | type 6, microcode at object+0x20 (IM_LOAD loads it from +0x40: header skipped) |

Resources are standard Xbox D3D objects (type at +0: 3 texture, 4 surface,
5 index buffer, 6 vertex shader, 7 pixel shader). The shader roles are
confirmed by the IM_LOAD packets each draw writes (type bits: 0 vertex, 1 pixel); a texture's Xenos fetch
constant (format, physical address, size) is at object+0x1C.

Every draw also records the command bytes it wrote (`tools/pm4_decode.py`;
the cursor at device+0x30 points at the last written word). Shader identity
comes from those packets and the microcode:
- Pixel shaders: the microcode (size from IM_LOAD) is found byte for byte in
  one extracted container.
- Vertex shaders: the D3D patches the vertex fetch instructions in memory, so
  they are matched by the most identical dwords (typically all but 2, e.g.
  58/60). VS/PS pairs come out as consecutive containers (141/142, 145/146,
  149/150...), as the game stores them.
- One group of 68 draws (pixel shader 140) has no good vertex match yet
  (11/39 dwords).
A live renderer should map objects to containers when the shaders are
created instead (hook the D3D create functions, as UnleashedRecomp does).

### Fetch constants and the frame inventory

Device +0x0480..+0x0780 is a copy of the fetch constant registers
0x4800-0x48BF: slots 0-15 are the 16 textures (type 2), and vertex fetch 95
(registers 0x48BE/0x48BF, device +0x0778) is the vertex buffer of stream 0.
`tools/xenos_fetch.py` decodes texture and vertex fetch constants (written by
the local Qwen model from Xenia's xenos.h; dwords 3-5 corrected by hand, as
its excerpt was cut short). `tools/frame_inventory.py` lists every captured
draw: primitive, count, shaders, decoded textures and vertex buffer.

The captured gameplay frame starts with deferred lighting passes (rect lists
reading the G-buffer and the 1024x600 24_8_FLOAT depth as textures), then
draws meshes (the terrain: 5451 indices).

Findings:
- The 380 mesh draws with a null +0x328C have no *pixel* shader.
  Their commands load only a vertex shader (one IM_LOAD): depth-only passes
  (shadow maps into 512x512 targets, depth prepass).
- 145/146/147 are the terrain shaders (heightfield vertex fetch), used for
  the depth pass and the colour pass.

## Helper modules (written by the local Qwen model, validated)

| Module | Content | Validation |
|---|---|---|
| tools/xenos_fetch.py | texture / vertex fetch constants | dwords 0-2 checked, 3-5 corrected by hand |
| tools/xenos_vfetch.py | vertex fetch instructions, vertex layouts | every bit position matches ucode.h |
| tools/xenos_state.py | render state registers → D3D12 summary | sample decode checked against registers.h |
| tools/xenos_tiling.py | tiled texture addressing, untile_2d | 22058 coordinates identical to Xenia's C++ |

## Shader creation

Qwen identified the init functions: 820DB120 initialises a vertex shader
object (r3 object, r4 microcode, stored at +0x20) and 820DA850 a pixel shader
(stored at +0x18). frame_capture.cpp hooks both and writes each object and its
unpatched microcode to build/shader_inits.bin. The game creates 557 shaders at
startup (all of fxlist_360.fxl); 507 match one extracted container exactly by
microcode, 50 by their longest identical prefix (32-256 bytes).

## Next steps

0. Find the D3D create functions (CreateVertexShader / CreatePixelShader /
   CreateTexture) to map objects to containers exactly.
1. Prototype: at each captured draw, resolve shaders (container hash →
   XenosRecomp output), textures (fetch constants) and buffers, and replay one
   frame offline with plume; compare with the Xenia image.
2. Decode the vertex layouts (vertex fetch instructions in each VS) and the
   render state registers the draws write (0x2000-0x2380).
3. Then hook the engine draws live and draw natively instead of the PM4 path.
