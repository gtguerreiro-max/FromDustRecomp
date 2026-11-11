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
  Ultra Performance), **DLSS Frame Generation** 2x-4x, and **DLSS 5 Neural Rendering** (see
  status below).
- **Fixed-rate world:** the world (water, lava, villagers) keeps the original 30 steps a second
  while the frame rate goes higher, with the water's texture flow kept at the world's speed.
- **PC settings panel** (F1): display mode and sizes up to 4K, FSR / CAS presentation, internal
  resolution 1x-3x, VSync and frame limit, anisotropic filtering, keyboard remapping, a PC-style
  mouse mode for the Breath, controller options and sound. The panel is in Portuguese.

## Status

Work in progress. The game reaches and plays the first levels; complete progression has not
been checked.

- **DLSS 5 Neural Rendering does not work fully yet: it only changes faces and skin**, not the
  whole scene. The likely cause is known from the PGR4 port, where the same thing happened: the
  model's automatic skin mask, combined with the control mask by "min", keeps it off everything
  but skin, and a one-channel control mask cancels its local tone and structure. Turning the
  automatic mask off and giving the control mask its channels is the next step.
- The native renderer runs at the game's 30 FPS (60-120 on screen with Frame Generation).
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
XInput. Default keyboard:

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
