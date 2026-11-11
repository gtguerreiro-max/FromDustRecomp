# Active native integration work

## Water stripes fixed (2026-11-04)

The striped / spiky water seen with **both** the native and the Xenia backends
came from the CPU, not from a renderer. The heightfield texture (physical
0x0AC3A000, k_16_16) is written by `sub_89199A98` in
`GalileoSimulatorWater_9_9.xex` (found with `FD_TRACE_WRITE_PHYS=0AC3A000:100000`,
which logs the guest writer of a watched physical range).

Root cause: XenonRecomp disassembles with `"cell 64"`, whose default dialect
still includes `PPC_OPCODE_403` (PPC405). The 405 multiply-accumulate entries
share opcode 4 with VMX128 and precede it in the table, so two `vsldoi128`
(neighbour shifts at the tile halo) were decoded as `mullhwu.` and became a
scalar multiply. A third case in `dustai.x360.f` clobbered r1. The dialect now
drops PPC_OPCODE_403 for cell (patches/xenonrecomp.patch); regenerating the two
modules changes exactly those three instructions. The heightfield no longer has
vertical stripes.

`tools/vmx_conformance/run.ps1 [-Modules <ppc dirs>]` compares every distinct
VMX instruction form of a recompiled module against an independent
big-endian reference. For the water module all 389 forms pass except the sign
of zero in `vmaxfp`/`vminfp` (SSE returns the second operand for ±0); that
remains a known, separate difference.

## Latest continuation status (2026-11-04)

The persistent goal tool currently reports **usageLimited**, not complete. The
user subsequently requested online research; findings and falsification checks
are in `docs/NATIVE_ONLINE_FINDINGS.md`. Do not claim that native gameplay is
finished. Bounded diagnostic sessions may be running during the continuation.

Follow-up progress in this continuation:

- `native-session-volume-interactive` reached the real tutorial, created the
  Breath and responded to left-stick movement. Its saved frame shows the move /
  camera tutorial; the later screenshot shows the near-man prompt. It **failed**
  afterward on VS 000 command discontinuity A0211E50 -> A0012084 and must not be
  counted as a successful complete session. It ran without the debug layer.
- Found a missing lifetime case: `BeginVertices` moves its pending operation
  from `stack` into `inlineDraw`, but command-block preservation only traversed
  `stack` and resolve. A block can be recycled before the next draw flushes
  that deferred operation. CommandBlock(begin) now flushes the matching inline
  draw before commands **and inline vertices** can be overwritten. The current
  longer debug-layer run is `native-session-deferred-inline`; outcome pending.
- Added an explicit experimental native launcher (`Jogar-nativo.cmd` /
  `play.ps1 -Renderer native`). Normal launchers retain Xenia. Interactive
  launch clears diagnostic timers, scripted input, dump / trace options and
  isolated settings. Both PowerShell scripts parse with zero syntax errors.
- `native-session-shared-memory-gameplay` completed **7,759 frames / 3,046,281
  CPU draw calls**, no unresolved shaders or geometry failures. It remained in
  the title backdrop and **does not validate gameplay**. The later headless
  `native-session-volume-gameplay` also remained there (6,352 frames).
- The actual windowed `native-session-navigation-profile` reached the first
  tutorial with textured sphere / figures, and LT advanced the camera. It then
  **failed** on a genuine linear BC3 128x128x32 volume used by VS 182. This is a
  demonstrated missing feature, separate from the physical-alias correction.
- Implemented base-level volume ownership / decoding (linear and tiled 3D),
  native D3D12 3D images, slice uploads, swizzled SRVs and versioned reuse.
  `volumes.rgba` is an optional backward-compatible replay sidecar. Volume mip
  chains / packed volume tails and stacked arrays remain explicit unsupported
  cases. The observed effect has no mips and is not packed or stacked.
- Texture unit tests check the actual BC3 dimensions / slice order and hardware
  tile-bank boundary vectors. `fd_native_volume_tests` checks every pixel of
  three GPU-sampled slices, padded upload rows, persistent reuse, swizzle,
  saved-replay round trip and truncated-data rejection under the debug layer.
- Added opt-in `FD_NATIVE_PROFILE` stage / draw-category timings, indexed named
  shader lookup and per-frame hot diagnostic-option reads. The current fresh
  test is `native-session-volume-interactive`; do not claim its result yet.
- Session validation now records its executable hash and schedule in
  `session-options.json`, with an optional `DebugLayer=false` performance run.
- Found the missing-color cause in physical memory: GPU reports written through
  A were not visible through the C alias used by XDK `D3DQuery::GetData`. A/C/E
  now share a page-file section, including E's physical +4 KB offset. Page-write
  watches, physical-address conversion and allocator lookup use that mapping.
  Completed queries now return 100 in the original consumer, without a result
  override. Four runtime CTests pass, including alias readback / protection.
- `native-session-physical-aliases` completed **4,207 frames / 939,594 CPU draw
  calls**, zero unresolved shaders, no D3D12 or geometry errors. Fresh title
  transition images have full color and matching mesh / depth counts. This
  does not yet prove the first level; the longer headless
  `native-session-shared-memory-gameplay` is the current validation.
- The user requests Computer Use be closed immediately after visual checks to
  avoid resource use. Its prior session has been completed and its JS kernel
  reset. Headless sessions, logs and saved PNG inspection remain available.
- Saved command-block hooks also cover 820D7C00, the lower finalizer that can
  bypass public flush / reservation wrappers. No discontinuity occurred in the
  completed physical-alias session. Failed earlier sessions remain failures.
- Fixed native occlusion report address (0x2325), 32-byte little-endian layout,
  cumulative visible counts and completion ordering. The latest fallback
  advances every completed report, including deferred survey intervals.
  ZPD traces now contain valid guest destinations. This is a conservative
  visibility fallback, not native GPU occlusion measurement.
- `native-session-visible-query` completed **6,772 frames / 1,411,947 CPU draw
  calls**, zero unresolved shaders, no D3D12 errors, and no geometry failures.
  It exercised Play > New story and the opening video, but its later scene
  still has missing color silhouettes. Actual native gameplay is unverified.
- Used computer-use to open F1 and inspect Screen / Graphics / Controls tabs in
  the native window. The final logo inset is clean in a fresh live title/menu
  image (`native-session-query-address/native-session-frame/frame.png`).
- The settings UI now identifies the active native backend correctly. Internal
  scaling / AF overrides are disabled for native sessions because they are not
  yet implemented there; the normal Xenia backend retains those options.
- Added the lower ring-reservation hook at 820D70D8 after a direct reservation
  bypassed 820D7D88. The five-minute visible session above exercises that hook
  without the prior discontinuous-command-block failure.
- `FD_TRACE_FRAME_COUNTS=1` compares CPU-collected mesh / terrain calls against
  actual PM4 consumption. `native-session-frame-counts` is the current bounded
  investigation. Do not infer that missing color calls are a texture problem
  before checking these counters. The shader capability survey found **zero
  memory-export shaders**, so do not implement speculative memexport handling.
- The counters agree exactly between CPU capture and PM4 consumption (for
  example frame 3541: 21 mesh / 11 terrain / 21 mesh-depth in both). Missing
  color calls are already omitted by the engine, not lost between collectors.
- `native-session-command-context` completed **3,829 frames / 873,885 CPU draw
  calls** with no geometry failures, reaching the first level and the “Hold LT
  to create the Breath” prompt. The saved image still has missing-color figures.
  Hooks at 820D65A8 (saved command-context restore) and 820D69A0 (temporary
  context reset) fix the remaining clear-quad discontinuity observed earlier.
- Verified F1 open/close, Screen / Graphics / Controls / native statistics in
  the real window. Toggling Show FPS displays the counter and writes ShowFPS=1
  to the isolated session INI. Final logo remains clean in fresh live images.
- Raw vertex streams now optionally deduplicate by watched page revisions.
  Unit checks cover immutable prior snapshots, changed stream generations,
  unchanged reuse and acquisition races. Four frames / 3,160 draws still have
  exact baseline RGB. The latest live performance run is
  `native-session-visible-queries`; do not claim its results before it ends.
- New diagnostics include fingerprinted `native-state.bin`, preserving per-draw
  CPU texture ownership, resolve source registers and front-buffer identity.
  Tests reject stale / truncated / empty sidecars and accept legacy FDR9 files.
  GPU history is still intentionally not serialized by a one-frame capture.

The sections below describe the earlier starting point. Since then:

- `FrameInput::continuous` retains targets, resolves, pipeline/shader caches and
  upload buffers. Color surfaces can share storage across depth bindings.
  Descriptor indices are rebound per frame; retired snapshots survive fences.
- Eight-frame GPU history test passes, including retained color when the bound
  depth buffer changes; default isolated replay still matches all 3,160 draws.
- Typed owned `TextureResource` inputs avoid serializing/copying images each
  frame. CPU page-write revisions invalidate decoded resources; immutable
  versions reuse GPU texture uploads. Old GPU texture versions are retired.
  `fd_native_texture_tests --render-owned` passes 264 bases / seven cubes /
  1,160 mips and four images with exact baseline RGB.
- `LiveResources` records per-draw CPU ownership and GPU-resolve authority;
  `Draw::cpuTextureMask` selects CPU changes over older native copies.
- The collector can run continuously from startup, hand off between render
  threads at completed operation boundaries, preserve command blocks at
  `820D7D88`, and expand multiple index chunks using engine primitive/count
  arguments. `command_stream.h` recovers validated packet suffixes around XDK
  inline data. Missing draw counts and incomplete copy state remain fatal.
- Plume can adopt the UI provider's existing D3D12 device. A fenced GPU compute
  copy presents native output to the UI presenter's R10G10B10A2 image; no CPU
  image upload is used for presentation. `FD_RENDERER=native` creates no Xenia
  GPU GraphicsSystem. The small runtime PM4 protocol service still runs.
- Settings UI now accepts provider/presenter directly and remains available
  to both backends. Native visible-window / HUD interaction has not yet been
  verified with computer use.
- `tools/validate_native_session.ps1` runs bounded native sessions with isolated
  settings and screenshots. Runs have completed up to 3,446 frames / 260,344 CPU
  calls without unresolved shaders or D3D12 errors. **Their images still show
  blue silhouettes / missing detail during transition.** These are execution
  milestones, not fidelity or gameplay completion.
- Persistent utility pipelines and upload buffers reduced title-frame native
  render cost from roughly 57 ms to roughly 9–14 ms in measured runs. Complex
  transition costs remain higher; do not advertise a gameplay FPS guarantee.
- `runtime/gpu/ui_fixes.cpp` hooks `8250D940` and narrowly identifies the title
  logo's six-vertex 512x256 atlas sprite. Its UV footprint is inset to protect
  against adjacent sprites in mip filtering. The final diagnostic inset is
  2.5..509.5 by 2.5..253.5 texels. A clean saved-frame comparison removes all
  dark-line pixels at the right boundary (mean residual -0.014, minimum -0.5).
  The earlier 0.5-texel inset left a faint line; do not reuse it. Hook activation
  was verified in a live run; a fresh live screenshot of the final 2.5 inset
  still needs checking.
- Additional synchronized CPU-input / GPU-consumer-texture validation passed
  at 51.4268 dB for 634 native draws, 694 commands, 57 textures / two cubes /
  216 mips, with no pixels over 32 error and zero D3D12 errors. Evidence:
  `build/native-cpu-frame-intro`. This reference shows a title transition.

Current build: `build/FromDust.exe`, catalog
`build/native-shaders-live/catalog.bin`. Latest build/test logs include
`build/native-continuous-build.log`, `build/test-native-history-eight.log`,
`build/test-native-owned-textures-render.log`, `build/test-native-frame-shared.log`.

Outstanding: identify and fix continuous-image divergence, audit completion
ordering / EDRAM ownership using the online primary sources, verify actual
native gameplay and transitions, validate visible settings / controls / audio,
test final logo in a fresh live run, and update release-facing documentation.
Older FDR9-only diagnostics omit `cpuTextureMask` and resolve source metadata.
New captures retain those in `native-state.bin`; neither format stores the full
preceding GPU history, so do not assume an isolated replay reproduces all live state.

The user explicitly requested finishing the remaining native integration and
continuing until complete. The thread goal is active, with no token/time budget.
Do not declare completion on the basis of another isolated-frame milestone.

## Verified in this work session (2026-11-04)

- GPU primary vertex pulling now uses existing descriptor-driven shader branches
  and a single zero-stride IA vertex. Rectangles retain CPU expansion. Optional
  `GeometryBounds` also restricts index ownership and validates base vertices.
- Full fetch source index is captured before its destination overwrites the
  register; mini fetches inherit that index, stream, stride and rounding.
- D3D12 SV_VertexID excludes BaseVertexLocation. Shared offset 532 restores the
  Xenos VGT index offset in shader r0. Source: Microsoft's HLSL extended command
  information specification. The rebuilt catalog is `build/native-shaders-live`;
  the old `build/native-shaders` remains unchanged for comparison.
- Native viewport conversion handles disabled viewport scale/offset and vertex
  W/XY/Z formats. Added shared constants at 1712, 1728, 1736. Typical enabled
  viewports preserve the previous path. Tests cover disabled/pixel viewports.
- Four saved frames / 3,160 draws pass primary pulling validation: converted IA
  bytes fall from 3,280,821,632 to 91,856. Three channels of the title differ by
  one RGB unit; all other saved-frame RGB remains exact. Debug layer active,
  zero errors. Evidence: `build/test-native-primary-pulling-render.log`.
- Shader catalog derives used texture slots from actual main-body sampler use.
  It must include Cube/3D as well as 2D suffixes. Missing cube bindings caused
  dark lighting in an initial native CPU-produced frame; this is fixed.
- `frame_builder` joins owned CPU geometry/state/constants and texture tables.
- `FD_NATIVE_FRAME_CAPTURE=1` synchronizes the CPU geometry collector with the
  existing one-frame GPU capture by exact swap serial. Texture snapshots still
  come from the consuming GPU hook after Xenia full resolve readback.
- The core embeds its depth conversion shaders at CMake build time (DXC -Fh).
  In-memory rendering no longer needs replay-local helper shader files.
- The live input can be saved with `ReplayFrame::Save` for repeatable debugging.
- Final whole-path title validation PASSED using
  `tools/validate_native_textures.ps1 -NativeFrame -RunSeconds 100 -CaptureAt 60
   -OutputDirectory build\native-cpu-frame-bound-textures -Screenshot`
  with `FD_REPLAY_DEBUG=1`. 136 CPU geometry draws, 192 ordered commands, 8,080
  converted vertex bytes, 148,544 raw stream bytes, equal CPU/GPU serial 853,
  zero errors. The image compares at 51.805658 dB with zero pixels >32 error.
  All 49 textures, one cube and 178 mips match the independent raw capture.
- `native-cpu-frame-title` and `native-cpu-frame-viewport` retain failed attempts.
  A replay and native input are saved under the latter. Diagnostic
  `fd_native_frame_compare` can substitute GPU state/constants/geometry or omit
  unmatched records. The two extra CPU BeginVertices draws are real stencil
  operations; omitting them did not fix lighting. Do not drop them to force a
  draw-count match. Cubemap binding was the actual luminosity issue.

## Still required to complete the user's goal

1. Persistent renderer resources and targets/resolves across frames; current
   `Renderer::RenderFrame` owns most GPU resources locally and uploads anew.
2. CPU-side live texture versioning with GPU-resolve authority, no dependency
   on Xenia consuming hooks/readbacks. The existing write-watch memory shim in
   `xenia_bridge.cpp` can serve as generic physical-memory tracking. Its lock
   order (Xenia global region before watch mutex) must remain intact.
3. Continuous CPU frame production, preserving completed vertex layouts and
   inline BeginVertices ownership; ring wraps and every relevant draw path.
4. A native output/presentation path and backend selection. Keep PC settings,
   controls/audio/profile and existing Xenia fallback functional. Then verify
   title, introduction, gameplay, resource mutation, scene transitions and HUD.

## Implementation direction being evaluated

- Add an explicit continuous mode to FrameInput, preserving isolated replay
  behavior. Persistent render cache needs targets, depth surfaces, resolved
  color/depth snapshots, shaders/PSOs, and utility pipelines. Rebind descriptors
  each frame using fixed static-texture capacity; do not let cached descriptor
  indices refer to a destroyed descriptor set. Reset per-frame draw/clear flags,
  retain contents, and transition resources out of last frame's readback state.
- Target cache keys should include dimensions. Existing TargetKey is six packed
  source registers; extending to eight with width/height preserves std::get<0..5>
  uses. Keep per-frame target-size metadata keyed by the original six fields.
- Packed depth copies currently live in a per-frame vector; resolved maps point
  into it. They must become owned persistent destination entries, with resized
  resources retired until the current GPU fence completes.
- Native continuous resolve reads should use actual previous native copies,
  rather than the replay adapter's initial `pendingResolves` approximation.
- Native UI can reuse the BSD Xenia UI provider/presenter/ImGui without creating
  a Xenia GPU graphics system. Refactor settings_ui from GraphicsSystem to
  provider/presenter inputs. Native command execution can coexist with the
  small runtime PM4 service for fences/interrupts (not Xenia draw translation).
- For direct GPU presentation on the same device, plume can wrap the UI
  provider's ID3D12Device. D3D12Device's constructor can accept an optional
  existing device, select its matching adapter LUID, and QueryInterface instead
  of D3D12CreateDevice. Then native output can be blitted into
  D3D12Presenter::RefreshGuestOutput (its destination is R10G10B10A2). Respect
  provider direct queue and resource/fence synchronization. Avoid claiming
  native play by merely showing an Xenia-produced image.

## Build and relevant files

Workspace: the repository root, game in `game\`.
No applicable AGENTS found. Do not spawn agents (developer forbids unless user
or AGENTS explicitly requests delegation). Git worktree contains many earlier
uncommitted changes; preserve them. No commits or destructive cleanup requested.

Runtime build under vcvars64:
`cmake --build build --target FromDust -j 6`
Standalone diagnostics:
`cmake --build build/fd_replay --target fd_native_geometry_tests
 fd_native_geometry_unit fd_native_state_unit fd_native_shader_tests
 fd_native_frame_compare fd_replay -j 6`
Shader tool:
`cmake --build XenosRecomp/build --target XenosRecomp -j 6`
`python tools/build_native_shaders.py --out build/native-shaders-live --jobs 6`
Use the VS BuildTools vcvars64.bat described in existing build_all.ps1.

Runtime executable was rebuilt and no diagnostic game process remains running
after the successful bounded validation. Update the main native-renderer and
validation docs with verified final results as work continues; this file is an
in-progress handoff, not completion evidence for the remaining integration.
