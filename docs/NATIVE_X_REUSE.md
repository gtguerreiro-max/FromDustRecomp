# Reusing NativeXRenderer in From Dust

Reviewed 2026-11-03 at `5aa5a05ce8bdc34d260777ac067ee83ecb91afcd`.
The read-only research checkout is `research/NativeXRenderer` (ignored).

Current user priority: correct and validate the base renderer first. DLSS, DLAA
and DLSS 5 research is retained for a later phase; none is enabled or integrated.

NativeXRenderer is useful source code for this project. Its own code is MIT;
adapted code must retain its copyright/license. Its NVIDIA NGX/DLSS dependencies
have separate terms in its THIRD_PARTY_NOTICES.md. No DLSS implementation has
been imported or enabled in From Dust yet.

| Existing component | Reuse in our renderer | Required adaptation |
| --- | --- | --- |
| `src/upscaler.cpp/.h` | DLAA/DLSS inputs, jitter, history resets, quality modes, depth conventions, camera motion pass | Diligent/Vulkan resources and commands become Plume/D3D12 resources and NGX calls; translate the small GLSL passes to HLSL |
| `src/motion.cpp/.h` | Match objects between frames, retain prior vertex constants, distinguish camera changes from object motion | Replace its Diligent format type; use our draw/buffer identities |
| `src/spirv_patch.cpp` | Method for evaluating current and prior vertex positions | This code patches SPIR-V; our XenosRecomp/DXIL path needs an equivalent shader variant, not a direct binary patch |
| `BeginView` / `EndViewGeometry` | Explicit main-scene boundary and insertion before post-processing/HUD | Identify the corresponding From Dust engine calls and its unjittered view-projection matrix |
| `src/targets.cpp`, `src/observer.cpp` | Surface formats, lifetime/alias handling, checking reconstructed state against the actual command processor | Retain the game's already mapped XDK layout and our Xenia register capture |

The core expects a modified ReXGlue SDK and Diligent Vulkan, while this project
uses XenonRecomp, Xenia and Plume D3D12. Linking the whole library would require
a larger runtime/backend migration. Port the components above into the current
renderer first, rather than treating the library as a drop-in dependency.

Practical order:

1. Finish exact-event colour/depth/state comparisons and move the validated replay
   resource handling to live engine hooks.
2. Mark the main view and its post-processing boundary. Retain colour, depth,
   matrices and previous-frame resources with explicit lifetimes.
3. Port camera motion and jitter; validate the vectors before enabling DLAA.
4. Port object history and motion shader variants. From Dust also deforms water
   and terrain through sampled heightfields: previous matrices/constants alone
   cannot reconstruct that motion. Preserve their previous heightfields too.
5. Add DLAA first, then DLSS at a reduced scene resolution while keeping UI and
   post-processing dimensions consistent.

Upstream currently treats its four DLSS upscaling modes as experimental (D53);
DLAA is exposed more broadly. Memory export is disabled in its shader translator
and unsupported in its draw path. A subsequent independent Xenia microcode
analysis found **zero memory-export shaders among our 554 catalog entries**
(`build/native-shaders/capabilities.json`), so it is not an established blocker
for these assets. This does not establish coverage of unknown runtime programs.
Neither capability should be assumed solved merely by importing the library.

## Base integration follow-up, 2026-11-03

Rechecked the upstream integration guide and state/observer design. The useful
sequence is to intercept D3D calls, verify reconstructed inputs against the
reference backend, then activate native rendering. BeginVertices data is read
only after commitment, and queued commands own their data. Our implementation
now has typed in-memory frame inputs and an owned live geometry batch, retaining
the already validated Plume/DXIL core. It does not import a second runtime/RHI.

The native geometry decoder matches all 3,160 saved-frame draws byte for byte.
A live scene snapshot captured 615 geometry packets with zero conversion
failures. Full live render state, texture production, persistent cross-frame
resources and native presentation remain separate required work.

Primary sources: [repository and license](https://github.com/ElGuslml/NativeXRenderer),
[upscaler](https://github.com/ElGuslml/NativeXRenderer/blob/main/src/upscaler.cpp),
[motion history](https://github.com/ElGuslml/NativeXRenderer/blob/main/src/motion.cpp),
[integration](https://github.com/ElGuslml/NativeXRenderer/blob/main/docs/integrating-a-game.md),
[decisions D46–D49 and D53](https://github.com/ElGuslml/NativeXRenderer/blob/main/docs/decisions.md).
