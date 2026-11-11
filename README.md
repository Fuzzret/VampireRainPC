# VampireRainRecomp

A native Windows version of **Vampire Rain** (Xbox 360, 2007), made by
statically recompiling the game's PowerPC code to C++ with
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and running it on a
reimplementation of the Xbox 360 system functions it uses, with Direct3D 12
graphics.

**This repository contains no game code or data.** You need your own copy of
the game disc. The builder reads it, extracts the game files and recompiles
the executable on your computer.

## Features

- Runs at a steady 30 fps, or shows 60, 120, 144 or unlimited frames per
  second: the game still runs at 30 (its logic is tied to it), and the
  renderer blends in-between frames from the last two game frames (this adds
  about one game frame of display delay)
- Direct3D 12 renderer: render resolution up to 4x (5120x2880), FXAA, mipmaps,
  vsync; windowed, borderless window or fullscreen (Alt+Enter switches)
- Other screen shapes: 4:3, 16:10, 16:9, 21:9 and 32:9 (the view widens or
  narrows instead of stretching; set Aspect ratio in the launcher)
- Keyboard and mouse, with direct mouse camera control and rebindable keys;
  Xbox controllers work too, with
  adjustable vibration
- Full sound (XMA audio)
- LAN multiplayer through the game's Xbox Live menus: host and join games
  with other PCs on your network
- Launcher for display, controls and online settings

## Requirements

- Windows 10 or 11, 64-bit
- A GPU with Direct3D 12 and rasterizer ordered views (most GPUs from 2015 on)
- A CPU with AVX2 by default (run the builder with `-Arch x86-64-v2` for older CPUs)
- A disc image of **Vampire Rain (USA, Europe)**, made from your own disc
- To build:
  - [Git for Windows](https://git-scm.com/download/win)
  - [Visual Studio 2022](https://visualstudio.microsoft.com/) (Community is
    free) or the Build Tools, with these components:
    - Desktop development with C++
    - C++ Clang tools for Windows
    - C++ CMake tools for Windows
- About 15 GB of free disk space, and 30-60 minutes for the first build

## Building

1. Clone or download this repository.
2. Optionally, put your disc image (`.iso`) in the repository folder.
3. Open PowerShell in the repository folder and run:

   ```
   powershell -ExecutionPolicy Bypass -File .\build.ps1
   ```

   If the image isn't in the folder, you'll be asked to pick it, or you can
   give its path with `-Iso "D:\path\to\Vampire Rain.iso"`.

The builder:

1. Finds Visual Studio's Clang compiler, CMake and Ninja.
2. Downloads XenonRecomp and Xenia's FFmpeg (for XMA audio) at tested commits,
   and applies `patches/xenonrecomp.patch`.
3. Builds XenonRecomp and `vr_tool` (disc extraction and executable decryption).
4. Extracts the disc into `Vampire Rain (USA, Europe)\` and checks that it is
   the supported version.
5. Decrypts the game executable into `work\default_dec.xex`.
6. Recompiles it to C++ in `recomp_out\` (with `config\vampirerain.toml`).
7. Builds the game and puts `VampireRain.exe` in the repository folder.

Running it again skips the steps that are already done. Use `-Force` to redo
everything, or `-NoBuild` to stop before compiling the runtime.

## Playing

Start **VampireRain.exe** in the repository folder. The launcher opens first, with display,
control and online settings (start with `--no-launcher` to skip it, or untick
"Show this window" and use `--launcher` to get it back).

Saves, settings and the shader cache go in `userdata\`. If something goes
wrong, `runtime.log` (and `runtime.prev.log`, from the run before) describe
it; start with `--verbose` for a detailed log, or `--console` to watch it. The first time new
effects appear, their shaders are compiled in the background, so an object may
be missing for a moment. Every shader the game has used is remembered, and all
of them are prepared at startup ("Preparing shaders"), so after that first
encounter they never pop in - including after an update or a graphics driver
change, which makes the startup take longer once.

### Default controls

| Action | Keys |
|---|---|
| Move | W A S D |
| Look / aim | Mouse (I J K L) |
| Shoot / aim | Left / right mouse button |
| A / B / X / Y | Space / C or Left Ctrl / R / E |
| LB / RB | Q / F |
| Start / Back | Enter / Tab |
| Left / right stick click | Left Shift / middle mouse or V |
| D-pad | Arrow keys or 1-4 |
| Menu (settings, controls, quit) | Esc |
| Save textures for modding / reload mods | F11 / F10 |

All of these can be changed under **Controls...** in the launcher, or in the
game: Esc opens the menu with the same settings. Display mode, window size,
frame rate, vsync, anti-aliasing and controls change at once; render
resolution, aspect ratio and player name the next time the game starts.

### Texture mods

Replacement textures go in `mods\textures` (next to `VampireRain.exe`; the
folder is made on the first start). Each texture is named by a 16-digit code
worked out from its contents, so a replacement keeps working wherever the game
loads it:

1. In game, press **F11**. The textures drawn in that frame are saved to
   `mods\dump` as `.dds` files named by their code (e.g. `3f2a9c0d1b4e5f60.dds`).
2. Edit or upscale the one you want (any size; a higher resolution is fine)
   and save it into `mods\textures` as `.dds` (BC1/DXT1, BC2/DXT3, BC3/DXT5,
   BC4, BC5, BC7 or 32-bit RGBA) or `.png`, keeping the 16-digit code at the
   start of the name. Anything after it is ignored, so `3f2a9c0d1b4e5f60_brick.png` works.
3. Press **F10** in game to load the folder again, or restart.

Keep the dumped file's colour channels as they are: some textures store data
other than colour (normal maps, masks), and the game reads them as saved.
Render targets (reflections, the screen itself) and cube maps can't be
replaced. `runtime.log` lists each replacement used, and any file that
couldn't be loaded.

### LAN play

Choose Xbox Live in the game's main menu. Games hosted on one PC show up in
the other PCs' searches. Allow `VampireRain.exe` through Windows Firewall on
private networks; it uses UDP ports 1000 and 3076. Set your name in the
launcher.

To try it with two copies on one PC, set `lan_test_instance = 1` in
`userdata\settings.ini` (under `[network]`). Start the second copy with
`VampireRain.exe --data userdata2`, with
`lan_test_instance = 2` in `userdata2\settings.ini`.

## Repository layout

| Path | Contents |
|---|---|
| `build.ps1` | The builder |
| `runtime/` | The PC runtime: Xbox 360 kernel and system functions, Direct3D 12 GPU emulation, audio, input, networking |
| `config/` | XenonRecomp configuration for this game (function bounds, jump tables) |
| `patches/` | Changes to XenonRecomp needed for this game |
| `tools/vr_tool/` | Disc extraction and executable decryption |

## Credits and licenses

- This project's own code: MIT, see [LICENSE](LICENSE)

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp): the recompiler (MIT)
- [Xenia](https://github.com/xenia-project/xenia): GPU register definitions
  and shader interpreter in `runtime/third_party/xenia` (BSD, see its LICENSE),
  and its FFmpeg fork with the XMA decoder (LGPL), downloaded at build time
- Vampire Rain is © its respective owners. This project is not affiliated
  with or endorsed by them.
