# PC settings validation — 2026-11-01

The Clang Windows build completed successfully. `RuntimeSettingsInput` passed
through CTest, covering settings reload and invalid values, key swaps and reserved
shortcuts, keyboard button/axis translation, bounded diagonal movement, blocked
input, controller-index errors, the Xbox big-endian layout and an empty event
queue.

Interactive checks in the running game window:

- F1 opened and closed the Portuguese overlay over the animated title screen.
- Showing FPS wrote `ShowFPS=1` to the isolated test INI.
- Start was changed from Enter to P. The new assignment survived a restart;
  pressing P with the panel closed reached the game's local-profile request.
  That original profile/sign-in flow is still a stub, so no authentication or
  gameplay progression was tested.
- F11 switched to borderless fullscreen and back to the original window size.
- The restart button launched a new process and restored saved settings.
- The corrected 30 FPS limit produced steady 30 FPS samples in the title scene.
- FSR presentation changed immediately. The live window size changed from
  1280x720 to 1600x900 without interrupting rendering.
- The controls list scrolled while the tab bar stayed visible.

Evidence: `build/settings-panel.jpg`, `build/validation-settings.ini`,
`build/validation-settings-ui.log` and `build/validation-settings-ui-gpu.log`.
The test INI was separate from the user's normal `build/user/settings.ini`.

A separate 65-second offscreen run with Scale=2 produced a visually verified
2048x1200 title scene in `build/settings-scale2.png`, with no recorded crashes.
Logs: `build/validation-settings-scale2.log` and the matching `-gpu.log` file.
The 3× scale and physical controller behavior still need testing; audio and full
gameplay remain outside the completed settings-panel work.

## Native replay UI — 2026-11-02

The existing title-screen capture now renders its logo and "Press START" text
with the real shaders. Mixed 16/32-bit index uploads are padded to four-byte
boundaries; shader init matching uses the physical section's actual length and
stage, correcting 20 of 557 mappings (including the UI pixel shader).

- Standalone fd_replay Release build with clang-cl: passed.
- `python -m unittest discover -s tests -p test_replay_pack.py -v`: five passed,
  covering short shaders sharing a prefix, metadata false matches, shader stage,
  the 1024-byte capture limit and unmatched code. The cases contain synthetic data.
- Repack: 185 records, all 45 required shaders compiled. Native GPU replay: passed.
- Full-image RGB PSNR against `build/xenia_title.png`: 17.05 → 18.44 dB.
  Logo region (380,35)-(600,155): 9.68 → 25.12 dB; text region
  (325,350)-(700,390): 13.35 → 19.98 dB. These measure the existing captured
  title frame, not gameplay or frame rate.
- Local Swift/Qwen reviewed the index allocation and offset arithmetic and
  approved it. Its zero-sized-draw caveat used a simplified snippet; the actual
  renderer already limits the draw count to the number of supplied indices.

Evidence: `build/replay/ui-before.png`, `build/replay/target_12.png`,
`build/replay/pack-ui.log`, `build/replay/build-ui.log`,
`build/replay/replay-ui-aligned.log`, `.local/qwen-index-review.txt` and its
matching JSON statistics. Diagnostic shader replacements were restored before
the final replay. Scene blur, dark detail and colored light artifacts remain;
fresh gameplay capture is still required.

## Native replay follow-up — 2026-11-03

This supersedes the earlier UI-only result. The native Release renderer builds;
22 synthetic replay regressions pass. The final title capture has 134 draws,
47 resolves and nine clears; all 45 shaders compile. The validation script
checks the native exit code and requires a freshly written image. The title
compares at 29.71 dB RGB PSNR, with 1.6% of pixels differing by more than 32.
RenderDoc has verified four-byte index-view alignment for all 134 indexed draws.

The real gameplay capture reaches the first scene with characters and the Breath
hint. All 1,138 actual draws now align, and all 82 required shaders compile.
Colour/depth pass identity, partial atlas copies, mip destinations and D32S8
stencil are retained. The native execution exits successfully, but its 12.66 dB
image still has major water/terrain differences, so the gameplay fidelity gate
has not passed. The first divergent height copy is now identified automatically.

Evidence: `build/title-final-replay/validation-native.log`,
`build/gameplay-final-replay/target_12.png`, `build/gameplay-reference.png`,
`build/gameplay-pass-comparison.json`, and `build/rdoc/title-final/bindings.json`.
The new FDX2 capture adds actual GPU float constants and has also been rendered
successfully: 12.17 dB against its own synchronized reference. First-pass constants
agree with the device copy, so they do not account for the heightfield discrepancy.
The standalone RenderDoc C++ inspector builds and opens RDC files successfully;
its title export at event 2109 matches the native target image pixel for pixel.
See `docs/RENDER_DEBUG_WORKFLOW.md` for repeatable commands and researched tools.

## Exact-event validation — 2026-11-03 (supersedes the first-height diagnosis)

The previous memory comparison did not establish an exact intermediate version.
The new RenderDoc reference exports actual event RTVs and shader inputs. Original
BC1 bytes matched the GPU; the replay's palette decoder was wrong. BC1, BC2/3 and
BC5 decoding, fixed16 SNORM storage and explicit texture LOD now have corrected
implementations. Thirty-three synthetic replay checks pass. The full gameplay
frame builds/renders successfully with all 82 shaders; two matched saved frames
compare at 30.03 dB (1.6% pixels >32) and 29.71 dB (1.8%). The matched title frame
compares at 33.50 dB (0.2%). No DLSS/DLAA is involved.

The automated RDC join verifies a unique ordered prefix of 1,067 gameplay draws
and compares 24 colour checkpoints. It stops before two PS183 draws absent from
the reference action stream; later draws are not certified by that report.
Remaining water edges, static mips and conditional/3D-texture handling still need
base-renderer work. NativeXRenderer reuse is assessed in `docs/NATIVE_X_REUSE.md`;
temporal enhancement work is deferred until the base is correct.

## Base gate completed on the saved frame — latest 2026-11-03 result

The new capture has complete host-submission metadata, 30 static 2D mip chains
(245 levels) and a hash binding them to the GPU register stream. All 984 submitted
game draws align; all 41 exported colour checkpoints have normalized MAE below
0.01. The final image compares at 39.16 dB, with 63/614400 pixels over the 32-level
RGB difference threshold (0.010254%). D3D12 debug output is empty.

Further root causes fixed: missing blend constants (32/255 in the smoothing
pass), shorter static index snapshots discarding triangles, and absent host depth
clipping in the fourth shadow cascade. Required shaders must compile and complete
index data must exist; silent partial rendering is rejected. Forty synthetic replay
checks pass. See the current validation command in `docs/NATIVE_RENDERER.md`.

Evidence: `build/validation-base-gate.log`,
`build/base-submission-mips-replay/validation-image.json`,
`build/base-submission-mips-replay/validation-rdoc.json`, and
`build/compare-base-depth-clip.log`. This validates saved-frame parity, not live
runtime integration or full-game coverage. Enhancement work remains deferred.

## Independent capture regressions — 2026-11-03

The later first-scene capture exposed depth copies selected by unrelated colour
attachments and resolve sources taken from temporary/invalid CPU command state.
Depth copies now use surface/depth identity; packing uses bound source surfaces.
The ice formation and terrain return, improving that capture from 20.53 to
35.73 dB. All three fresh captures were repacked and validated after the fixes:

- `base-submission-mips`: 39.16 dB, 63/614400 pixels over 32, 984 draws and 41
  exact-event checkpoints verified.
- `gameplay-bright-complete`: 35.73 dB, 497/614400 pixels over 32, 1,084 draws and
  41 exact-event checkpoints verified.
- `title-base-complete`: 34.37 dB, 68/614400 pixels over 32, 134 draws and 40
  exact-event checkpoints verified.

No D3D12 messages, unmatched draws or unexplained colour-copy exclusions occur
in these runs. Each frame has one explicit clear-only copy excluded. Missing
readbacks and unexplained exclusions can no longer silently reduce coverage.
D3D12 errors/corruption make native execution fail. The native build and all 44
synthetic replay tests pass. Evidence: `build/validation-base-surface-regression.log`,
`build/validation-bright-final-gate.log`, `build/validation-title-final-gate.log`,
and the matching replay JSON reports. These remain offline checks, with both
gameplay frames from the introductory scene; live integration is still pending.

The additional four synthetic checks reject unknown formats, 3D textures treated
as 2D, and fixed16 decode failures falling back to RGBA8, while retaining valid
texture pixels. Negative integration checks also reject a missing native readback,
an unverified colour-copy source and a missing pixel shader. Their diagnostic
files/index were restored and the base positive gate passed again; evidence is
`build/test-missing-readback-rejection.log`,
`build/test-unverified-source-rejection.log`,
`build/test-missing-shader-rejection.log`, and `build/validation-base-final-gate.log`.

## Four-capture base regression — latest result

The elevated Breath tutorial failed the original image gate at 34.67 dB and
0.5662% outliers. The root cause was `SETp_INV` retaining scalar 1 when the
predicate became true, changing terrain shadow-cascade selection. The fixed
translation is tracked in `patches/xenosrecomp.patch`. Native 2x MSAA, colour
resolves and selected/quantized depth samples are also retained.

After regenerating all 93 required sources and repacking all four captures:

- `base-submission-mips`: 39.41 dB, 20 outliers, 984 draws / 41 checkpoints.
- `gameplay-bright-complete`: 36.03 dB, 409 outliers, 1,084 draws / 41 checkpoints.
- `title-base-complete`: 34.37 dB, 45 outliers, 134 draws / 40 checkpoints.
- `gameplay-breath-complete`: 40.24 dB, 505 outliers, 958 draws / 41 checkpoints.

Every image is 1024x600; outliers exceed 32 in at least one RGB channel. All
explicit image/checkpoint gates pass, without D3D12 messages, unmatched draws or
unverified colour resolves. Evidence: `build/validation-<capture>-predicate.log`
and each replay's `validation-image.json`/`validation-rdoc.json`.

`tools/test_predicate_codegen.ps1` compiles and executes the actual generated
expression for synthetic microcode, checking inputs 0, 1, 2, 0.5, -1 and two
boolean round trips: seven cases pass. Forty-four Python replay tests pass.
Native and XenosRecomp builds succeed, both dependency patches reverse-check,
and `git diff --check` passes. This broadens first-level saved-frame coverage;
live integration and later levels remain pending. Enhancements remain deferred.

## Opaque presentation regression

The phone-preview brightness report exposed final PNG alpha used as window
opacity. The native Breath PNG had mean alpha 73.9/255, while the synchronized
reference was fully opaque. Compositing on black lowered the native mean RGB
from 56.7 to 18.8. Export now writes `frame.png` as RGB and makes the matching
final target PNG opaque; raw/intermediate alpha is preserved.

All four captures pass again with opacity enforced and unchanged RGB metrics:
`build/validation-<capture>-opaque.log` (Breath: `validation-breath-opaque.log`).
RGB bytes of `frame.png` and the corresponding final target agree. Three new
checks cover transparent rendered/reference images with matching RGB and valid
RGB/opaque RGBA inputs; all 47 Python replay tests pass. The generated predicate
test's seven cases also remain valid.

## Shared core and debug-layer audit

The CLI and optional game-runtime diagnostic now link the same renderer library.
Two alternating cycles of all four captures complete eight frames / 6,320 game
draws on one D3D12 device. All four RGB/RDC gates still pass after that sequence.
The game executable itself renders four saved frames / 3,160 draws successfully.
A 75-second normal Xenia startup also exits successfully. Four malformed input
cases reject invalid magic, truncated name/payload, truncated words and trailing
data before creating a device. The 47 Python tests pass.

**Correction to earlier debug claims:** the previous code ignored a failed
`D3D12GetDebugInterface`. Empty logs therefore did not establish a clean debug
execution. Layer activation and the device info queue are now mandatory when
`FD_REPLAY_DEBUG` is requested. Before Graphics Tools was installed, both SDK
614 and an isolated 620 SDK/local-factory experiment returned
`DXGI_ERROR_SDK_COMPONENT_MISSING`. Production remains on 614. Graphics Tools
was subsequently installed with the user's UAC approval on 2026-11-03; DISM
reports `Installed` and no restart required. Actual debug results follow below.

Evidence: `build/validation-native-eight-frames.log`,
`build/validation-runtime-native-core-stdout.log`,
`build/smoke-normal-native-linked.log`, `build/test-native-input-rejection.log`,
and `build/validation-<capture>-shared-core.log`. Live native gameplay remains
pending; the runtime entry point is explicitly a saved-frame diagnostic.

## Graphics Tools installed: active D3D12 validation

After UAC approval, the official component installed without requiring a
restart. All four complete captures pass again with `FD_REPLAY_DEBUG=1`:

| Capture | Draws | Colour checkpoints | D3D12 errors | Warnings |
| --- | ---: | ---: | ---: | ---: |
| base-submission-mips | 984 | 41 | 0 | 10 |
| gameplay-bright-complete | 1,084 | 41 | 0 | 21 |
| title-base-complete | 134 | 40 | 0 | 8 |
| gameplay-breath-complete | 958 | 41 | 0 | 20 |

Every execution report confirms `full_frame`, `debug_requested` and
`debug_active`. Image metrics remain unchanged; all 163 colour checkpoints
pass with no unmatched draws or unverified colour resolves. Warnings are
not suppressed: each capture has five later clear-value mismatches; the rest
describe float-backed placeholders in generated uint vertex inputs; the
affected fetches use shader pulling.
These runs have zero errors, not zero warnings.

Optimized clears uncovered an upstream Plume depth-clear format bug, fixed in
`patches/plume.patch`. Texture-creation failures now stop with a diagnostic.
All 47 Python tests and seven actual generated-predicate cases pass; all three
dependency patches reverse-check, and native/runtime builds succeed.

Runtime assertions caught attachment binding before its depth-state transition
on the clear path. Transitions now precede framebuffer binding for every colour
attachment and shared depth. The game executable then completes four saved
frames / 3,160 draws, and the CLI completes eight frames / 6,320 draws on one
device, with active debug validation and zero errors. These remain saved-frame
diagnostics; live native gameplay is not connected.

Evidence: `build/validation-<capture>-debug-active.log`, each replay's
`native-report.json`, `validation-image.json` and `validation-rdoc.json`, plus
`build/graphics-tools-result.json` and `build/build-runtime-debug-active.log`.
The runtime and sequence logs are
`build/validation-runtime-native-debug-active-stdout.log`,
`build/validation-runtime-native-debug-active-stderr.log` and
`build/validation-native-eight-frames-debug-active.log`.

## Compiled shader catalog and live object identity

All 554 extracted containers regenerate and compile as native VS/PS entry
points. A second build reuses all 554 cached outputs. The native C++ catalog
checks distinguish identical physical code with different virtual metadata,
code that differs only beyond the old 1,024-byte snapshot, stages and reused
object addresses. Unknown reinitialization removes the old binding. Malformed,
truncated, duplicate and trailing catalog records are rejected. All 554 actual
catalog entries round trip through complete identity and named/staged lookups.

All four RGB/RDC gates pass with `-ShaderCatalog build/native-shaders/catalog.bin`
and active D3D12 debug validation: unchanged PSNR/outliers, all 163 checkpoints,
3,160 submitted draws and zero D3D12 errors. Reports confirm catalog use.
Two alternating catalog cycles also complete eight frames / 6,320 draws on
one device with all four final PNG hashes unchanged. The final game executable
completes four saved frames / 3,160 draws with the same catalog and active debug
validation (`build/validation-native-catalog-eight-frames.log` and
`build/validation-runtime-native-catalog-{stdout,stderr}.log`).

The bounded 300-second live run spans startup and the first scene:

- 7,471 complete CPU frames, 1,190,827 engine draw calls.
- 1,088,298 indexed calls; the last complete frame has 634 total calls.
- 557 shader initializations matched; zero invalid or unknown identities.
- 75 catalog shaders used; zero unresolved draw calls.

This validates live object-to-native-asset identity. Gameplay rendering remains
on Xenia; engine call totals include BeginVertices and are not GPU draw counts
or performance measurements. A live native frame/resource producer remains pending.

Negative integration checks exercise a one-entry catalog. Live audit exits 1
with 556 unknown shader initializations and an unresolved draw; saved-frame
rendering rejects its missing shader before creating a device. Separately,
the live launcher rejects a 20-second intro-only run despite perfect shader
coverage, because no indexed scene frame was reached. All 47 Python replay
tests still pass.

## Native state and resource producers

`fd_native_state_tests` compares device reconstruction against all 3,160 saved
draws, including bool/loop constants and fetch metadata. Packet scissor inputs
come from the GPU reference in this offline test; index offset comes from the
CPU call arguments. `fd_native_state_unit` checks required inputs and bounds.
The live window-helper observer captures logical scissor bounds, which are not
yet proven to match every tiled submitted draw.

`fd_native_texture_tests` matches 264 base textures, seven cubemaps and 1,160
mip levels byte for byte, including C++ resource-table packing. Its `--render`
mode replaces all three texture tables in the four memory frames with those
rebuilt tables: 3,160 draws preserve exact native baseline RGB with D3D12 debug
active and zero errors. `fd_native_texture_unit` checks BC edge cases, endian,
signed fixed values, immutable address reuse, compatible sampler changes and
rejected unsupported/truncated inputs. Evidence: `build/test-native-state.log`,
`build/test-native-state-unit.log`, `build/test-native-textures.log`,
`build/test-native-texture-unit.log`, `build/test-native-resource-frames.log`.

The local `Testar-render-nativo.cmd` diagnostic also passes: four saved frames,
3,160 draws, fresh images/reports, shared catalog, active debug and zero errors.
It writes a standalone comparison HTML file with embedded images. It does not
claim a performance test or live native gameplay.

The revised Start/A schedule reaches the live complexity threshold. The observer owns
617 commands with geometry, device/logical-window state and both float banks;
617 state packets and zero failures. Evidence:
`build/native-shaders/live-state-scene.json`. The per-engine-call final-draw
scope and Xenia presentation remain explicit in the report. The final screenshot
shows the menu background, so the complexity threshold is not proof of playable
level coverage. Earlier low-complexity title-only runs remain rejected.

The updated runtime also builds and completes `--native-replay` for all four
frames / 3,160 draws on one device with debug active and zero debug errors.
Evidence: `build/build-native-state-textures.log` and
`build/validation-runtime-state-textures-{stdout,stderr}.log`. All 47 Python
tests pass; all three dependency patches still reverse-check cleanly.

## Missing air sparkle regression

The breath frame's synchronized GPU reference is RGB-identical to its selected
reference PNG. Pixel history localizes the missing sparkle to event 7148,
VS 182 / PS 183, reading a plain 2D texture through `tfetch3D`. The native path
now selects the actual 2D view for that instruction/fetch combination. Explicit
LOD sampling uses the same selection; volume/stacked uploads remain unsupported.

All four frames / 3,160 draws render with the rebuilt 554-shader catalog and
debug active, with no debug errors. All 163 RDC colour checkpoints pass. The
other three image metrics are unchanged; the breath frame improves from 40.24
to 40.83 dB and from 505 to 398 outliers. Its 25x25 sparkle region has maximum
error 1 and mean 0.193, versus 31 and 1.702 before. The permanent region check
rejects the old image and validates exact reference provenance. Unknown captures
are explicitly reported as having no registered effect regions.

The runtime also passes this frame with replay-local rebuilt shaders and no
catalog, covering the second shader-loading path. Evidence:
`build/validation-particles.log`, `build/validation-native-particles.log`,
`build/validation-effects-before.json`,
`build/gameplay-breath-complete-replay/validation-effects.json`, and
`build/validation-runtime-particles-{stdout,stderr}.log`. All 48 Python tests pass.

Evidence: `build/build-native-shaders.log`, `build/build-native-shaders-cached.log`,
`build/test-native-shader-catalog.log`,
`build/validation-<capture>-native-catalog.log`,
`build/native-shaders/live-gameplay-audit.json`,
`build/native-shaders/live-missing-shader-audit.json`,
`build/test-native-catalog-missing-shader.log` and `build/test-live-scene-gate.log`.

## In-memory frames and the native geometry producer

- `fd_native_frame_tests` preloads/moves all four file adapters, renders typed
  inputs with no diagnostic directory and checks exact baseline RGB. Four frames /
  3,160 draws pass with debug active. Missing draw data clears the output, fails
  before GPU submission and leaves the completion count unchanged.
- All four RGB/RDC gates pass again after extracting the file adapter:
  `build/validation-<capture>-memory-core.log`. Runtime execution also completes
  four frames / 3,160 draws (`build/validation-runtime-memory-{stdout,stderr}.log`).
- `fd_native_geometry_tests` compares C++ conversion against every validated
  replay draw: all 3,160 match vertices, indices, input semantics, fetch metadata
  and pulled stream bytes. BeginVertices fixtures use the fetch committed in
  its command region rather than its earlier device shadow.
- `fd_native_geometry_unit` checks snapshot ownership after guest mutation,
  preserving old stream bytes and reusing identical new snapshots, rectangle
  expansion and rejected missing/truncated/unsupported data.
- The 315-second live scene run completes geometry capture: 615 packets,
  zero failures, 380,017,520 vertex bytes and 3,425,700 index bytes.
- Direct device constants agree in all 37,388 used float4 comparisons against
  synchronized GPU state. XenosRecomp's embedded literals are excluded. Xenia's
  independent analyzer checked all 554 catalog shaders and found no memory export.

The geometry snapshot is scoped to the final draw packet in an engine call;
it is not a full native frame producer. The running game still uses Xenia.
State reconstruction, textures, cross-frame resources and native presentation
remain pending. Counts and diagnostic snapshot sizes are not performance data.

Evidence: `build/test-memory-frames.log`, `build/test-native-geometry.log`,
`build/test-native-geometry-unit.log`, `build/native-shaders/live-geometry.json`,
`build/native-shaders/capabilities.json` and
`build/<capture>-replay/validation-device-constants.json`. All 47 Python replay
tests still pass.

## Sky light shafts: packed base mip and conditional EXEC

The 16x64 BC3 mask's base mip is packed at X=16 texels; its corrected bytes
match RenderDoc exactly. Both texture producers handle packed level zero;
already extracted higher mips are not offset again. All four higher-mip tables
remain byte-identical. Synthetic cases cover tall/wide/square/non-power-of-two
BC textures, tiled/linear storage, bounds, and native uncompressed packed bases.

Conditional EXEC previously ran unconditionally. Boolean/predicate guards and
conditional END now use the complete 256-bit runtime boolean bank. All 554
shaders compile. Four frames / 3,160 draws and all 163 exact-event colour
checkpoints pass with debug active and zero errors. Updated image metrics:
initial 49.306323 dB / 20 outliers; bright 48.326504 / 0; title 52.631946 / 0;
breath 43.996322 / 376. These supersede earlier metrics; residual breath
differences remain. C++ rebuilt resources still render exact native baseline RGB.

Explicit shaft regions pass. The title's left region rejects both the old dim
image and the intermediate incorrect attenuation. The runtime also completes
all four frames on one device. All 49 Python tests pass. Evidence:
`build/validation-light-shafts.log`, `build/test-native-light-resources-final.log`,
`build/test-packed-base-unit.log`, `build/effects-title-before-light.json`,
`build/effects-title-before-conditional.json`, updated `validation-effects.json`
and `build/validation-runtime-light-final-{stdout,stderr}.log`.

## Native ordered operations and renderer HUD

`tools/prepare_native_operation_tests.py <capture> <replay>` prepares owned
surface/argument/copy-command fixtures. `fd_native_operation_tests --render
<catalog> <replay> ...` compares all 39 clears and 197 resolves and replaces
them in the four frames. All 3,160 draws retain exact baseline RGB with zero
debug errors (`build/test-native-operation-frames.log`). All 197 register
sources are contiguous CPU packets; none needs a GPU-state fixture fallback.
`fd_native_operation_unit` covers PM4 payload isolation, padding, write-one
headers, truncation, missing surface state and clear rectangle ownership.

The live capture owns 676 ordered commands: 615 geometry, 11 clear and 50
resolve packets, no failures. Evidence:
`build/native-operations-live/live-geometry.json`. Its engine-final-draw scope,
logical scissor and Xenia presentation remain explicit. The complexity gate
does not independently identify a playable level. `validate_native_live.ps1`
checks operation counts as well as geometry and clears HUD-open overrides.

`ctest --test-dir build -R 'RuntimeSettingsInput|RuntimeFrameTiming'
--output-on-failure` passes both checks. New interface preferences round-trip
and clamp correctly without requiring a graphics restart. Frame timing tests
verify the 240-sample ring, paced intervals, draw request deltas, P95 and reset.
The F1 settings HUD and renderer tab were checked in the game window at
1280x720. Adapter/memory data, frame graph and integration status are legible.
The 49 Python tests also pass (`build/test-python-hud-operations.log`).

## Optimized Release verification

The cached Release C++ flags were empty, leaving generated PPC and runtime
objects unoptimized. The production build script now explicitly configures
`/O2 /Ob2 /DNDEBUG`; `build/build-optimized-runtime.log` records the full rebuild.
Both runtime CTests pass again. The optimized runtime renders all four native
saved frames / 3,160 draws with debug active and no errors. RGB equals the
pre-optimization native images embedded in the comparison HTML.

Single-instance offscreen measurements on the title Start screen, with identical
settings hashes, improve from 18.7 FPS (18–19) to 39.7 FPS (38–41), using the last
ten one-second samples after profiling/readback. Both screenshots were inspected.
The first optimized run reached the menu faster and is excluded from comparison;
the matched run only dismisses the intro. This measures Xenia title performance,
not native gameplay, and does not lock animation frames/background activity.
Evidence: `build/perf-title-before/measurement.json`,
`build/perf-title-after-matched/measurement.json`, and
`build/validation-runtime-optimized-{stdout,stderr}.log`.

## First-use layout regression

The faster runtime exposed nine errors in a rendered introduction frame:
vertex declaration format/stride fields were still zero at the Begin hook.
Before/after dumps in `build/native-geometry-diagnostics` prove the XDK patches
those fields, fetch destinations and stream selections inside the draw call.
The observer now owns completed code at End, including for delayed inline
vertex commands, so a later rebind cannot change the captured layout.

`validate_native_live.ps1 -RunSeconds 185 -GeometryCaptureAt 80
-MinimumPatchedShaders 1 -Screenshot` passes. It captures nine layout patches,
711 geometry packets, 11 clears and 50 resolves (772 ordered commands), with
zero failures. Pulled streams total 1,230,480 owned bytes. No candidate frame
was discarded in this run. Patch coverage is required, not inferred from an
otherwise successful frame. Valid frames without the requested coverage can
be skipped; an erroneous frame is always terminal. The final scene image is
for scene identification and is not asserted to match the geometry frame.

Evidence: `build/native-first-bind-verified/live-geometry.json`,
`build/test-native-first-bind-verified.log`, and `scene.png` in that directory.
The earlier nine-failure runs and the intermediate run rejected for zero patch
coverage remain separate evidence. All 3,160 saved geometry draws still match
their exact baseline bytes; both runtime CTests also pass. This is native input
producer validation. Interactive rendering still uses Xenia.

## Native texture memory acquisition — 2026-11-04

The new C++ producer derives texture storage from fetch descriptors and reads
owned base/mip banks through a memory callback. All 264 base textures, seven
cubes and 1,160 mip levels across the four saved captures match the independent
reference bytes. Native mip pitches, offsets, packed positions and bank extents
match the GPU layout fixtures. Rendering all four frames / 3,160 draws from the
production memory-reader API's tables retains exact baseline RGB, with the
D3D12 debug layer active and zero errors. Existing input-signature warnings
remain. Evidence: `build/test-native-memory-textures-render.log`.

`tools/validate_native_textures.ps1 -Screenshot` passes on the rebuilt runtime.
Its 120-second run captures the title screen and invokes the native producer
inside the GPU texture-consuming hook after full resolve readback. All 49 base
textures, one cube and 178 mip levels exactly match independently decoded raw
banks from the same register stream. The synchronized screenshot was inspected.
Evidence: `build/native-texture-live-final/validation-textures.json`,
`native-texture-report.json`, `scene.png` and the logs in that directory.

This is first-use-per-address acquisition, not per-draw resource versioning or
native interactive rendering. The producer still uses the Xenia consuming hook
for this diagnostic; CPU snapshot timing alone is not asserted to be sufficient.
The repeated live descriptor path skips unchanged resource layouts. Normal
launchers clear the diagnostic flag. The earlier successful capture is retained
separately in `build/native-texture-live-title`.

The C++ ownership checks pass, including shared packed bank reads, mutation,
pitched linear images, linear BC alignment, bounds and unreadable banks
(`build/test-native-texture-storage-unit.log`). All 54 Python checks pass,
including rejecting stale register bindings, failed/incomplete reports and wrong
native pixels (`build/test-python-native-textures.log`). Both runtime CTests pass
again (`build/test-runtime-native-textures.log`).

## Native physical aliases and volume textures (2026-11-04)

Four runtime CTests pass, including `RuntimeMemoryAliases`: GPU query reports
written via A are visible via C, E has its physical +4 KB offset, page protection
is shared, and private image / MMIO memory remains independent. The original
XDK query consumer now reads nonzero completed intervals without an override.

`fd_native_texture_unit` covers the actual tutorial BC3 128x128x32 fetch, all
slice contents, truncated storage, tiled volume bank boundaries and acquisition
ownership. `fd_native_volume_tests` renders every texel of a three-slice volume
through a genuine Texture3D SRV. It checks row padding, persistent reuse,
component swizzle, saved replay reload and truncated sidecar rejection under
the D3D12 debug layer (`build/native-volume-render-tests.log`). Base volumes are
supported; volume mip tails and stacked arrays remain explicit unsupported cases.

The four saved frames still match all 3,160 baseline draws with exact RGB
(`build/native-volume-regression-render.log`). Eight-frame GPU history checks
pass (`build/native-volume-history-tests.log`), the full 554-shader catalog passes
identity / named lookup checks, and all 54 Python tests pass
(`build/native-volume-python-tests.log`).

The first windowed tutorial test reached the textured sphere / characters but
failed on the then-unsupported volume (`native-session-navigation-profile`).
Two subsequent successful headless sessions stayed in the title backdrop and
must not be called gameplay validation. The current windowed volume validation
is recorded separately in `docs/NATIVE_LIVE_PROGRESS.md`; completed runtime
reports and an inspected tutorial image are required before claiming success.
