# Upstream sources

These clones are not committed. Recreate them at the pinned commits and apply
the local patches:

```bash
git clone https://github.com/hedge-dev/XenonRecomp XenonRecomp
git -C XenonRecomp checkout ddd128bcca99fe8bfbb99bea583c972351fa6ace
git -C XenonRecomp submodule update --init --recursive
git -C XenonRecomp apply ../patches/xenonrecomp.patch

git clone https://github.com/hedge-dev/XenosRecomp XenosRecomp
git -C XenosRecomp checkout 990d03b28a27b50277ee5d8d942e1c5f873869d1
git -C XenosRecomp submodule update --init --recursive
git -C XenosRecomp apply ../patches/xenosrecomp.patch

git clone https://github.com/renderbag/plume third_party/plume
git -C third_party/plume checkout b84a6cebb83aa052cf0690946e7c5de36fb0cfd1
git -C third_party/plume submodule update --init --depth 1
git -C third_party/plume apply ../../patches/plume.patch

git clone https://github.com/xenia-canary/xenia-canary third_party/xenia
git -C third_party/xenia checkout 96ce1ac47ee7646deac2a2c9c912351e8ff0edee
git -C third_party/xenia submodule update --init --depth 1 \
    third_party/DirectX-Headers third_party/FFmpeg third_party/FidelityFX-CAS \
    third_party/FidelityFX-FSR third_party/cxxopts third_party/date \
    third_party/disruptorplus third_party/fmt third_party/imgui third_party/snappy \
    third_party/tomlplusplus third_party/utfcpp third_party/xbyak third_party/xxhash
git -C third_party/xenia apply ../../patches/xenia.patch
```

The native renderer's NVIDIA SDKs are not committed either:

- `third_party/dlss_sdk`: sparse checkout of https://github.com/NVIDIA/DLSS
  (`include`, `lib/Windows_x86_64/{x64,rel}`) for DLAA.
- `third_party/streamline`: `streamline-sdk-v2.14.16.zip` from
  https://github.com/NVIDIA-RTX/Streamline/releases, extracted (`include`,
  `lib/x64`, `bin/x64`) for DLSS Frame Generation.

`third_party/xenia/third_party/DirectXShaderCompiler/include` holds two public
headers from microsoft/DirectXShaderCompiler (see `third_party/xenia_gpu/README.md`).
The game files (`game/`) must come from your own copy of From Dust (XBLA).

The From Dust shader/runtime ABI includes the plain-2D case of Xenos
`tfetch3D`: bit 31 in the 3D descriptor index tags a 2D heap index. Rebuild the
shader catalog and regenerate replay-local shader assets together with runtime
changes to this binding. Actual volume and multi-slice stacked uploads remain
unsupported. See `docs/NATIVE_RENDERER.md` for the sparkle regression evidence.

Conditional EXEC/jump guards also read the full 256-bit Xenos boolean bank at
shared offset 1680, including pixel indices 128 onward. Rebuild the runtime and
shader assets together. The patch includes the control-flow correction and the
shared constant declarations; see the light-shaft validation in the same document.
