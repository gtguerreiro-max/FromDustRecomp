# Online investigation — 2026-11-04

The user explicitly requested online help after the continuous native renderer
continued showing blue silhouettes / missing surface detail during transitions.
The sources guided protocol and memory checks. The physical-alias defect below
is now demonstrated locally; first-level visual validation is still pending.

## Physical memory aliases: demonstrated cause

Tracing the original XDK `D3DQuery::GetData` consumer showed completed queries
returning zero even when the native PM4 service wrote nonzero reports. Its
readback pointer uses C, whereas GPU reports are written through A. The runtime
previously allocated independent storage throughout the 4 GB guest interval.

[Xenia's memory implementation](https://github.com/xenia-project/xenia/blob/master/src/xenia/memory.cc)
maps A and C to the same physical section and E at physical offset +4 KB.
The runtime now uses that mapping too. Windows
[MapViewOfFile3](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile3)
supports the required page-aligned section offset when replacing a placeholder.
Physical write watches protect all aliases, and physical-address / allocation
lookups canonicalize them consistently.

The unmodified consumer now returns 100 for completed query intervals. The
temporary diagnostic consumer override was removed. Runtime tests verify GPU
report writes through A are visible to C, bidirectional A/C/E writes, E's offset,
shared protection, private image / MMIO regions and aliased deallocation. All
four runtime CTests pass. The 4,207-frame physical-alias session has no D3D12 /
geometry errors and restores full color in the title transition. A longer
first-level validation is in progress; this is not a claim of complete gameplay.

## Volume effect after creating the Breath

The windowed tutorial run later failed on an actual texture, rather than a
speculative shader feature: VS 182 binds slot zero as linear BC3 **128x128x32**,
fetch `01024802 058C6054 07C3F87F 00A80D10 00000003 00000418`.
The old decoder explicitly rejected dimension 2. Base-level native volume
ownership, decoding, 3D upload and swizzled views are now implemented. It has no
mip bank, packed tail or stacked-array flag.

Primary local references are Xenia's `xenos.h::xe_gpu_texture_fetch_t`,
`texture_util.cc::GetGuestTextureLayout` and `texture_address.h::Tiled3D`.
3D size fields are 11/11/10 bits; slice strides retain aligned block rows.
The tiled-address implementation carries Xenia's BSD attribution. Tests cover
the observed BC3 image, bank / slice boundaries and every GPU-sampled pixel of
a small nonuniform volume, including replay round trip and swizzle. This does
not claim support for volume mip chains, packed volume tails or stacked arrays.

## Native PM4 protocol defects found during follow-up

The local primary Xenia definitions exposed three errors in native ZPD query
completion: `RB_SAMPLE_COUNT_ADDR` was 0x2311 instead of **0x2325**, the code
wrote only eight big-endian bytes instead of eight little-endian words, and a
constant result could not represent a cumulative sample counter. The native
fallback now writes all 32 bytes, splits the visible count between A/B lanes,
and advances it for every completed report, following Xenia's conservative
fake-query path. This also keeps deferred survey intervals visible. Completion
lanes are written last. This is not real hardware occlusion counting.
Runtime logs now show actual report destinations (for example 0x02385C60)
where the previous implementation always read zero. The new CTest checks query
interval subtraction, conservative deferred intervals, endian layout and
32-bit counter wrap. Earlier draw-count-based fallback traces should not be
mistaken for the current per-report implementation.

References in the bundled primary source: `xenos_zpd_report.h`,
`xenos.h::xe_gpu_depth_sample_counts`, `register_table.inc`, and
`pm4_command_processor_implement.h::ExecutePacketType3_EVENT_WRITE_ZPD`.
The EXT visibility bounds were also corrected to eight-pixel units.

An experiment changing COHER and vblank identifiers together stalled boot at
53 draws (`build/native-session-gpu-registers`). Further comparison with
`GraphicsSystem::ReadRegister` showed that **0x1951 is the intended vblank
interrupt status**, distinct from 0x194D's vblank status. Its misleading local
name was corrected. COHER_STATUS_HOST is 0x0A31 (0x0A2F is COHER_SIZE_HOST).
The native SHD counter was also using completed frames rather than vblank
ticks; it now advances on the 60 Hz vblank thread, as Xenia's MarkVblank does.
These later timing changes require a fresh session check. Do not call the
53-draw experiment successful.

## Primary sources and implications

- [From Dust compatibility issue #1147](https://github.com/xenia-project/game-compatibility/issues/1147)
  records sensitivity to VSync / CPU scheduling and missing ground textures.
  The 2024 report says changing readbacks, render-target paths and depth options
  did not resolve that report's missing terrain. This is evidence against
  treating this as a generic texture-download problem, not proof that our bug
  has the same cause. It also documents the invalid-fetch compatibility option.
- [Xenia texture cache implementation](https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/texture_cache.cc)
  explicitly handles `kInvalidTexture` (type 0) when the compatibility option is
  enabled. Our native decoder / binding path currently accepts only type 2.
  However, an audit of **1,461 + 1,572 + 191 actually used texture bindings** in
  three saved native/CPU-input frames found only type 2. Do not present enabling
  invalid fetches as a confirmed fix for these frames.
- [Xenia's render-target write-up](https://xenia.jp/updates/2021/04/27/leaving-no-pixel-behind-new-render-target-cache-3x3-resolution-scaling.html)
  explains that color and depth views can reinterpret the same EDRAM storage,
  and that independent host images do not preserve these semantics. Preserving
  host depth precision across transfers also matters for multipass rendering.
  Our color/depth caches share identical views but do not implement full
  ownership / reinterpretation of overlapping EDRAM ranges.
- [Xenia render-target cache](https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/render_target_cache.cc)
  tracks ownership by EDRAM tile ranges and transfers data as ownership changes.
  This is a concrete reference for auditing the native resource model.
- [Xenia command processor](https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/command_processor.cc)
  is the reference for draw submission, waits, read-pointer publication and
  completion events. Our native rendering executes engine batches at `VdSwap`,
  while the small PM4 service counts draws and services events separately.
  Audit the ordering between actual native GPU execution and completion exposed
  to the guest, especially for work observed before a swap. This is an open
  hypothesis, not a demonstrated root cause.

## Local falsification checks

All checks are diagnostic copies; no depth/stencil bypass was enabled in the
production renderer.

- `build/native-input-isolated` replays a captured native 619-draw frame. The
  blue silhouettes also occur in this isolated adapter, so preserving targets
  between frames is not by itself a complete explanation.
- `build/native-input-gpu-seeds` substitutes 44 uniquely matched resource base
  images from a GPU-consuming reference, matched by shader/slot use and shape.
  Higher mips were deliberately left unchanged. Only 464 pixels change, with a
  maximum channel difference of 4; the visual defect remains. This is a limited
  test, not proof that every texture subresource is correct.
- `build/research-stencil-off` disables stencil in the saved diagnostic. Its RGB
  is exactly unchanged. Stencil rejection is not the explanation in that frame.
- `build/research-depth-always` changes comparison to ALWAYS on 198 color draws.
  It produces heavy overdraw and retains the missing-detail problem. Disabling
  depth is not a fix; full depth-storage / copy semantics still need auditing.
- Metrics: `build/research-online-checks.json`.

The earlier synchronized 634-draw CPU-input frame with GPU-consumer textures
still passes against the reference at **51.4268 dB, zero pixels over 32 RGB error,
zero D3D12 errors** (`build/native-cpu-frame-intro`). Its image is a title/menu
transition, not proof of completed native gameplay.
