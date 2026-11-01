# Embedded Xenia graphics backend

This CMake subdirectory builds the base, Windows UI, GPU, and Direct3D 12 parts
of the vendored Xenia Canary source. Headers in `shims/` replace the Xenia CPU,
kernel, and memory interfaces with adapters implemented in
`runtime/gpu/xenia_bridge.cpp`.

The GPU sees physical memory at the FromDust guest block's `0xA0000000` mapping.
CPU writes to cached pages trigger a Windows vectored exception handler, which
invalidates the GPU cache and restores write access. Windows file I/O explicitly
invalidates its destination before calling `ReadFile`. The guest MMIO write
pointer is polled; interrupt callbacks execute recompiled guest functions on
adopted host threads. The previous command-only processor remains available via
`FD_RENDERER=null`.

Local changes in the vendored source:

- `base/mapped_memory_win.cc`: use `const` for the invalid Windows handle, since
  the pointer cast cannot be a Clang constant expression.
- `base/filesystem_win.cc`: resolve the executable with `GetModuleFileNameW`,
  which works with the runtime's narrow `main` entry point.
- `gpu/draw_util.cc`: use the Clang-supported `_mm_cvtsi64_si128` intrinsic.
- `gpu/d3d12/d3d12_command_processor.cc`: count draws and swaps via memory shim
  telemetry, guarded by `FROMDUST_GPU_SHIM`. The optimized PM4 dispatch makes
  qualified backend calls, so subclass overrides cannot collect these counts.
- `gpu/d3d12/d3d12_graphics_system.cc`: stop initialization cleanly if Direct3D
  device creation fails.
- `ui/d3d12/d3d12_presenter.cc`: use the startup VSync preference for the
  Windows swapchain's presentation interval, disabling tearing flags when
  synchronization is enabled. This is guarded by `FD_XENIA_GPU`.

The memory telemetry adapter also calls the runtime's frame pacer on each swap,
so the PC FPS limit constrains actual guest frame submissions. GPU preferences
are set before workers start and remain fixed until restart. Window and image
presentation changes use the UI thread. The settings overlay reuses Xenia's
ImGui drawer; the game has its own keyboard/mouse/XInput adapter in
`runtime/input.cpp`, so the drawer's unused InputSystem query entry points return
device-not-connected.

Xenia requires fmt 12 while XenonRecomp uses fmt 11. They are built separately
with their own headers and coexist through fmt's versioned namespaces.

DirectXShaderCompiler's two required public headers and license were retrieved
from Microsoft's official `microsoft/DirectXShaderCompiler` repository. Shader
compiler DLLs are optional for the existing DXBC path.

Upstream: https://github.com/xenia-canary/xenia-canary

The Xenia BSD license is retained at `../xenia/LICENSE`. Third-party dependency
licenses remain in their original directories.
