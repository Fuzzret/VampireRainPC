# Vampire Rain runtime

Host program that runs the XenonRecomp output of Vampire Rain (Xbox 360) on
Windows. It loads the game's executable into a 4 GB guest address space,
provides the 191 Xbox 360 kernel/XAM functions the game imports, and calls the
game's entry point.

## Building

Use `build.ps1` in the repository root - it extracts the disc, decrypts the
executable, runs XenonRecomp and builds this project (see the main README).
To rebuild after changing the runtime only:

```
cmake --build runtime\build
```

(from a *x64 Native Tools Command Prompt*, or run `build.ps1` again - it skips
the steps that are already done). CMake options: `RECOMP_DIR`,
`XENONRECOMP_DIR`, `FFMPEG_DIR` and `VR_ARCH` (default `x86-64-v3` = AVX2; use
`x86-64-v2` on older CPUs).

## Run

The build copies `VampireRain.exe` (and the shader compiler DLLs) into the
project folder; start it there. It's a normal window program with no console.

```
VampireRain.exe --no-launcher --verbose --console
```

Options: `--launcher` / `--no-launcher`, `--data <dir>` (saves and settings,
default `userdata`), `--game <dir>`, `--xex <file>`, `--verbose` (detailed
log: per-frame statistics, network, system messages), `--console` (show the
log in a console window), `--trace` (every implemented kernel call - very
large), `--software-gpu`.

The log is written to `runtime.log` (the previous run's to `runtime.prev.log`).
Without `--verbose` it holds startup, settings, warnings and errors only.

## Source map

| File | What it does |
|---|---|
| `src/main.cpp` | Startup, argument parsing, crash handler |
| `src/memory.*` | 4 GB guest reservation, on-demand commit, guest heaps |
| `src/xex_loader.*` | Loads the XEX image, fills the function table, resolves imports and kernel variables |
| `src/thread.*` | Guest threads (stack, KPCR, KTHREAD, context), calling guest code from host threads |
| `src/objects.*` | Events, semaphores, timers, handles, waiting |
| `src/xboxkrnl.cpp` | Kernel functions: memory, threads, sync, TLS, critical sections, strings, time, XEX |
| `src/kernel_file.cpp` + `src/vfs.*` | File I/O; maps `game:\`, `d:\`, `\Device\Cdrom0` to the disc folder |
| `src/format.cpp` | `sprintf` / `vsprintf` / `_vsnprintf` / `DbgPrint` over guest varargs |
| `src/xam.cpp` | Controller input (XInput), user profile, UI, content, network (offline) |
| `src/video.cpp` | `Vd*` functions + GPU stand-in (ring-buffer read-pointer echo, 60 Hz vblank) |
| `src/audio.cpp` | XAudio driver callback at console rate (no output yet), XMA contexts |
| `src/stubs.cpp` | **Generated** logged stubs for the 28 imports not implemented yet |
| `src/guest_checks.h` | Force-included into generated code: reports indirect calls to unknown functions |
| `tools/gen_stubs.py` | Regenerates `stubs.cpp` after you implement more functions |

To implement a stubbed import, add `GUEST_IMPORT(Name) { ... RETURN(value); }`
to any source file (arguments: `ARG32(0)`, `ARG32(1)`, ... — the 9th and later
come from the stack automatically), then run `python tools/gen_stubs.py`.

## Debugging aids built in

- **Crash handler**: a bad memory access prints the *guest* address, e.g.
  `CRASH: segfault at guest address 00000010` = a null pointer + 0x10.
- **Unknown indirect calls**: `indirect call to XXXXXXXX ... has no recompiled
  function` — that address is code XenonRecomp didn't find. Add it to
  `functions` in `vampirerain.toml` and re-run XenonRecomp.
- **Deadlock hint**: a thread stuck >10 s on a critical section logs which
  thread holds it.
- The null page (guest 0x0000-0xFFFF) is never mapped, matching the console.

## Status

Tested by building and running against the real game code and disc files:
the game boots, sets up its heaps, names and starts its 9 threads, initializes
the GPU ring buffer and interrupt callback, opens its pack files
(`pack\common\*.opb/.dpb/.tpb`) and reads them, and creates its draw thread.

It then crashes in effect/resource setup (`sub_824831C0`, called from
`sub_82483310` <- `sub_82484A70`): an object of the class with vtable
`820A0BE8` is given a data pointer (`+132`, set in `sub_824834D8` from field
`+4` of a load descriptor in `sub_82480D48`) to an aligned little-endian
texture-name list in `common.dpb`, but reads it with a packed layout (fields
at offsets 9/11/17/23/31/55). Either the descriptor points at the wrong
resource or the wrong reader class is chosen for it. Next step: trace where
the descriptor passed to `sub_82480D48` is built from the pack's table of
contents, and compare with Xenia (the emulator) running the same game.

## Not done yet

- **Graphics**: no renderer. The GPU stand-in only keeps the game from
  waiting. A real one parses the PM4 command stream in the ring buffer and
  draws with Vulkan/D3D12 (see Xenia's GPU code, and Unleashed Recompiled's
  approach of hooking D3D calls instead).
- **Audio output**, **XMA decoding**, **saves** (`XamContentCreateEx` is a
  stub), **profile settings**, **SEH unwinding** (`RtlUnwind`,
  `__C_specific_handler`).
- `NtSuspendThread` on *another running* thread is only counted, not enforced.
