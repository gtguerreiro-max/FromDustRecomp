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

git clone https://github.com/xenia-canary/xenia-canary third_party/xenia
git -C third_party/xenia checkout 96ce1ac47ee7646deac2a2c9c912351e8ff0edee
git -C third_party/xenia submodule update --init --depth 1 \
    third_party/DirectX-Headers third_party/FFmpeg third_party/FidelityFX-CAS \
    third_party/FidelityFX-FSR third_party/cxxopts third_party/date \
    third_party/disruptorplus third_party/fmt third_party/imgui third_party/snappy \
    third_party/tomlplusplus third_party/utfcpp third_party/xbyak third_party/xxhash
git -C third_party/xenia apply ../../patches/xenia.patch
```

`third_party/xenia/third_party/DirectXShaderCompiler/include` holds two public
headers from microsoft/DirectXShaderCompiler (see `third_party/xenia_gpu/README.md`).
The game files (`game/`) must come from your own copy of From Dust (XBLA).
