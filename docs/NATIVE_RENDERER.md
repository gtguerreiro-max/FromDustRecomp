# Native renderer: research notes

> Latest continuation (2026-11-04): interactive native execution now runs without
> Xenia's GPU command processor. The regular launcher still selects Xenia while
> fidelity / stability validation continues. Physical A/C/E aliases fixed query
> readbacks and restored missing mesh colour. The first tutorial then exposed a
> real BC3 volume, now supported by native base-level 3D decoding / upload and an
> optional `volumes.rgba` replay sidecar. GPU tests sample every slice, test
> swizzle / persistent reuse and reload the saved sidecar. Four earlier frames /
> 3,160 draws remain exact RGB. See `NATIVE_LIVE_PROGRESS.md` for live results and
> remaining limitations; older milestones below describe their original state.

> Current priority (2026-11-03): finish the rendering base correctly. DLSS,
> DLAA, neural rendering and other visual enhancements are deferred. The notes
> about upscalers below are research, not implemented features. Latest verified
> gameplay replays compare at 39.41, 36.03 and 40.24 dB. Four captures, including
> the title and the elevated Breath tutorial, align all 3,160 submitted draws
> and pass all 163 checked colour checkpoints. The game
> still uses Xenia's GPU backend. See the current validation entry at the end.
>
> Debug-layer audit: earlier empty D3D12 logs did not prove that the debug layer
> was active. Activation is now checked strictly. Windows Graphics Tools was
> installed with the user's UAC approval on 2026-11-03; no restart was needed.
> Production still uses Agility SDK 614. See the actual debug results below.

> Shortcut found (2026-11-02): 96dot/Dragon-Age-Origins-Recomp (MIT, Xenia
> BSD) adds DLSS (Streamline), FSR 4 / 3.1, XeSS and frame generation on top of
> Xenia's D3D12 renderer, the path FromDust already uses. Its code is
> `xenia/src/xenia/gpu/d3d12/dao_upscale*.cc/.h` (6668 lines) plus hooks in
> d3d12_command_processor, d3d12_render_target_cache and pipeline_cache. Motion
> vectors: each opaque draw's vertex shader is re-run with stream output and
> matched to the previous frame's draw (hash of VS ucode, PS, vertex fetch
> address, vertex count; closest float constants), drawn into R16G16_FLOAT and
> depth-tested. Jitter: viewport ndc_offset before system constants. Their
> Xenia is close to ours (~960 differing lines in those five files, ignoring
> line endings), so porting it is the fastest route to DLSS. The native
> renderer below stays the long-term route for new lighting.

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
Compilation alone does not establish correctness. Saved-frame output is now
checked against exact RenderDoc events; see the latest base validation below.

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

## Status of the offline replay (fd_replay)

- Captures a frame, packs it into replay.bin, and draws it in offscreen plume/D3D12 with PNG readback.
- Draws indexed, rectangle-list, auto-indexed, and depth-only passes with per-pass render targets.
- Uses viewports, reverse-Z depth, culling, clears, blending, and colour write masks.
- Supports up to 4 colour targets per configuration, with per-RT formats and resolved reads.
- Binds real pixel shaders, pixel/vertex constants, boolean and loop constants.
- Terrain and 13 vertex shaders draw; exact vertex layouts are used where vfetch data matches.
- Textures are decoded, uploaded, and bound with swizzled views, including table textures and fixed BC2/BC3.
- Colour and depth resolves (depth through an R32_FLOAT view) are aliased to host render targets.
- Captured resolves are aliased to host render targets and bound for later reads.
- The output is compared to the Xenia screenshot; PSNR improved from 6 to 10 dB.

Still missing:
- Colour LUTs that are not yet represented correctly.
- Skinned meshes (the tribe): bone matrices POSITION13-15 are fetched with an index computed in the shader (vertex pulling needed).
- The remaining skipped draws.
- The live in-game renderer and DLSS.

Next steps:
1. Skinned meshes: vertex pulling for vfetch with computed indices (XenosRecomp).
2. Sky and fog: the scene target is black where Xenia shows blue fog.
3. Colour LUTs.
4. Run the live renderer in the game.
5. DLSS.

(Section drafted by the local Qwen model from the commit log.)

## Helper modules (written by the local Qwen model, validated)

| Module | Content | Validation |
|---|---|---|
| tools/xenos_fetch.py | texture / vertex fetch constants | dwords 0-2 checked, 3-5 corrected by hand |
| tools/xenos_vfetch.py | vertex fetch instructions, vertex layouts | every bit position matches ucode.h |
| tools/xenos_state.py | render state registers → D3D12 summary | sample decode checked against registers.h |
| tools/xenos_tiling.py | tiled texture addressing, untile_2d | 22058 coordinates identical to Xenia's C++ |
| tools/xenos_texture.py | texture data → RGBA / PNG (DXT1/3/5, 8888, 16_16, 8...) | 41 of 58 textures of a captured frame decode; the level's terrain texture (DXT1 1024²) is correct. Missing: 24_8_FLOAT, 16_16_16_16, 8_8; DXT4_5 fails |

## Shader creation

Qwen identified the init functions: 820DB120 initialises a vertex shader
object (r3 object, r4 microcode, stored at +0x20) and 820DA850 a pixel shader
(stored at +0x18). frame_capture.cpp hooks both and writes each object and its
unpatched microcode to build/shader_inits.bin. The game creates 557 shaders at
startup (all of fxlist_360.fxl); 507 match one extracted container exactly by
microcode, 50 by their longest identical prefix (32-256 bytes).

## Decision: own renderer on plume (2026-11-02)

Goal: better than the Xenia-based ports: no GPU emulation, DLSS through
Streamline on D3D12, new lighting, and Vulkan for Steam Deck/Linux/Android.
- RHI: plume (MIT; D3D12, Vulkan, Metal). Builds with our clang-cl toolchain;
  its triangle example runs on D3D12 (`third_party/plume`, see patches/README).
- Shaders: XenosRecomp DXIL/SPIR-V (all 554 compile).
- Capture of state: hook the engine's D3D draws and read the device (layout
  above), as NativeXRenderer (MIT, ReXGlue/DiligentCore/Vulkan) also does; it
  is a design reference. UnleashedRecomp (GPL) is reading reference only.
- EDRAM: not emulated. Render targets are host textures; resolves copy or
  alias them.

Milestones:
1. Capture vertex/index buffer contents with the frame (offline data).
2. `fd_replay`: load a capture, draw it with plume and the DXIL shaders, write
   a PNG; compare with the Xenia screenshot of the same frame.
3. Live: hook the draws in the game and draw natively instead of the PM4 path.
4. Then DLSS/DLAA (motion vectors from previous-frame constants per draw),
   SSAO/SSGI from the G-buffer, Vulkan build for Steam Deck.

## Next steps

0. Find the D3D create functions (CreateVertexShader / CreatePixelShader /
   CreateTexture) to map objects to containers exactly.
1. Prototype: at each captured draw, resolve shaders (container hash →
   XenosRecomp output), textures (fetch constants) and buffers, and replay one
   frame offline with plume; compare with the Xenia image.
2. Decode the vertex layouts (vertex fetch instructions in each VS) and the
   render state registers the draws write (0x2000-0x2380).
3. Then hook the engine draws live and draw natively instead of the PM4 path.

## Status 2026-11-02 (handoff)

Workflow: capture a frame, replay it natively, compare every resolve with Xenia's.

- Capture: `FD_FRAME_CAPTURE=<s>`. Decode the synchronized GPU reference with
  `python tools/frame_reference.py`; a separately timed screenshot may capture a transition.
  Title screen: no `FD_AUTOPRESS`, 60 s. In full-readback diagnostic mode, confirmed gameplay:
  Start at 140 s, then A every four seconds from 145 to 260, capture at 280 s (run 296 s).
  Writes build/frame_capture.bin, textures.bin, buffers.bin, shader_inits.bin and
  xenia_regs.bin (Xenia's register file at every GPU draw, via the FROMDUST_GPU_SHIM hook), and
  frame_reference.bin (the captured frame's final resolve after GPU readback).
- `python tools/replay_pack.py` then `build\fd_replay\fd_replay.exe build\replay`.
  Optional `--capture-dir <dir> --out <dir>` keeps multiple captures reproducible.
- `python tools/compare_resolves.py [--save]`: our copies (resolve_NN_ADDR.png/.raw) vs Xenia's
  (readback_resolve=full puts them in textures.bin). Note: textures.bin holds one version per
  address (when it was read), so for addresses copied several times only the last copy compares.
- `tools/rank_targets.py`, `tools/target_gallery.py`, `tools/img_compare.py`, `tools/color_fit.py`.
- Debug switches in fd_replay: FD_TRACE_PS, FD_TRACE_DRAWS, FD_STOP_AFTER, FD_ZERO_SLOTS,
  FD_LIVE_DEST, FD_NO_SNAPSHOT, FD_PENDING_LIVE, FD_NO_COLOR_BIAS, FD_REPLAY_DEBUG_PS.
- RenderDoc 1.47 in third_party/renderdoc_dl (gitignored); launched under renderdoccmd the game
  triggers a capture of the same frame; tools/rdoc/rdc_info.py runs in qrenderdoc. Xenia draws
  EDRAM through a UAV, so per-draw images need its RTV mode.

Where it stands: the title screen's final composite reproduces Xenia's scene, lighting,
depth of field, logo and "Press START" text. The final title capture compares at 29.71 dB,
with 1.6% of pixels differing by more than 32 in any RGB channel. This is image fidelity,
not a frame-rate measurement. Evidence is preserved under build/captures/title-final and
build/title-final-replay; the reference is xenia_title_cube.png in the capture directory.

2D layer fixed (2026-11-02):
- Index upload offsets now stay aligned to four bytes when mixing R16_UINT and R32_UINT
  views. Previously the three UI draws started at offsets congruent to 2 modulo 4, after
  odd-sized 16-bit index lists. Their vertex-dependent shaders produced no visible geometry;
  an SV_VertexID-only diagnostic triangle still rendered. Padding the allocation and each
  draw's offset restores the real geometry without shader overrides.
- Shader init matching now uses the container's physical section and its actual length,
  with the stage checked. The old 32-byte prefix fallback confused short shaders, including
  the 60-byte UI pixel shader (113 incorrectly selected as 003). All 557 captured objects
  still match; 20 associations change. Some containers have identical physical sections.
- Repacked the existing title capture: 185 records (138 draws, 47 resolves), all 45 required
  shaders compile, and fd_replay builds and runs successfully. The final target_12.png now
  shows the logo and text in the reference positions. Comparison with build/xenia_title.png
  improves from 17.05 to 18.44 dB (full RGB image; other scene differences remain).
- Regression command: `python -m unittest discover -s tests -p test_replay_pack.py -v`
  (five synthetic cases; no game data needed).

Further renderer corrections:
- Pixel constant bank c0..255 is retained, including ps_ZBufferParam at c230. Truncating at
  c223 made the DOF mask zero. Depth resolves now preserve both R32_FLOAT snapshots for
  depth-format fetches and the existing packed RGBA bytes for raw-format fetches.
- Texture capture covers full 32x32-block storage, and all six cube faces at their 4 KB-aligned
  strides. Small BC textures must retain tiled bank bytes beyond pitch * logical height.
  Cube SRVs and space2 indices restore ambient light (the statue and pillar were dark).
- Samplers retain per-fetch addressing and min/mag/mip filtering. The previous universal WRAP
  sampler wrapped colour LUT coordinates and produced green/red artifacts on bright pixels.
- Clear calls retain flags, ARGB, floating depth and rectangles. Original D3D surface objects
  identify their targets; D3D's temporary clear registers must not be replayed as real targets.
  Pending clears also apply to targets allocated later, and clear-only resolves retain contents.
  D32S8 host surfaces now retain stencil, including clears, reference/read/write masks,
  comparisons and operations. Packed depth-byte copies still put zero in the stencil byte;
  distinct back-face read/write masks are not supported by the host pipeline interface.
- Indexed draws with damaged parsed target state can align by initiator/index address between
  strict anchors. Repeated auto-indexed passes still require a target match. All 134 packed
  title draws now align with Xenia's register dump. SQ_PROGRAM_CNTL also distinguishes
  identical geometry in alpha/depth prepasses from the later colour pass. FDR6 includes
  pixel centres, stencil masks, partial copy offsets and mip destinations;
  the shader half-pixel offset is applied only for D3DZero draws.
- Twenty-two synthetic regression checks pass. `FD_DUMP_DEPTH=1` writes float snapshot PNG/raw
  pairs. `FD_NO_CLEARS=1` is an A/B diagnostic switch.

Gameplay diagnosis (2026-11-03): the saved first-scene capture packs all 1,138 actual draws,
50 resolves and 10 clears (three other CPU records have no draw). All 82 shaders compile.
Snapshots preserve all colour formats, four shadow atlas regions and render-to-mip copies.
The wall of water is visible after correcting alpha/colour pass alignment, but fidelity is
still insufficient: 12.66 dB against the synchronized reference before the FDX2 capture.
An earlier memory-snapshot comparison suggested the heightfield at record 6;
that comparison alone cannot identify the first divergent checkpoint.
FDX2 now captures actual GPU float constants. These agree with the device values in the
first pass (apart from shader literals), so constants do not explain the initial mismatch.
The fresh FDX2 gameplay frame compares at 12.17 dB; it does not pass the fidelity gate.
See [the repeatable workflow](RENDER_DEBUG_WORKFLOW.md); do not use stale PNGs after a failure.

RenderDoc: `FD_REPLAY_CAPTURE=1` while launching fd_replay under renderdoccmd records the
offscreen draw sequence. The verified final title capture is build/rdoc/title-final/native_capture.rdc.
For inspection without the GUI, convert with `renderdoccmd convert -f <rdc> -o <xml> -c zip.xml`,
then run `python tools/rdoc/native_bindings.py <xml> --json <out>` to inspect actual GPU index
views, pipelines and constant buffer bindings. The native capture verifies the index alignment.

Next phase: finish the synchronized gameplay comparison, then extract the replay's resource/state
handling into a live renderer and feed it from the engine hooks. The game still runs on Xenia's
GPU backend; fd_replay is offline. Remaining fidelity differences must not
be presented as a completed live renderer or DLSS integration.

## Exact-event correction results — 2026-11-03

The RenderDoc C++ inspector now indexes PSOs directly from the recorded API
chunks, joins microcode hashes to packed records and exports intermediate RTVs
and shader inputs in one session. This avoids a replay/GUI visit for every draw.
Signed DDS casts, MRT selection and atlas offsets are retained. Ambiguous joins
are rejected; optional prefix comparisons stop at the first unverified draw.

The decisive error was in texture decompression, not the initial heightfield
memory. The BC1 compressed bytes of the gameplay border mask matched Xenia
exactly, while the former palette assigned the stored second endpoint to index
3 rather than 1. The decoder now handles standard endpoint indices, BC1
three-colour transparency, BC2/BC3 forced four-colour mode, and BC5's two BC4
planes. The decoded border-mask image now matches the GPU input pixel for pixel.

Two further corrections:

- Fixed16 render targets use SNORM with the Xenos [-32,32] range and reciprocal
  write/fetch scales. Half float could exceed the range and lose precision.
- XenosRecomp retains `SetTextureLod` and explicit/register-LOD texture fetches.
  Previously the normal-map downsample read an uninitialized mip through implicit
  derivatives. Mip checkpoints now drop from about 0.564 MAE to 0.004/0.002/0.001.
  The changes are preserved in `patches/xenosrecomp.patch`; regenerate HLSL with
  `tools/regenerate_replay_shaders.py` and repack without renumbering containers.

| Matching frame | Earlier replay | Corrected replay | Pixels with any RGB difference >32 |
| --- | ---: | ---: | ---: |
| `captures/gameplay-gated` (actual first scene) | 12.85 dB | 30.03 dB | 36.1% → 1.6% |
| `captures/gameplay-used-textures` (second camera/frame) | 14.15 dB | 29.71 dB | 1.8% after correction |
| `captures/rtv-title-reference` | 31.85 dB | 33.50 dB | 0.2% after correction |

Depth surfaces are now keyed independently of colour-attachment combinations,
following the storage-identity pattern used by NativeXRenderer. Rebinding colour
attachments must preserve existing depth/stencil. The current saved gameplay
image is unchanged by this resource-sharing correction.

Current limitations remain explicit: remaining water/terrain edge differences,
static texture mip capture, texture3D handling and some conditional-draw cases.
The exact gameplay comparison verifies 1,067 draws and 24 checkpoints before
two PS183 draws absent from the reference GPU action stream. It does not claim
complete draw coverage. Thirty-three synthetic replay regressions pass, all 82
gameplay shaders compile, and the native renderer and runtime build successfully.

Evidence: `build/validation-title.txt`, `build/validation-second-gameplay.txt`,
`build/compare-gameplay-lod.log`, `build/gameplay-gated-replay/validation-rdoc.json`,
and the matching `build/rdoc/gameplay-gated/xenia_capture.rdc`. All are diagnostic
artifacts, not gameplay performance measurements. Continue resolving the base
differences before integrating live hooks; enhancement work remains deferred.

[Reusable NativeXRenderer components](NATIVE_X_REUSE.md) are documented separately.

## Base validation gate — latest result

The later fixes supersede the earlier incomplete prefix comparison:

- GPU capture records whether each guest call actually submitted a host draw.
  `gpu_draws.bin` and `gpu_mips.bin` carry a hash of the matching register stream,
  rejecting mixed-frame metadata. Two guest draws that the backend skipped are
  now omitted explicitly, allowing a complete 984-draw RDC alignment.
- Static 2D mip banks use Xenia's actual layout/packed-tail utilities, not guessed
  offsets. Thirty chains (245 extra levels) are decoded and uploaded on the host.
- FDR9 retains blend RGBA constants, complete texture fetch descriptors (LOD
  ranges/bias/borders), and all context registers. Constant-factor blending now
  uses the captured factor (the simulation uses 32/255), not D3D12's default.
- A shorter use of a static index buffer no longer replaces a longer identical
  snapshot. This recovered missing triangles in both shadows and the scene.
  Insufficient index data and shader compilation failures now fail explicitly.
- PA_CL_CLIP_CNTL controls host depth clipping. The former default depth clamp
  drew geometry beyond the last shadow cascade's near plane. Its checkpoint
  error falls from 0.133914 to 0.000010 after applying the captured clip state.

`build/captures/base-submission-mips` now replays at **39.16 dB** RGB PSNR.
Only **63/614400 pixels (0.010254%)** differ by more than 32 in any RGB channel.
The first shadow depth image has no missing geometry and mean absolute depth
error 0.00000373. All **41** exported colour checkpoints are below the chosen
0.01 average normalized error threshold; no first divergence is reported.
There are no D3D12 debug messages in this run.

The repeatable validation command is:

```powershell
# Requires Windows Graphics Tools (installed on this development PC).
$env:FD_REPLAY_DEBUG = '1'
./tools/validate_replay.ps1 -ReplayDir build/base-submission-mips-replay -Reference build/rdoc/base-submission-mips/reference.png -CaptureDir build/captures/base-submission-mips -RenderDocCapture build/rdoc/base-submission-mips/xenia_capture.rdc -MinimumPsnr 35 -MaximumOutlierPercent 0.1 -RequirePassAgreement
```

Evidence: `build/validation-base-gate.log`, the replay directory's
`validation-image.json` and `validation-rdoc.json`, and
`build/compare-base-depth-clip.log`. This is a saved-frame base-renderer gate,
not proof that every level/effect is correct or that live rendering is integrated.
The next base work is extracting this validated renderer into the runtime and
checking longer gameplay/resource lifetimes; DLSS/DLAA remain deferred.

## Independent-frame regression — 2026-11-03

A second fresh first-scene capture exposed two more source-selection bugs:
depth resolves incorrectly depended on the colour attachment, and CPU command
resynchronization could leave temporary or invalid copy-source registers.
Depth copies now select depth storage by surface/depth identity. Resolve packing
reads the source surface objects, as explicit clears already do. This restores
the distant ice formation and terrain; the new frame improves from 20.53 to
35.73 dB. All three captures were repacked and checked with the corrected code:

| Capture under `build/captures/` | RGB PSNR | Pixels over 32 | Aligned draws | Colour checkpoints |
| --- | ---: | ---: | ---: | ---: |
| `base-submission-mips` | 39.16 dB | 63 (0.010254%) | 984 | 41 |
| `gameplay-bright-complete` | 35.73 dB | 497 (0.080892%) | 1,084 | 41 |
| `title-base-complete` | 34.37 dB | 68 (0.011068%) | 134 | 40 |

The directory name `gameplay-bright-complete` is a capture label, not proof of
scene coverage: both gameplay frames are from the introductory scene. All
checkpoints have normalized MAE below 0.01, no draws or colour resolves remain
unverified, and there are no D3D12 debug messages. Each capture excludes one
clear-only colour copy from the draw-based comparison. Validation rejects a
missing native readback or an unexplained colour-copy exclusion; D3D12 errors
now make native execution fail before output images are accepted.

Evidence: `build/validation-base-surface-regression.log`,
`build/validation-bright-final-gate.log`, `build/validation-title-final-gate.log`,
and each replay directory's `validation-image.json`/`validation-rdoc.json`.
Image gates are at least 35 dB for the two gameplay frames and 34 dB for the
title, with fewer than 0.1% outlier pixels in each case. These are saved-frame
regression thresholds, not claims of pixel identity or full-game correctness.

## Breath tutorial and predicate correction — latest result

The elevated camera exposed an XenosRecomp `SETp_INV` error. The predicate became
true for input 1, but the generated scalar result incorrectly stayed 1 instead
of becoming 0. Terrain shaders use this predicate stack to select shadow
cascades. Fixing that case raises the new Breath capture from 34.67 to 40.24 dB.
The correction is retained in `patches/xenosrecomp.patch`.

The native replay also retains the original scene's 2x MSAA, resolves colour
before copies/readbacks, and selects the corresponding depth sample. Xenia's
default host sample pattern is used for parity; its optional programmable Xenos
pattern can be selected with `FD_XENOS_SAMPLE_POSITIONS`. Float depth snapshots
retain D24FS8/UNORM24 quantization. MSAA was a separate omission, not the cause
of the large terrain lighting difference.

All 93 shader sources needed across the four captures were regenerated and
repacked, including the depth helper shaders. The final enforced gates pass:

| Capture | RGB PSNR | Pixels over 32 / 614400 | Aligned draws | Colour checkpoints |
| --- | ---: | ---: | ---: | ---: |
| `base-submission-mips` | 39.41 dB | 20 (0.003255%) | 984 | 41 |
| `gameplay-bright-complete` | 36.03 dB | 409 (0.066569%) | 1,084 | 41 |
| `title-base-complete` | 34.37 dB | 45 (0.007324%) | 134 | 40 |
| `gameplay-breath-complete` | 40.24 dB | 505 (0.082194%) | 958 | 41 |

No D3D12 messages, unmatched draws, unexplained colour resolves or divergent
checkpoints occur. The C++ generated-expression test executes the actual output
of a synthetic `SETp_INV` instruction: five scalar/stack values and two boolean
round trips pass. Run `tools/test_predicate_codegen.ps1` in an x64 Visual Studio
developer shell. The 44 Python replay tests also pass.

Evidence: `build/validation-<capture>-predicate.log`, each corresponding replay
directory's JSON reports, and `build/test-predicate-codegen.log`. The Breath
capture was taken after Start at 140 seconds and LT held from 280 to 325 seconds,
with capture delayed until 430 seconds. This adds another camera/tutorial state
within the first level; later levels, long resource lifetimes and live native
integration remain to be verified. DLSS/DLAA remain deferred.

Final image export now uses opaque `frame.png`. The game's post-processing alpha
was previously written as PNG transparency, making previews much darker on black
backgrounds despite close RGB values. The final compatibility target PNG is also
opaque; intermediate/raw alpha is unchanged. All four image/checkpoint gates pass
again with opacity enforced, and 47 Python tests plus seven generated predicate
cases pass. See `build/native-reference-opaque-comparison.png` for the direct
reference/native comparison, with unchanged RGB bytes.

## Shared native core and runtime diagnostic

The validated implementation now lives in `runtime/gpu/native/renderer.cpp`;
`tools/fd_replay/main.cpp` is a thin CLI. `gpu::native::Renderer` owns one
device/queue/list/fence and waits for GPU completion before releasing a frame's
resources. Debug state belongs to the context rather than process globals.
Eight frames alternating all four captures (two cycles) complete with 6,320
game draws on one device, preserving all four image/RDC gates.

Configure `-DFD_NATIVE_RENDERER=ON` to link this same core into `FromDust.exe`.
`FromDust.exe --native-replay <directory> [<directory> ...]` renders saved frames
before guest workers start; four frames / 3,160 game draws have run successfully
inside the game executable. Normal Xenia-backed startup also passes a bounded
75-second smoke run. The option defaults off, and live guest draw submission is
not connected yet. No new native gameplay/performance claim follows from this.

Evidence: `build/validation-native-eight-frames.log`,
`build/validation-runtime-native-core-stdout.log`,
`build/smoke-normal-native-linked.log`, and
`build/validation-<capture>-shared-core.log`. Four malformed replay inputs now
fail before device initialization instead of allowing unchecked reads.

The later debug audit supersedes all earlier "no D3D12 messages" claims as a
validation result. `FD_REPLAY_DEBUG=1` now fails explicitly if layer activation or
its info queue is unavailable. Both the bundled 614 SDK and an isolated official
620 SDK test returned `DXGI_ERROR_SDK_COMPONENT_MISSING`; production remains on
614. Windows Graphics Tools was subsequently installed through
`tools/install_graphics_tools.ps1` with the user's UAC approval. DISM reports
`Installed`, with `RestartNeeded: false` (`build/graphics-tools-result.json`).

Optimized clear values reduce redundant clear warnings. This also exposed a
Plume bug: depth optimized clears used a typeless resource format instead of
the typed DSV format. `patches/plume.patch` fixes the format and explicitly
initializes depth/stencil; resource-creation failures now stop with a diagnostic.
Remaining warnings are retained in the logs: different later clear values and
float-backed placeholders for generated uint vertex inputs (the affected
fetches use shader pulling).

Active debug validation passes all four captures: 3,160 submitted game draws,
163 colour checkpoints, unchanged RGB metrics and zero D3D12 errors. Warnings
remain visible and counted (10/21/8/20 for separate-process capture runs).
Running inside `FromDust.exe` also exposed a binding-order assertion: clears
must transition every framebuffer attachment, including sampled shared depth,
before binding it. The corrected order passes the runtime's four saved frames
and an eight-frame CLI sequence with debug active. Evidence:
`build/validation-runtime-native-debug-active-stdout.log`,
`build/validation-runtime-native-debug-active-stderr.log`,
`build/validation-native-eight-frames-debug-active.log`, and
`build/validation-<capture>-debug-active.log`. Live native gameplay remains pending.

## Live shader identities and shared compiled assets

`tools/build_native_shaders.py` regenerates all 554 extracted containers with
the patched XenosRecomp and compiles actual VS/PS entry points with DXC. The
fixed specialization function matches the saved-frame renderer. Output goes
to ignored `build/native-shaders/`: HLSL, DXIL, a provenance manifest and one
`catalog.bin`. An incremental key covers the source container, recompiler,
common HLSL, DXC/DXIL libraries and flags; an unchanged rebuild compiles nothing.
The builder publishes the catalog only after every shader compiles.

The native core accepts `fd_replay --shader-catalog <catalog.bin> <replay-dir>`.
Both shader bytecode and output metadata then come from that catalog; a missing
entry fails before device creation, without falling back to replay-local assets.
The four captures preserve their RGB metrics and all 163 colour checkpoints
with the catalog selected and the D3D12 debug layer active. Execution reports
explicitly confirm catalog use. Evidence: `build/validation-<capture>-native-catalog.log`.

For live integration, initialization hooks register shader objects before D3D
patches vertex fetch instructions. Pixel virtual metadata starts at object
`+0x28`; vertex metadata starts at `+0x368`. The catalog compares full virtual
metadata and full physical bytes, including stage and lengths. Several
containers share physical code but have different metadata, so a physical
prefix or physical-code-only hash cannot safely select a native shader.
Unknown initialization erases any old object binding, including address reuse
across stages. Matching pointers refer to immutable catalog entries.

Set `FD_NATIVE_SHADER_CATALOG=<catalog.bin>` in an `FD_NATIVE_RENDERER=ON` build
to audit the registry while the game runs. `FD_NATIVE_AUDIT_REPORT` selects the
JSON report; its default is beside the catalog. Only complete CPU frames after
the first swap enter coverage totals. These are engine call counts, including
BeginVertices, rather than submitted GPU draw counts. The report explicitly
identifies Xenia as the rendering backend and does not claim native gameplay.

`tools/validate_native_live.ps1` launches a bounded unattended audit with the
known Start/A schedule, checks process success and report freshness, then
requires shader coverage and a scene-complexity threshold. Intro/menu-only
coverage is insufficient for that scene gate. The renderer still needs a live
frame/resource producer and presentation path; shader identities are its first
validated input.

The first bounded 300-second audit passed 7,471 complete CPU frames and
1,190,827 engine draw calls, including 1,088,298 indexed calls. All 557 shader
initializations matched; 75 catalog entries were used, with zero unresolved
draws. The final complete scene frame contains 634 calls. An incomplete catalog
fails explicitly, and the scene gate rejects an intro-only run even when its
shader identities are fully covered. Evidence:
`build/native-shaders/live-gameplay-audit.json`,
`build/native-shaders/live-missing-shader-audit.json`, and
`build/test-live-scene-gate.log`. This is validated live identity coverage, not
live rendering by the native core.

## Typed frame input, owned geometry and direct constants

The renderer now accepts `FrameInput` directly through `Renderer::RenderFrame`.
`ReplayFrame` is the file adapter; parsing and storage live in `replay_frame.cpp`.
It cannot be copied, and its move preserves the storage referenced by draws.
Frame inputs borrow immutable spans until the synchronous GPU fence completes.
An empty diagnostic directory produces no frame files; `FrameOutput` returns
opaque RGB and execution counts. Failure clears the output and does not count
a completed frame. This readback path is for correctness checks, not the final
native swapchain or an asynchronous performance design.

The C++ geometry producer decodes live vertex fetches using complete catalog
metadata, snapshots primary/pulled streams, converts formats and byte order,
and expands rectangle lists. `Geometry::Bind` supplies owned data to a typed
draw. `StreamArena` reuses identical stream snapshots while retaining different
contents if a guest buffer changes at the same address. All 3,160 saved-frame
draws match their validated vertices, indices, semantic layouts and pulled
streams exactly. Four frames rendered from preloaded memory match baseline RGB
exactly on one device; all four image/RDC gates still pass.

`FD_NATIVE_GEOMETRY_CAPTURE=<seconds>` adds one live geometry snapshot to the
shader audit. `FD_NATIVE_GEOMETRY_MIN_INDEXED` defaults to 500; the first eligible
scene frame is copied. `FD_NATIVE_GEOMETRY_REPORT` chooses its JSON report.
BeginVertices captures its shader/fetch inputs at reservation and reads completed
data before the next draw, clear, resolve or swap. Wraps, missing inputs and
unsupported draws fail explicitly. A captured batch remains owned and accessible
through `audit::CapturedGeometry()`; it is not yet a complete renderable frame.
The scope is one engine call's final draw packet, with no submitted-GPU-draw
count claim. Full call expansion/state and resource handling must precede native
backend activation.

The 315-second scene run captured 615 geometry packets, zero failures,
380,017,520 converted vertex bytes and 3,425,700 index bytes. These are diagnostic
snapshot sizes, not an optimized per-frame upload or a performance claim.

The constant producer copies both device float banks into each owned command.
An independent check compares used float registers from Xenia's microcode
analyzer against synchronized GPU state, excluding literals already embedded by
XenosRecomp: 37,388 float4 comparisons across all four captures have zero
differences. The same analyzer reports no memory export in all 554 catalog
shaders. Run `FromDust.exe --native-shader-capabilities <catalog> <report.json>`
to regenerate that analysis without starting the guest or creating a GPU device.

Evidence: `build/test-memory-frames.log`, `build/test-native-geometry.log`,
`build/test-native-geometry-unit.log`, `build/validation-<capture>-memory-core.log`,
`build/native-shaders/live-geometry.json`, `build/native-shaders/capabilities.json`
and each replay's `validation-device-constants.json`. Live texturing, complete
state reconstruction, resource persistence and presentation still need connection.

## Device state and native texture preparation

`SnapshotDeviceState` copies the XDK device's register blocks, bool/loop bank,
texture fetches and sampler metadata into an owned `Draw`. Index offset is an
argument of the engine draw call, rather than the stale device shadow. Scissor
bounds are explicit packet inputs: offline tests supply the synchronized GPU
bounds, while the live observer records the logical window-helper arguments.
Those logical bounds still need comparison with the submitted tiled draw state.
All 3,160 saved-frame draws match the reconstructed state. This does not prove
complete live call expansion or tiled scissor handling.

`texture_decode.cpp` implements the captured formats, endian conversion, tiling,
BC decompression, fixed-point expansion, cube faces and supplied mip layouts in
C++. `TextureTables` owns immutable versions even if a guest address is reused,
binds compatible fetches and packs the existing frame-input resource tables.
Across the four captures, 264 base textures, seven cubemaps and 1,160 mip levels
match the validated Python-produced bytes exactly. Replacing the resource tables
with these C++ results renders four frames / 3,160 draws with exactly the existing
native baseline RGB and no D3D12 debug errors. Baseline equality is not equality
to the Xenia reference; the previously measured residual differences remain.

The updated bounded Start/A schedule reaches the live complexity threshold after
separate intro dismissal and title confirmation. The observer captures 617 owned
commands: 617 geometry/state packets, 5,054,464 constant bytes and zero failures.
It still explicitly reports `native_rendering: false` and
`scope: engine-call-final-draw`; this is producer validation, not native gameplay.
Evidence: `build/native-shaders/live-state-scene.json` and
`build/native-shaders/live-state-scene-audit.json`. The final screenshot still
shows the menu background, so this count alone is not proof of playable-level
coverage. Earlier title-only runs below the threshold are rejected. Explicit
scene identification remains necessary for unattended gameplay validation.

Evidence: `build/test-native-state.log`, `build/test-native-state-unit.log`,
`build/test-native-textures.log`, `build/test-native-texture-unit.log` and
`build/test-native-resource-frames.log`. Live texture acquisition at the correct
time, clear/resolve ordering, cross-frame resources and native presentation
remain required before exclusive native gameplay.

## Local visual test

Double-click `Testar-render-nativo.cmd` to rerender all four saved frames with
the shared catalog and active D3D12 debug layer. It checks fresh outputs and
zero debug errors, then opens `build/teste-render-nativo/comparacao.html` with
native/reference images side by side. `tools/testar_render_nativo.ps1 -NoOpen`
runs the same diagnostic without opening the browser. This is a saved-frame
comparison, not an interactive native renderer. `Testar-jogo.cmd` launches the
current interactive runtime using Xenia and clears inherited diagnostic timers
and scripted input first.

## Air sparkles in the breath capture

The reported missing sparkles were a native sampling difference, rather than
animation at different times. Decoding the GPU-synchronized `frame_reference.bin`
reproduces the selected Xenia reference RGB exactly. RenderDoc pixel history
localizes the sparkle at (66,61) to event 7148, VS 182 / PS 183 (packed record
951). Its `tfetch3D` instruction reads a plain non-stacked 64x64 2D texture.

Xenos distinguishes an instruction's `k3DOrStacked` dimension from the actual
fetch constant's dimension. The native shader previously always selected the
unbound 3D heap and lost this contribution. The shared binding now tags plain
2D descriptors with bit 31 in the 3D descriptor table; `tfetch3D` and its explicit
LOD variant select the final 2D SRV, preserving its mip chain, swizzle and resolve
alias. This does not implement volume or multi-slice stacked uploads. The Python
packer now rejects stacked textures explicitly instead of retaining one slice.

After rebuilding all 554 catalog shaders and runtime/local replay assets, the
25x25 sparkle region's maximum RGB error falls from 31 to 1 (mean 1.702 to
0.193). Whole-frame PSNR improves from 40.239822 to 40.833551 dB and outliers
from 505 to 398. The other three frames retain their metrics; all 163 exact-event
colour checkpoints still pass. Residual image differences remain elsewhere.

`tests/native_effect_regions.json` pins small effect regions to exact capture
and reference RGB hashes. `tools/validate_effect_regions.py` runs in both replay
validation and the local visual launcher. The previous image fails this check;
the corrected one passes. Whole-frame averages alone missed this visible effect.
Evidence: `build/validation-particles.log`, `build/validation-effects-before.json`,
the breath replay's `validation-effects.json`, and
`build/validation-runtime-particles-{stdout,stderr}.log`.

## Sky light shafts and conditional shader execution

VS 117 / PS 118 draw the shafts in the title and scene captures. Their 16x64
BC3 mask has its base mip at X=16 texels inside a packed tail. Extracting that
region matches the RenderDoc texture exactly. Python and C++ now support packed
base mips in tiled/linear storage; already extracted higher mips are not offset
again. All four higher-mip tables remain byte-identical.

The compiler also omitted conditional EXEC guards. VS 117 executed both
alternatives of angular fade despite opposite boolean conditions. Boolean and
predicate EXECs are now guarded, with conditional END inside the guard. The
complete 256-bit boolean bank at shared offset 1680 preserves hardware indices,
including pixel booleans starting at 128. Rebuild shaders and runtime together.
These are translation/layout corrections, not added artistic lighting.

Updated metrics, using the same references and gates:

| Capture | PSNR (dB) | Pixels with error >32 |
| --- | ---: | ---: |
| base-submission-mips | 49.306323 | 20 |
| gameplay-bright-complete | 48.326504 | 0 |
| title-base-complete | 52.631946 | 0 |
| gameplay-breath-complete | 43.996322 | 376 |

All 3,160 draws and 163 exact-event colour checkpoints pass with debug active
and zero errors. C++ rebuilding matches 264 base textures, seven cubes and 1,160
mip levels, and renders exactly the updated native baselines. The title's left
shaft region rejects both the original dim image and the intermediate incorrect
attenuation. Its maximum/mean errors fall from 32 / 17.225 to 3 / 0.323. Other
title/breath shaft regions and the sparkle region pass too. Residual breath
differences elsewhere remain.

Evidence: `build/validation-light-shafts.log`, `build/test-packed-base-unit.log`,
`build/test-native-light-resources-final.log`, `build/effects-title-before-light.json`,
`build/effects-title-before-conditional.json`, each `validation-effects.json`, and
`build/validation-runtime-light-final-{stdout,stderr}.log`. All 554 shaders rebuild
and all 49 Python tests pass. Layout/control-flow semantics were independently
checked against Xenia's `texture_util.cc` and `shader_translator.cc`.

## Ordered native clear and resolve production

`operations.cpp` constructs owned clear/resolve commands from device surface
bindings, explicit clear arguments and the copy registers committed by each XDK
resolve call. Clear rectangles are copied before guest mutation. The parser
walks complete PM4 packets, skips payloads and rejects missing/truncated copy
state; it never treats a payload word as a register header. Null-texture XDK
flush calls are excluded. Partial texture destinations and supported mip copies
retain the existing replay semantics. Missing/noncontiguous command ranges and
unsupported layouts fail explicitly.

All 39 clears and 197 resolves across the four captures match the meaningful
operation state; all 197 copy-register sources come from contiguous CPU packets,
without synchronized GPU-register fixtures. Unused colour state in depth-only
operations may differ from the old adapter's stale tracked registers. Replacing
every saved operation with the C++ result renders all four frames / 3,160 draws
with exactly the validated native baseline RGB and zero D3D12 debug errors.
Unit checks cover packet payload isolation, truncated data, write-one-register
headers, missing surfaces and clear-rectangle ownership.

The bounded live observer now retains clears and resolves among geometry
commands. A complex frame captured 615 geometry commands, 11 clears and 50
resolves: 676 ordered owned commands, zero failures. This still has the explicit
`engine-call-final-draw` scope and is not a complete playable-level proof or a
native presentation path. Live textures at the right time, complete submitted
draw expansion, persistent GPU resources and native presentation remain pending.
The diagnostic copies 380 MB of converted vertices; these snapshots are not an
optimized production upload path or a performance measurement.

Evidence: `build/test-native-operation-unit.log`,
`build/test-native-operation-frames.log` and
`build/native-operations-live/live-geometry.json`.

## Settings HUD and renderer information

F1 opens the settings HUD. Window presets now include 1440p and 4K; the interface
adds frame interval visibility, four HUD corners and a 75–150% interface scale.
These choices are normalized and saved alongside existing settings. They apply
without a graphics restart.

The renderer tab reports the actual active Xenia / Direct3D 12 backend, adapter,
physical window size, startup graphics settings, local DXGI memory usage/budget
and optional native audit progress. It explicitly identifies native integration
as incomplete. No unavailable renderer switch or DLSS option is exposed.

Performance history contains up to 240 real guest swap intervals, measured
after frame pacing, independently of HUD paints. It shows FPS, mean, P95 and
maximum interval, a graph and draw requests per frame. These are delivery
intervals, not GPU execution timestamps, and draw requests include internal
operations rather than counting only submitted native draws. Native gameplay
performance must be measured after live integration, with one game instance and
diagnostic snapshots/readbacks disabled; saved-frame rendering cannot establish
interactive FPS. The observed Xenia HUD FPS is not a native benchmark.

The settings/input and frame timing tests pass, including round trips, bounds,
restart requirements, history wrap, percentiles and reset. The live game HUD
was also visually checked at 1280x720, including the renderer tab and scrolling.
Both launchers clear inherited settings-open/tab diagnostic overrides so a
normal launch or unattended validation does not accidentally block game input.

## Optimized runtime build and performance baseline

The existing CMake cache selected Release but had an empty
`CMAKE_CXX_FLAGS_RELEASE`; generated build rules confirmed no C++ optimization,
including the recompiled guest modules. `build_all.ps1` now explicitly pins
`/O2 /Ob2 /DNDEBUG` when configuring Release. All runtime/guest/backend targets
were rebuilt with those flags. This benefits the common CPU runtime and does
not activate native presentation.

A single offscreen Xenia instance at the title's Start prompt, using identical
settings (verified SHA-256), improved from 18.7 FPS (18–19) to 39.7 FPS (38–41).
Each result uses the last ten one-second swap-count samples, after profiling
and the screenshot capture finished. Both screenshots were checked for the same
screen. Faster loading required fewer scripted Start presses after optimization;
the earlier optimized run reached the main menu and is explicitly excluded from
this comparison. Animation frames and background workload are not locked.
These are bounded title measurements, not native-backend or gameplay FPS, and
do not establish a 60 FPS target. Evidence:
`build/perf-title-before/measurement.json`,
`build/perf-title-after-matched/measurement.json`, their `frame.png`/logs, and
`build/build-optimized-runtime.log`. The bounded reproduction script is
`build/measure-title.ps1`.

The optimized runtime renders four native saved frames / 3,160 draws with the
D3D12 debug layer active and zero errors. Their RGB also exactly matches the
pre-optimization comparison HTML's embedded native images. Settings/input and
frame timing checks pass with optimization enabled. Evidence:
`build/validation-runtime-optimized-{stdout,stderr}.log` and each replay's fresh
`native-report.json`. Native command reconstruction also passes all 236
operations with the debug layer active (`build/test-native-operation-frames.log`).

## First-use vertex declaration timing

The optimized runtime reaches the rendered introduction more quickly. Its
native observer exposed nine first-use vertex-layout failures that the earlier
title/menu captures did not exercise. Small before/after shader dumps show the
cause directly: the XDK patches zero format/stride placeholders during the draw
flush. For example, VS 169 changes from format zero to formats 26/31 and a
32-byte stride. Other skinning/particle shaders also reorder fetch destinations
and select secondary streams while applying their declarations.

The observer now copies completed vertex code in `End`, immediately after the
guest draw call returns. Inline `BeginVertices` commands keep that owned shader
snapshot while waiting for their reserved vertex bytes to be filled. Reading
the code later, when flushing an inline command, could pick up a subsequent
rebind. Device constants remain snapshots of their draw's input state.

Diagnostic failures identify the shader, register and semantic and save owned
before/after code under the report's filename. The optional
`FD_NATIVE_GEOMETRY_MIN_PATCHED` / `validate_native_live.ps1
-MinimumPatchedShaders` regression mode retains the first complete valid frame
that exercises the requested number of layout updates. Valid frames without
that coverage are counted and discarded; any conversion/order failure remains
terminal. It retains the existing scene-complexity threshold and bounded run.
`-Screenshot` captures the final runtime scene for manual identification; that
image is not claimed to be the exact captured geometry frame.

Evidence of the original failure: `build/native-operations-live-optimized`,
`build/native-geometry-diagnostics/live-geometry.json`, its `.failure-*.bin`
snapshots and `scene.png` (the rendered waterfall introduction, not a menu).
The saved-frame geometry regression still matches all 3,160 draws exactly.

The targeted run `build/native-first-bind-verified/live-geometry.json` now
captures nine declaration updates and completes with zero failures: 711
geometry commands, 11 clears, 50 resolves and 772 ordered commands in total.
It owns 1,230,480 pulled-stream bytes as well as both constant banks. The
required patch-coverage gate is enabled; no unpatched frame was skipped in this
run. Evidence: `build/test-native-first-bind-verified.log` and
`build/build-native-first-bind-final.log`. The full report remains explicitly
`native_rendering: false`; this verifies completed input ownership, not native
rendering of that introduction or a native playable level.

## Native texture memory producer — 2026-11-04

`texture_storage.cpp` now derives base/mip addresses, row pitches, aligned
subresource offsets and packed-tail positions directly from fetch descriptors.
It does not call Xenia layout utilities or consume captured mip descriptors.
The rules are adapted from the local BSD-licensed texture utilities, with
explicit physical-memory bounds and unsupported-view failures. Base 2D reads
exclude unused trailing rows; cubemaps retain the existing six-aligned-slice
decoder contract. Mip-bank extents match the independent GPU layout reference.

`SnapshotTexture` reads and owns both banks. Packed level zero and its higher
levels share one memory read when their addresses overlap. `TextureTables::Capture`
decodes those banks using the fetch pitch, without the old capture adapter's
tightly-repacked-image inference, and builds the native frame resource tables.
Linear BC base rows are aligned in blocks as required by the storage layout.
The caller must synchronize GPU-written memory before invoking this API.

`FD_FRAME_CAPTURE=<seconds>` together with `FD_NATIVE_TEXTURE_CAPTURE=1`
connects the producer to the GPU-consuming texture hook, after the diagnostic
full resolve readbacks. It exports `native-{textures,cubes,mips}.rgba` plus
`native-texture-report.json`, bound to the completed register stream hash.
The exact scope is `gpu-consumer-first-use-per-address`, matching the existing
raw capture's resource inventory. A base first used without mips can acquire
its mip bank when first used later. Repeated unchanged descriptors do not
recalculate the layout. This still relies on Xenia's consuming thread and
readbacks; it is not an exclusive native resource or presentation path.

The repeatable bounded command is:

```powershell
.\tools\validate_native_textures.ps1 -Screenshot
```

It uses isolated 1x settings, clears unrelated diagnostic overrides, checks
fresh outputs and runs `validate_native_texture_capture.py`. The reference
decoder reads the raw banks and independently captured GPU mip layouts. It
rejects incomplete/failed reports, register-hash mismatches, wrong pixels,
duplicate mip records and truncated tables. Explicit mip resource indices make
record ordering immaterial. Normal interactive and native geometry launchers
clear the new diagnostic override.

The final 120-second title run acquires 49 base textures, one cubemap and 178
mip levels with zero producer failures and exactly the independent reference
pixels. `scene.png` was inspected and identifies the title's Start prompt.
Evidence: `build/native-texture-live-final/native-texture-report.json`,
`validation-textures.json`, `scene.png` and the runtime logs in that directory.
The earlier successful run remains in `build/native-texture-live-title`.

Across the four saved captures, 264 bases, seven cubes and 1,160 mip levels
match the reference. All mip pitches/offsets/packed positions and bank extents
are also checked against the GPU metadata. The production memory-reader entry
point reconstructs every resource without being supplied those layouts. Four
frames / 3,160 draws rendered from its tables retain exact native baseline RGB,
with the D3D12 debug layer active and zero errors. Existing input-signature
reinterpretation warnings remain. Evidence:
`build/test-native-memory-textures-render.log` and
`build/test-native-texture-storage-unit.log`.

The ownership unit checks cover shared tails, guest mutation, pitched linear
images, aligned linear BC rows, physical-memory bounds and unreadable banks.
All 54 Python tests and both runtime CTests pass. Full per-draw resource version
tracking, resource persistence across frames, complete submitted-draw expansion
and native presentation remain pending. Mip-only views, 3D/stacked textures,
cube mip chains, float mip tables and overlapping base rows fail explicitly;
they are not silently replaced or inferred.

## DLAA / DLSS and CPU frame production — 2026-11-05

**DLAA** (`runtime/gpu/native/upscaler.{h,cpp}`, `motion.hlsl`) uses the NGX
DLSS SDK directly (github.com/NVIDIA/DLSS, sparse checkout in
`third_party/dlss_sdk`, not committed; `nvngx_dlss.dll` is copied next to the
executable). Standard NGX calls also let NGX injectors such as OptiScaler (and
its DLSS 5 neural-rendering fork) attach as they do to PC games.

- Scene boundary: the last colour resolve with the front buffer's size that is
  followed by a smaller pass (in From Dust: the copy to `0x00DBC000` before
  the 512x300 / 256x150 bloom chain and the HUD). DLSS evaluates that copy and
  the result replaces it, so post-processing and HUD are unchanged.
- Jitter: Halton(2,3), 8+ phases, added to the shared half-pixel offset of
  scene draws at output size (index count > 6, so screen quads are excluded).
- Motion vectors: camera-only reprojection of the scene depth with this and the
  previous frame's world-to-clip matrix — `VS_DustConst[8..11]` (c28..c31) of
  the terrain shaders, detected by shader source. Moving units and deforming
  water/terrain get camera motion only (DLSS treats the rest as disocclusion).
- F1 > Gráficos > Anti-aliasing selects it live; `FD_NATIVE_UPSCALER=dlaa`
  overrides; `FD_UPSCALER_NO_JITTER`, `FD_UPSCALER_JITTER_FLIP`,
  `FD_UPSCALER_BYPASS` are diagnostics. Diagnostic frame dumps add the scene
  before / after DLSS and the motion vectors (`resolve_*_D155*`).
- Verified: thousands of evaluated frames with the D3D12 debug layer and no
  errors. Super Resolution modes: see "Internal resolution and DLSS Super
  Resolution" below.
- DLSS 5 (3D-guided neural rendering): the public Streamline 2.14.16 declares
  `kFeatureDLSS_NR` but ships no plugin or header; the model is delivered to
  supported games over the air. Not integrated.

**CPU frame production** was the native path's real bottleneck (15–16 FPS,
renderer ~10 ms). With a game-thread profile (`FD_PROFILE`, which now also
prints self <- caller pairs) and `FD_NATIVE_PROFILE` capture timings:
vertex streams / indices cached across frames by guest range and write-watch
revision with bulk byte swaps; per-frame command / stream vectors keep their
capacity; `DescribeTextureStorage` cached by fetch; texture revisions re-read
only after watched writes; fixed16 decode hoisted; hot `getenv` removed.
Result: 30–31 FPS (the game's own rate) with DLAA. Texture, geometry and
memory-frame regression tests still match exactly. Remaining per-frame cost:
~6 ms re-decoding the two CPU-written water textures (3 MB), ~3 ms geometry,
~10 ms rendering.

Synchronized native vs Xenia frames (same swap, `validate_native_textures.ps1
-NativeFrame`): 50.44 dB / 15 of 614,400 pixels over 32 at 325 s, 52.41 dB /
0 pixels at 410 s.

## DLSS Frame Generation (2x-4x) — 2026-11-05

From Dust simulates and renders at 30 FPS. NVIDIA DLSS Frame Generation
(DLSS-G, multi frame generation on RTX 50) adds 1-3 generated frames between
real ones, so the display shows 60 / 90 / 120 FPS while the game keeps its own
rate (latency stays that of 30 FPS). Implementation:
`runtime/gpu/native/frame_generation.{h,cpp}` on the public Streamline SDK
2.14.16 (`third_party/streamline`, the extracted release package, not
committed; the six DLLs it needs are copied next to the executable).

- **Manual hooking.** Streamline is initialized only when the setting is on at
  start-up, before the Xenia D3D12 provider exists. The provider hands its DXGI
  factory to `slUpgradeInterface`, so the swap chain the presenter creates is
  the Streamline proxy; everything else keeps the native D3D12 interfaces.
  `sl.interposer.dll` is delay-loaded after `sl::security::verifyEmbeddedSignature`.
  The hooks are `xe::ui::d3d12::fd_present_hooks` (patches/xenia.patch).
- **Inputs.** The renderer reuses the DLAA motion pass (now also run with DLAA
  off): R32F reversed depth and RG16F camera motion vectors at 1024x600,
  tagged as `eOnlyValidNow` on the renderer's command list (Streamline copies
  them; the next frame rewrites them before this one is presented), with
  `mvecScale = 1/size`, `clipToPrevClip` (transposed: Streamline is
  row-vector) and depth inverted. The presenter tags the back buffer subrect
  holding the guest output and, as hudless colour, the back buffer right
  after the guest output is drawn and before the PC overlay (F1 button, FPS,
  menu) — so the overlay is not warped.
- **Frames and pacing.** One frame token per rendered frame; Reflex low
  latency with `slReflexSleep` and PCL markers (simulation / render submit at
  the renderer, present start / end around the Streamline `Present`).
  Xenia repaints the window for the UI far more often than the game renders
  (240 Hz here); while frame generation is on, repaints of an already
  presented frame are skipped so each game frame reaches the pacer once.
  DLSS-G is switched off (resources retained) when a frame has no scene (menus,
  videos) and while the F1 panel is open. It also pauses itself when the window
  is not focused.
- **Settings.** F1 > Gráficos > DLSS Frame Generation: off / 2x / 3x / 4x.
  Enabling or disabling applies on restart (Streamline must hook the factory
  first); the multiplier changes live and is capped by the GPU's
  `numFramesToGenerateMax` (5 on the RTX 5060 Ti). The status line shows
  availability and the tagged frame count; the FPS counter shows game and
  displayed rates. `FD_FRAME_GENERATION=0..3` overrides the setting,
  `FD_FG_SHOW_GENERATED` shows only generated frames (artifact inspection),
  `FD_STREAMLINE_VERBOSE` forwards Streamline's info log, `FD_FG_REFLEX_OFF`
  leaves Reflex off.
- **Coexistence with DLAA.** The direct NGX DLAA path and Streamline share the
  driver's NGX runtime; both run in the same frame.
- **Verified** (`tools/test_frame_generation.ps1`, 239 Hz monitor, VSync on):
  2x -> 2 frames per present; 4x -> 30 game frames/s, ~120 displayed/s, DLSS-G
  status OK, thousands of tagged frames.

## Internal resolution and DLSS Super Resolution — 2026-11-05

The native renderer can draw the game above 1024x600 (F1 > Gráficos >
Resolução interna: 1x, 1.5x, 2x, 2.5x, 3x, applied live; `FD_NATIVE_SCALE`
overrides, also for replays). Only **screen targets** are scaled: the front
buffer's size, or its width with a letterbox up to 10% shorter (videos draw
1024x588). The terrain / water simulation passes, shadow maps and the bloom
chain keep their guest sizes, which is why the effects survive (Xenia's
whole-EDRAM scaling breaks them).

- `Target` keeps its guest and host sizes and its scale; the scale is part of
  the target key, so one EDRAM surface can exist at two scales in a frame.
  Host viewports, clear rectangles, resolve regions and the snapshots /
  depth snapshots of scaled targets are scaled; the half-pixel offset stays
  in guest pixels (it is half a guest pixel whatever the raster density).
- `vPos`: XenosRecomp now emits `iPos.xy / (1 + g_NativePixelScaleMinusOne)`
  (shared constant 1740, 0 at native size, so 1x is bit-exact), keeping screen
  space reads (refraction, depth-based effects) in guest pixels. Rebuild the
  catalogs with `tools/build_native_shaders.py` after updating XenosRecomp.
- Changing the scale drops targets of the old scale and the colour / depth
  surfaces only they referenced, and resets the resolve aliases.
- **DLSS Super Resolution** (F1 > Anti-aliasing / DLSS: Qualidade 67%,
  Equilibrado 58%, Desempenho 50%, Ultra desempenho 33%): the scene draws
  (up to its copy) render at `scale x ratio`, DLSS writes the copy at the
  internal resolution into its own surface, and the post-processing / HUD read
  it at full size. With 2x and Desempenho the scene renders at 1024x600 and is
  shown at 2048x1200. Jitter is in render pixels; frame generation's motion
  vectors and depth are the render-size ones.
- Cost (RTX 5060 Ti): 2x with DLAA ~11.5 ms render per frame in the level
  (vs ~10 ms at 1x); the title at 2x ~6.4 ms. All still at the game's 30 FPS.
- Verified: texture / geometry / memory-frame regressions exact at 1x with the
  rebuilt catalog; title, menus, intro video and the first level match 1x at
  2x; DLSS Performance 1024x600 -> 2048x1200 and Quality in the level.

## Rendering off the game thread — 2026-11-05

The native renderer used to run inside VdSwap on the game's render thread:
in the level's opening flight the thread spent ~46% in VdSwap (render, present)
and ~32% capturing draws, leaving ~23% (about 11 ms) for the game itself, so
heavy scenes ran at 20-22 FPS. `live_audit.cpp` now hands each captured frame
to a render worker: the batch's command / stream vectors are swapped with
recycled ones, the frame's texture references are copied (resources are
immutable `shared_ptr`s, replaced on change), and the game goes on capturing
the next frame while the worker builds the draws, renders, presents and paces.
One frame is in flight; the next hand-over waits for it. `FD_NATIVE_SYNC=1`
restores inline rendering. The renderer never reads guest memory, so the
overlap is safe; geometry bytes are copied into each frame's own arena.

Result: the level's opening flight holds 30 FPS (was 20-22). With
`FD_VBLANK_HZ=120` and no frame limit the game loop itself reaches 60 FPS
there, and 40 in the menus. At 60 FPS the camera of that cutscene stays on time
(it is time-based), but the sea and the narration run ahead: the simulation
steps per frame, so a higher rate needs the simulation decoupled from the
render loop first (in progress).
