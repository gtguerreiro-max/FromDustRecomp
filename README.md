<div align="center">

# From Dust for Windows, with DLSS

**The Xbox 360 (XBLA) version of From Dust, statically recompiled into a native Windows program,
with an experimental Direct3D 12 renderer that adds NVIDIA DLAA, DLSS and DLSS 5.**

![Windows](https://img.shields.io/badge/Windows-10%20%7C%2011-0078D6?logo=windows&logoColor=white)
![NVIDIA](https://img.shields.io/badge/NVIDIA-DLAA%20%7C%20DLSS%20%7C%20DLSS%205-76B900?logo=nvidia&logoColor=white)
![Status](https://img.shields.io/badge/status-work%20in%20progress-orange)
![Game files](https://img.shields.io/badge/game%20files-not%20included-lightgrey)

</div>

This is **not an emulator**. The game's PowerPC code is translated ahead of time into C++ with
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and built with this project's own
runtime (kernel, memory, audio, input). The graphics go through
[Xenia Canary](https://github.com/xenia-canary/xenia-canary)'s Direct3D 12 backend, or through
an experimental native renderer that draws the game's Direct3D calls directly.

> [!IMPORTANT]
> **You need your own copy of the game.** Nothing from the game is in this repository: no
> executables, data, saves or generated code. The recompiled code is made on your computer, from
> your copy of From Dust for Xbox 360 Live Arcade (title ID `58410A91`).

> [!NOTE]
> **Made with AI.** Most of the code and the investigation were done by Claude (Anthropic's AI),
> directed and tested by the repository's owner.

> [!TIP]
> Looking for the **Steam Deck**? See [FromDustDeck](https://github.com/gtguerreiro-max/FromDustDeck),
> the later ReXGlue-based port that runs at 60 FPS on the Deck.

## Features

- **Two renderers.** Xenia's Direct3D 12 backend (the default, the complete one), or the
  **native renderer** (`Jogar-nativo.cmd`), which plays the title, introduction and tutorial
  levels and matches Xenia's image at 50-52 dB.
- **NVIDIA RTX features** in the native renderer: **DLAA**, **DLSS Super Resolution** (Quality to
  Ultra Performance), **DLSS Frame Generation** 2x-4x with Reflex markers, and **DLSS 5 Neural
  Rendering** (see status below).
- **PC settings panel** (F1), in English, saved in `build\user\settings.ini`. Most options apply
  live.

### Camera

- **Global view by default.** The game's far camera (RB) works like an RTS view on a PC screen,
  so it is kept: when the game drops back to the close camera on its own (after cut scenes and
  tutorials), the global view comes back. Pressing RB yourself to go close is respected.
- **Mouse wheel zoom** of the global view: straight in and out (0.4x-1.6x), without falling into
  the close camera. Speed per notch, inversion, and an option to remember the zoom between
  sessions.
- **Edge scrolling:** with the mouse, the Breath moves over a still camera, and the camera scrolls
  when the Breath reaches the screen edges, faster the closer it is to the edge. The edge band
  and speed are adjustable.
- **Camera rotation** by dragging with the middle (or right) mouse button, with its own speed
  and per-axis inversion.
- With a controller, or with these options off, the camera follows the Breath as on the console.

### Mouse, keyboard and controller

- **PC-style mouse** for the Breath: it follows the pointer on screen (or works like a stick, in
  the classic mode); the left and right buttons absorb and release.
- **Keyboard and mouse prompts:** the game's controller glyphs in menus, tutorials and tool tips
  are redrawn as keycaps with your current bindings, and mouse icons. They switch live between
  controller and keyboard.
- Remappable keys, stick sensitivity, dead zone, Y inversion, vibration.

### Picture

- Window or full screen, sizes up to 4K; FSR 1 or CAS presentation; internal resolution 1x-3x
  (native renderer: any scale, with DLSS on top).
- **Anisotropic filtering** 2x-16x, and **sharper shadows** (shadow maps 2x or 4x the game's
  512x512).
- **HDR output** on an HDR display: the game's SDR image expanded, highlights such as lava and
  fire brighter, the HUD kept at paper white. Not yet checked on a real HDR screen.

### Smoothness and speed

- **Fixed-rate world:** the world (water, lava, villagers) keeps the original 30 steps a second
  while the frame rate goes higher, with the water's texture flow kept at the world's speed.
- **Smooth terrain and water:** their heights are blended between world steps, so they move
  smoothly above 30 FPS.
- **Even pacing** (120, 60 or 30 FPS, so every world step stays on screen for the same time),
  VSync and a frame limit.
- Lower input latency: about 53 ms at the original 30 FPS, about 37 ms with the fixed-rate world
  (measured on an RTX 5060 Ti).

### HUD and information

- Optional **FPS counter**, **frame time** and **measured input latency**, in any corner, with an
  interface scale of 75-150 %.
- The F1 panel's last tab shows the active renderer, GPU, window size, video memory use and a
  graph of the last 240 frame times (mean, 95th percentile, worst).
- DLSS 5 and HDR work on the scene before the game's HUD, so text and subtitles stay untouched.

## Status

Work in progress. The game reaches and plays the first levels; complete progression has not
been checked.

- **DLSS 5 Neural Rendering now works on the whole scene.** As in the PGR4 port, it runs on
  the scene right after DLAA / DLSS, before the game's depth of field, bloom and colour grading,
  so the model's change goes through the game's own look and the HUD stays untouched. On the
  finished image, where it ran before, the effect was barely visible. In the first level, model A
  at 200 % now changes 10.5 % of the pixels by more than 10/255 (0.4 % before). Its skin auto
  mask is off. **F8** turns it on and off in play; strength settings (model, structure, tone,
  passes) are in F1 > Graphics.
- The native renderer runs the game at its 30 FPS, or higher with the fixed-rate world (60-120 on
  screen with Frame Generation).
- A physical controller and an in-game save point still need a full test.

## What you need

- **From Dust for Xbox 360 (XBLA)**, extracted so that `game\` holds `Default.xex`, the
  `Galileo*.xex` modules and the game's data.
- **Windows 10/11** with Visual Studio 2022 Build Tools (clang-cl, Windows SDK), CMake, Ninja,
  Python 3 and Git.
- The upstream sources and NVIDIA SDKs listed in [patches/README.md](patches/README.md), checked
  out at their pinned commits with this project's patches applied. `tools/fetch_dlss_nr.py`
  fetches the DLSS 5 package from NVIDIA and installs it only if its signature is NVIDIA's.
- For the RTX features, an NVIDIA RTX GPU (RTX 40 for 2x Frame Generation, RTX 50 for 3x / 4x)
  and a recent driver. Nothing bypasses NVIDIA's checks: on hardware that says no, the option
  reports "unavailable".

## Build and play

```powershell
git clone https://github.com/gtguerreiro-max/FromDustRecomp
cd FromDustRecomp
# Set up the upstream sources as in patches\README.md, and put the game in game\.
.\build_all.ps1          # recompiles the modules into ppc\ and builds FromDust.exe
.\play.ps1               # Xenia's renderer
.\play.ps1 -Renderer native   # or Jogar-nativo.cmd: the native renderer with DLSS
```

After the first build, `.\build_all.ps1 -SkipRecomp` reuses the generated modules. The
executable in `build\` finds the sibling `game\` folder by itself.

## Controls

Press **F1** for the settings panel (Escape or F1 closes it). An Xbox controller works through
XInput. With the mouse: the pointer moves the Breath, the left button absorbs, the right button
releases, the wheel zooms and the middle button turns the camera. In the native renderer
**F8** turns DLSS 5 on and off, to compare in play. Default keyboard:

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

## Documentation

- [docs/NATIVE_RENDERER.md](docs/NATIVE_RENDERER.md): how the native renderer works, DLSS, DLSS 5
  and the measurements behind each change.
- [docs/RENDER_DEBUG_WORKFLOW.md](docs/RENDER_DEBUG_WORKFLOW.md): capturing and comparing frames.
- `run.ps1` runs bounded diagnostic sessions (`-Headless`, `-Capture`, `-Renderer null`); the
  `FD_*` environment variables are described in the docs.

## Credits and licences

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and
  [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) (hedge-dev).
- [Xenia Canary](https://github.com/xenia-canary/xenia-canary) (BSD 3-Clause): its graphics
  backend, XMA decoder and audio driver, without its CPU emulator or kernel.
- [plume](https://github.com/renderbag/plume).
- NVIDIA DLSS, Streamline and the NGX SDK, under NVIDIA's licences, fetched separately.
- From Dust is © Ubisoft. This project is not affiliated with or endorsed by Ubisoft, Microsoft
  or NVIDIA.
