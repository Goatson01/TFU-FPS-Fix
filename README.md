# TFU-FPS-Fix

**A high frame rate fix for *Star Wars: The Force Unleashed* (PC) that keeps the physics intact.**

The PC version of The Force Unleashed is locked to 30 FPS. The existing unlock patches raise the
cap but break the physics: Force-gripped objects float upward, debris and ragdolls misbehave,
and so on. TFU-FPS-Fix runs the game at 60 FPS while the physics behave exactly like the
original 30 FPS game.

## Features

- **60 FPS** with a configurable frame cap
- **Physics fix:** the game simulation runs at a fixed 30 Hz, exactly like the original game,
  while rendering runs at 60 FPS. Gripped objects stay where you hold them, and hit-stop and
  slow motion work as intended.
- **Smooth animation** between simulation steps (interpolation at the Direct3D 9 level)
- **Borderless windowed** mode: instant alt-tab, other windows can be shown on top of the game
- **No modified game files:** `SWTFU.exe` is never changed. Everything is applied in memory
  and removed by deleting two files.

## Installation

1. Download `TFU-FPS-Fix-1.0.zip` from the [Releases](../../releases) page.
2. Open the game folder. In Steam: right-click the game > **Manage** > **Browse local files**.
3. Copy `dinput8.dll` and `tfufix.ini` from the zip into that folder, next to `SWTFU.exe`.
4. Start the game as usual.

**To uninstall,** delete `dinput8.dll`, `tfufix.ini` and `tfufix.log` from the game folder.

> Use the original `SWTFU.exe`. Don't combine this fix with the old "FPS cap fix" executables
> (40/50/60/Unlimited FPS `SWTFU.exe`).

## Compatibility

| Version | Status |
|---|---|
| Steam, Ultimate Sith Edition, v1.2.94564 | Tested |
| GOG / retail v1.2 | Same game code, should work, not tested |

The fix verifies the game code before patching anything and stays inactive if it doesn't
match. `tfufix.log` in the game folder shows what happened at startup.

## Settings

All settings are in `tfufix.ini`, with explanations. The defaults are the recommended ones.

| Section | Setting | Default | Description |
|---|---|---|---|
| `[Framerate]` | `FPSCap` | `60` | Frame cap. `0` = original (30). 60 is the tested value. |
| `[Physics]` | `FixedTimestep` | `1` | The physics fix. `0` = raw unlock only (physics bugs). |
| `[Interpolation]` | `Enabled` | `1` | Smooth animation between simulation steps. |
| `[Interpolation]` | `SnapDistance` | `8` | Advanced: larger jumps between steps aren't smoothed. |
| `[Display]` | `Borderless` | `1` | Borderless windowed. `0` = the game's exclusive fullscreen. |
| `[Debug]` | `Log` | `1` | Write `tfufix.log`. |
| `[Debug]` | `Telemetry` | `0` | Per-second statistics in the log, for troubleshooting. |

## Known limitations

- Some areas and cutscenes are still limited to 30 FPS by the game itself (for example parts
  of Imperial Raxus Prime and the Death Star).
- The world is simulated at 30 Hz. Character animation is smoothed, but some things, such as
  particles and certain effects, may still move in 30 Hz steps.
- Smoothing adds about one simulation step (33 ms) of display delay.
- Frame caps other than 60 have not been tested.

## How it works

The game loads `dinput8.dll` from its own folder. The fix forwards all DirectInput calls to the
real Windows DLL, waits until the Steam DRM has decrypted the game code, verifies it, and then
patches it in memory:

1. **Frame cap:** the 30 FPS cap in the game's application object is raised.
2. **Fixed 30 Hz simulation:** the call to the game's main update (`App::GameUpdate`) is
   redirected. It runs on a real-time 30 Hz clock, and during each update every timer value
   the game uses is set exactly as in the original game. Among other things, that gives Havok
   the vanilla pattern of two 1/60 s physics steps per update, which is what the game's force
   tuning (e.g. Force grip) expects.
3. **Interpolation:** at the Direct3D 9 level, each draw call is recognized between frames, and
   skeletal animation is blended between the last two simulation steps.
4. **Borderless windowed:** the Direct3D device is created windowed, and the game window is
   turned into a borderless, non-topmost window covering its monitor.

The full write-up, with all addresses, findings and dead ends, is in
[docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md).

## Building from source

The fix is a single freestanding C file (no C runtime) built with LLVM.

**Linux / macOS / WSL**, with `clang`, `lld-link` and `llvm-dlltool` installed:

```sh
./build.sh            # -> build/dinput8.dll
./package.sh 1.0      # -> dist/TFU-FPS-Fix-1.0.zip
```

**Windows**, with [LLVM](https://github.com/llvm/llvm-project/releases) installed
(e.g. `winget install LLVM.LLVM`) and on `PATH`:

```bat
build.cmd
```

Builds are reproducible: the same source produces a byte-identical DLL. The release
`dinput8.dll` for 1.0 has SHA-1 `ef9d9017276f6e293ea04a05e0e3b6b584b98fad`.

### Repository layout

```
src/tfufix.c                    the whole fix
src/*.def                       export (dinput8) and import (kernel32, user32) definitions
dist/tfufix.ini, README.txt     files that go into the release zip
docs/REVERSE_ENGINEERING.md     how it works, with every address
build.sh, build.cmd, package.sh build scripts
```

## Credits

Created by Claude. Like, fully, I take no credit beyond testing and making it open-source.

## License

[MIT](LICENSE).

*Star Wars: The Force Unleashed* is a trademark of Lucasfilm Ltd. This project is not
affiliated with or endorsed by Lucasfilm, Disney, LucasArts or Aspyr. No game files are
included or modified.
