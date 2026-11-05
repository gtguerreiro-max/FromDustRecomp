# From Dust recompilation

Windows x64 runtime for the statically recompiled Xbox 360 version of From Dust
(title ID `58410A91`). The CPU runs generated native C++; Xenia Canary's embedded
Direct3D 12 backend handles the original Xenos GPU commands.

## Build and run

Requires the existing game files in `game/`, generated code in `ppc/`, Visual
Studio 2022 Build Tools with Clang and the Windows SDK, CMake, Ninja, and Python 3.

```powershell
Set-Location F:\FromDustRecomp
.\build_all.ps1 -SkipRecomp
.\play.ps1
```

The executable can also be opened directly from `build/`: it finds the sibling
`game/` directory automatically and opens a normal Windows game window.

The experimental native D3D12 renderer can be selected with `Jogar-nativo.cmd`
or `.\play.ps1 -Renderer native`, using `build/native-shaders-live/catalog.bin`.
It plays the game (title, introduction, tutorial levels) at the game's 30 FPS,
with NVIDIA DLAA and DLSS Frame Generation (2x-4x: 60-120 FPS on screen)
available in the F1 panel on RTX GPUs. Synchronized frames
compare with Xenia at 50-52 dB. The regular launcher keeps Xenia as its default.

## PC settings panel

Press **F1**, or click **Definições** in the top-right corner. The panel is in
Portuguese and saves preferences automatically to `build/user/settings.ini`.
Game input is blocked while it is open; F1 or Escape closes it. The game keeps
rendering while the panel is open.

- **Ecrã:** window or borderless fullscreen, five window sizes through 4K, FPS
  and frame interval counters, HUD corner, 75–150% interface scale, bilinear /
  FSR / CAS presentation, and sharpness. Changes apply immediately. **F11** or
  Alt+Enter toggles fullscreen.
- **Gráficos:** anti-aliasing (original or **NVIDIA DLAA**, native renderer and
  RTX GPU, applied immediately, with a live status line); **internal
  resolution** 1x-3x and **DLSS Super Resolution** (Qualidade / Equilibrado /
  Desempenho / Ultra desempenho) for the native renderer, applied
  immediately; **DLSS Frame
  Generation** off / 2× / 3× / 4× (RTX 40 for 2×, RTX 50 for 3× / 4×; on or off
  applies on restart, the multiplier immediately; paused while the panel is
  open or the window is not focused); 1× / 2× / 3× internal
  resolution for the Xenia backend, VSync, 30 / 60 / 120 / unlimited FPS, and anisotropic filtering.
  The latter apply on restart, with an explicit **Reiniciar o jogo** button. VSync synchronizes the Windows swapchain;
  the guest frame limiter also runs at the point where frames are presented.
- **Controlos:** automatic, keyboard/mouse or XInput controller. PC-style
  mouse: moving the mouse guides the Breath (with its own speed), left button
  absorbs (LT), right button releases (RT), holding the middle button turns
  the camera and the wheel zooms; the cursor is hidden while playing and
  returns with F1 / Esc. Camera sensitivity, stick deadzone, invert Y and
  controller vibration. Hover an option for an explanation.
  Click any keyboard binding, then press the new key. Escape cancels. Reusing a
  key swaps the two assignments. F1, F11 and Escape are reserved for the panel.
- **Renderer e estatísticas:** active backend, GPU, local memory usage/budget,
  active graphics settings, frame interval history with mean/P95/maximum,
  draw requests and native integration status. These intervals are not GPU
  execution timings. The selected backend is shown; the experimental native
  renderer uses the game's 1x resolution and filtering, so its unavailable
  internal scaling / anisotropy overrides are disabled in the panel.

Default keyboard controls:

| Action | Keys |
| --- | --- |
| Start / pause | Enter |
| A / B | Space / Backspace |
| Movement | WASD |
| Camera | IJKL, or drag with the right mouse button |
| D-pad | Arrow keys |
| X / Y | X / R |
| LB / RB | Q / E |
| LT / RT | Z / C |
| Back / select | Tab |

Controller input is only passed to the game while its window has focus. A local
Player profile is available and level 1 has been reached. Complete progression
and an in-game save point still need validation.

Omit `-SkipRecomp` to regenerate the CPU modules using XenonRecomp as well. The
Direct3D 12 backend is enabled by default. `-NoGraphics` builds the earlier
command-only runtime instead.

For a bounded diagnostic run:

```powershell
.\run.ps1 -Seconds 75 -Headless -Capture .\build\frame.png -CaptureSeconds 65
.\run.ps1 -Seconds 15 -Renderer null
```

`-Headless` renders offscreen using the same GPU backend. `-Renderer null` runs
the command-only processor for comparison. `-Trace` enables import logging;
`-Watchdog 20` records guest stacks after 20 seconds.

Direct launches also support `FD_RENDERER=null`, `FD_HEADLESS=1`, `FD_CAPTURE`
(PNG path), and `FD_CAPTURE_SECONDS`. Logs are written as `FromDust.log` in the
working directory and `FromDust-GPU.log` beside the executable.

Diagnostic variables:

- `FD_AUTOPRESS=<s>:<hex buttons>[:<ms>[:<LT>:<RT>[:<LX>:<LY>]]],...` drives pad 0,
  for example `40:0010` (Start), `50:1000` (A), `160:0:6000:255:0` (hold LT 6 s).
- `FD_PROFILE=<s>` samples a guest thread (`FD_PROFILE_DELAY`, `FD_PROFILE_THREAD`).
- `FD_AUDIO_DUMP=<file>` writes the mixed output as raw 48 kHz 6-channel float32.
- `FD_SELFTEST=save` creates, writes, reopens and reads back a test save through
  the same XAM/NT exports the game uses, then exits (exit code 0 = pass).

## Current state

- All seven XEX modules have native code and load in the runtime.
- Direct3D 12 initialization, video playback, and the animated 3D title screen
  have been verified in a 65-second offscreen run without crashes. A real
  1024x600 title-screen capture is saved in `build/title-frame.png`. The last
  ten one-second presentation samples ranged from 46 to 53; this isn't a
  gameplay performance measurement. Logs are in `build/validation-d3d12.log`
  and `build/validation-d3d12-gpu.log`.
- The window and UI message loop have also been verified in a 22-second run
  with the window hidden. See `build/validation-window.log` and
  `build/window-frame.png`.
- The physical memory adapter invalidates GPU buffer/texture caches on CPU
  writes, including destinations of Windows file reads.
- The PC settings overlay, preference persistence, keyboard remapping, live
  window resizing and fullscreen switching have been tested in the game window.
  `build/settings-panel.jpg` shows the panel. Keyboard/mouse and Windows XInput
  now implement the Xbox input exports. Physical controller behavior still
  needs a hardware test.
- Internal resolution at 2× was verified in a 65-second run, producing a
  2048x1200 title-screen image in `build/settings-scale2.png` without crashes.
  The logs are in `build/validation-settings-scale2.log` and the matching
  `-gpu.log` file. The 3× setting has not yet been visually tested.
- Audio: `runtime/apu/apu.cpp` drives Xenia's XMA decoder (its FFmpeg fork,
  `third_party/xenia/third_party/FFmpeg`) and XAudio2 output driver. The XMA
  register writes (stwbrx + eieio) reach it through `PPC_MM_STORE_U32` in
  `runtime/ppc_overrides.h`. The log reports kicks, frames and peak level every
  10 s. Volume and mute-when-unfocused are in the F1 panel ("Som").
- A local profile ("Player") is signed in and the title is fully licensed
  (`runtime/kernel/exports_xam.cpp`). The game reaches level 1. Save creation
  and loading pass `FD_SELFTEST=save`, but an in-game save point has not been
  reached in a test yet.

The integration is in `runtime/gpu/xenia_bridge.cpp`. The embedded backend build
and kernel/memory adapters are in `third_party/xenia_gpu/`. It uses Xenia's BSD
licensed graphics code without its CPU emulator or kernel. See
`third_party/xenia/LICENSE` and dependency licenses under `third_party/`.

## Settings and input checks

Configure CMake with `-DFD_BUILD_TESTS=ON`, build `RuntimeSettingsInputTests`, then
run `ctest --test-dir build -R RuntimeSettingsInput --output-on-failure`.
The checks exercise preference reload and invalid values, duplicate key swaps,
keyboard-to-controller translation, blocked input, big-endian output and empty
event handling. Interactive validation logs are in
`build/validation-settings-ui.log` and `build/validation-settings-ui-gpu.log`.

## Native renderer diagnostics

The native C++ renderer is being integrated; interactive play still uses Xenia.
Four saved frames / 3,160 draws render with the native texture memory producer
and retain the validated native baseline RGB. Its 264 base textures, seven
cubemaps and 1,160 mip levels match the independent reference bytes.

With `FD_NATIVE_RENDERER=ON` in the build, run:

```powershell
.\tools\validate_native_textures.ps1 -Screenshot
```

This bounded test acquires textures inside the game after GPU resolve readback,
derives their layouts natively, and compares them with independent raw captures.
The verified title run acquires 49 textures, one cubemap and 178 mip levels with
zero differences. It uses isolated settings and writes evidence under
`build/native-texture-validation/`. This first-use-per-address capture is a
diagnostic stage; resource versions, complete draw expansion and native
presentation remain pending. See `docs/NATIVE_RENDERER.md` and
`tests/VALIDATION.md` for scope and evidence.
