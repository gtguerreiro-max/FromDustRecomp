# Repeatable renderer diagnosis

Compare one saved frame repeatedly. A new five-minute game run is needed only
when the existing capture lacks data; shader/state changes can use its replay.

1. Pack with `python tools/replay_pack.py --capture-dir <capture> --out <replay>`.
   Converted vertex buffers are cached per address/layout during packing.
2. Run `./tools/validate_replay.ps1 -ReplayDir <replay> -Reference <reference.png> -CaptureDir <capture>`.
   It rejects failed executions and stale output PNGs before comparing images.
3. `validation-passes.json` compares memory snapshots heuristically. Even the last
   level-zero copy may not be the version saved at first use; it does not prove
   where a divergence started. Use the exact-event RDC comparison below.
4. Check the inputs and constants of that pass before changing later lighting
   or post-processing. FDX2 GPU register records retain both float constant banks.
   Old FDXR register dumps still load, using the captured device constants.

Tool research (2026-11-03):

- [Xenia GPU traces](https://github.com/xenia-project/xenia/blob/master/docs/gpu.md)
  support capturing one frame and replaying it with xe-gpu-trace-viewer. This is
  the next useful reference capture path when CPU hooks lack initial GPU state.
- [RenderDoc replay scripting](https://github.com/baldurk/renderdoc/blob/v1.x/docs/python_api/python_module.rst)
  exposes replay analysis. The distributed Windows package does not include a
  standalone Python module; it must be built for a matching Python version.
  Our installed RenderDoc 1.47 captures native replay through renderdoccmd.
  `tools/rdoc/native_bindings.py` inspects its converted XML without a GUI.
  The C++ replay inspector now also opens RDC files directly and exports any
  event's colour target. Built and verified against the official 1.47 API:
  `build/renderdoc-sdk-v1.47/replay_inspect.exe <capture.rdc> [<event> <output.png>]`.
  Omitting the event lists actions. The title export at event 2109 matches
  target_12.png pixel for pixel. Rebuild in an x64 Visual Studio developer shell
  with `tools/rdoc/build_inspector.ps1 -DownloadHeaders` (headers stay in build/).
  Use an output filename ending in `.dds` to preserve float data instead of
  clamping it to PNG. The first gameplay simulation is captured separately at
  `build/rdoc/gameplay-simulation/native_capture.rdc` (events 52, 63 and 79);
  its event-52 float target was successfully exported to `first-pass.dds`.
- [PIX pixtool](https://devblogs.microsoft.com/pix/pixtool/) exports events and
  render/depth buffers and supports replay with D3D validation. PIX is not
  installed in the standard location on this machine; it has not been tested.
- [NativeXRenderer](NATIVE_X_REUSE.md) provides useful renderer, motion-history
  and DLAA/DLSS code. The linked assessment lists concrete reusable components
  and the adaptations needed for our D3D12/XenonRecomp runtime.

## Exact-event automation

Record the CPU frame and RDC together with `renderdoccmd capture -w`, launching
FromDust with `FD_FRAME_CAPTURE`, `FD_EXIT_SECONDS` and `FD_HEADLESS=1`. The runtime
explicitly starts/ends RenderDoc on the matching GPU swaps. Optional
`FD_CAPTURE_MIN_INDEXED=900` delays capture until a preceding frame has at least
900 indexed calls, avoiding the low-complexity menu/underwater transition.
This is a scene-complexity gate, not a general scene identifier. Keep all binary
capture files and the RDC together; never mix frames with the same draw counts.

Packing now writes `replay-index.json`, including a CPU capture digest. Run the
native replay successfully, then:

```powershell
python tools/rdoc/compare_passes.py <matching.rdc> --capture-dir <capture> --replay-dir <replay>
```

The inspector indexes recorded PSO bindings without replaying every draw just to
read its name. Shader microcode hashes and original draw counts establish a
unique ordered join. Ambiguous or incomplete joins are rejected. `--allow-prefix`
can explicitly compare only the verified prefix; unmatched later draws are
reported and no checkpoints beyond them are compared. A successful partial
diagnosis is not a full-frame fidelity pass.

Buffers are exported in one replay session as DDS, retaining signed/float data,
the actual RTV cast of typeless resources, MRT slots and atlas copy regions.
`validation-rdoc.json` lists checkpoint metrics and the first one above the
chosen threshold. Its normalized fixed16 metrics use the Xenos [-32,32] range.
The report excludes depth copies and targets without a known preceding draw.

To inspect shader inputs, use:

```powershell
build/renderdoc-sdk-v1.47/replay_inspect.exe <capture.rdc> --inputs <event> <directory>
```

This exports the textures accessed by that event, including their mip chains.
It made the BC1 palette bug directly reproducible: original compressed bytes
matched Xenia exactly, but the former decoder selected the wrong palette entries.
BC2/BC3 four-colour mode and BC5 channel decoding are covered by synthetic blocks.

New captures also write `gpu_draws.bin` (actual host submissions) and `gpu_mips.bin`
(2D mip banks with layouts from Xenia's texture utilities). Archive both alongside
the other capture files. Their register-stream hash rejects stale metadata.
Repacking writes FDR9 plus `mips.rgba`; the native reader retains older formats.
After changing XenosRecomp, use `tools/regenerate_replay_shaders.py --replay-dir
<replay>` before repacking. Shader/container numbers are preserved.

Use `-MinimumPsnr`, `-MaximumOutlierPercent` and `-RequirePassAgreement` with
`validate_replay.ps1` to enforce an image/checkpoint gate. Image dimensions must
match, a fresh successful native run is required, and `validation-image.json`
retains exact outlier counts rather than rounded percentages. Prefix comparisons
remain useful diagnostics but cannot pass the complete-checkpoint requirement.
Unknown colour-copy sources also fail that requirement; only explicit clear-only
copies may be excluded. Missing native readbacks fail the comparison directly.
With `FD_REPLAY_DEBUG=1`, D3D12 error/corruption messages make native execution
fail before writing the validation images.
Missing/invalid native shaders or failed pipelines also fail execution without
requiring the debug layer. Texture decode errors and unsupported 3D uploads fail
packing explicitly, with their source address and format, rather than substituting
white or one 2D slice.

Source identity matters: resolve packing reads bound D3D surface objects rather
than potentially temporary CPU command registers. Native depth resolves select
surface/depth storage independently of colour attachments. The independent
`gameplay-bright-complete` capture caught both errors; keep it in regression
checks alongside `base-submission-mips` and `title-base-complete`.

The native renderer is still an offline diagnostic. Image fidelity and diagnostic
capture FPS must not be presented as normal gameplay performance or live integration.

## Localizing a shader difference

The inspector can find the draw affecting one pixel without stepping an entire
shader:

```powershell
build/renderdoc-sdk-v1.47/replay_inspect.exe <capture.rdc> --pixel-history <event> <x> <y>
```

`--inputs` also exports accessed constant buffers. Xenia packs used float
constants; compare them against the corresponding used register indices, not
the native buffer's unused leading registers. `--textures` reports actual MSAA
sample counts. Full shader stepping has a 20-second/step bound because replay
debugging may not terminate even for a frame that rendered successfully.

In the Breath capture, history at event 6849 / pixel (250,210) identified event
6230, terrain PS146. Constants matched, all compared texture mip texels and cube
faces matched, and the remaining large lighting error came from `SETp_INV`.
The generated scalar must be zero for input 1, with a true predicate. The new
generated-expression regression checks this independently of saved game data.

## Display images and alpha

Use `frame.png` for the displayed final frame. The last colour-copy source is
exported as opaque RGB; its compatibility `target_<n>.png` is also made opaque.
Intermediate targets and raw resolve data retain their alpha channels.
The game's final alpha contains post-processing data, not window opacity.
Previously every final PNG pixel was transparent: on a black background the
Breath frame's mean RGB fell from 56.7 to 18.8 just through compositing.

`validate_replay.ps1` now defaults to `frame.png` and requires both presentation
images to be opaque. Numerical RGB comparison alone did not catch this display
bug. `build/native-reference-opaque-comparison.png` contains a direct, unscaled
reference/native comparison; the renderer's RGB values were not changed.

## Reusing the renderer in the runtime

`runtime/gpu/native/Renderer` is shared by the CLI and the optional game build
(`-DFD_NATIVE_RENDERER=ON`). Run `fd_replay --repeat 2 <dir1> <dir2> ...` to test
resource retirement and descriptor reuse across successive frames on one device.
`FromDust.exe --native-replay <dir1> ...` exercises the same code in the runtime
before guest workers start. It is still saved-frame rendering, not live hooks.

Check debug activation itself. Earlier code silently continued after
`D3D12GetDebugInterface` failed, so an empty message log was insufficient evidence.
`FD_REPLAY_DEBUG=1` now requires successful layer activation and an info queue.
The development PC initially returned `DXGI_ERROR_SDK_COMPONENT_MISSING`.
Windows Graphics Tools was installed with the user's UAC approval using
`tools/install_graphics_tools.ps1` (DISM `/NoRestart`); installation reports
`Installed` and no restart required. Run the image/RDC gates with
`FD_REPLAY_DEBUG=1` and check `native-report.json`: `debug_requested` and
`debug_active` must both be true, and `debug_errors` must be zero.
Warnings remain visible and counted; an empty log alone is never sufficient.

Microsoft documents the matching runtime/debug layer and Windows component
requirements in the [D3D12 redistributable specification](https://microsoft.github.io/DirectX-Specs/d3d/D3D12Redistributable.html#d3d12-debug-layer).

## Native shader catalog and live registry audit

```powershell
python tools/build_native_shaders.py
build/fd_replay/fd_native_shader_tests.exe build/native-shaders/catalog.bin
$env:FD_REPLAY_DEBUG = '1'
tools/validate_replay.ps1 <normal capture arguments> -ShaderCatalog build/native-shaders/catalog.bin
tools/validate_native_live.ps1
```

The catalog rebuild is incremental; it includes current native bytecode and
complete original virtual/physical identities. Never match only a physical
prefix: multiple extracted shaders share code but differ in metadata.
The runtime hook observes initialization before vertex fetch patching and
invalidates stale bindings when an object address is reused.

Live registry coverage describes complete CPU frames; the game continues using
Xenia for rendering. `shader_coverage_passed` alone can describe an intro screen,
so the live launcher additionally requires indexed calls and the configured
number of calls in the last complete frame (500 by default). It fails if the
catalog is incomplete or the required scene was not reached. Diagnostic timing
and engine call totals are not gameplay performance measurements.

## Memory input and geometry ownership

```powershell
build/fd_replay/fd_native_frame_tests.exe build/native-shaders/catalog.bin <four replay directories>
python tools/prepare_native_geometry_tests.py <capture directory> <replay directory>
build/fd_replay/fd_native_geometry_tests.exe build/native-shaders/catalog.bin <capture> <replay> [<capture> <replay> ...]
build/fd_replay/fd_native_geometry_unit.exe
tools/validate_native_live.ps1 -RunSeconds 315 -GeometryCaptureAt 280
```

The memory test checks returned RGB against the existing validated native PNGs;
the core receives spans and writes no frame files. Keep their backing storage
immutable until `RenderFrame` returns. The geometry fixture tool references
existing capture buffers and preserves the inline fetch selected at commitment.
Geometry tests compare every converted byte with the validated replay, including
pulled stream contents after adjusting their arena offsets.

Live geometry capture keeps an owned batch and reports its limited scope: one
final engine draw packet per call. It does not enable native gameplay. A command
range wrap or incomplete input is a failure, rather than a guessed geometry path.
The pending producer must expand full calls and add verified state/textures.

Replay validation also runs `tools/validate_effect_regions.py` for small visible
effects hidden by a whole-frame average. Regions in `tests/native_effect_regions.json`
are pinned to CPU capture and reference RGB hashes. Unknown captures explicitly
have no region coverage. Keep the title's left shaft: it checks attenuation as
well as presence of the texture mask.

Pixel shader stepping accepts an optional primitive ID after X/Y to select the
fragment from pixel history. Tracing is bounded to 10,000 states and 20 seconds,
including within returned batches. Incomplete traces fail explicitly.
