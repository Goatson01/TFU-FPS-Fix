# Reverse engineering notes

How TFU-FPS-Fix works, and how every address in `src/tfufix.c` was found. All addresses refer
to `SWTFU.exe` version 1.2.94564 (image base `0x400000`).

## Contents

1. [The executable and its DRM](#1-the-executable-and-its-drm)
2. [Getting control at the right moment](#2-getting-control-at-the-right-moment)
3. [The frame cap](#3-the-frame-cap)
4. [Main loop and frame timer](#4-main-loop-and-frame-timer)
5. [Why physics breaks above 30 FPS](#5-why-physics-breaks-above-30-fps)
6. [The fixed 30 Hz simulation](#6-the-fixed-30-hz-simulation)
7. [Rendering and interpolation](#7-rendering-and-interpolation)
8. [Borderless windowed](#8-borderless-windowed)
9. [Dead ends and lessons](#9-dead-ends-and-lessons)
10. [Open issues and useful leads](#10-open-issues-and-useful-leads)

---

## 1. The executable and its DRM

- The Steam `SWTFU.exe` is wrapped with **SteamStub** (`.bind` section). Its `.text` section is
  encrypted on disk and decrypted in memory at startup.
- The retail build (SecuROM, `.secu` section) has readable code and **the same section layout**:
  `.data` and `.rsrc` are byte-identical to Steam, and `.rdata` differs by 86 bytes (imports).
  The retail executable was therefore used for all static analysis.
- SecuROM relocated some functions into `.secu` (e.g. `0x4BE770` jumps to `0x3A57E00`). The
  code is readable there. Steam's `.bind` (0x56000 bytes) is large enough to hold `.secu`
  (0x3C400) plus the SteamStub loader, which is consistent with the two builds being identical.
- Middleware: Havok 5.5.0-r1 (statically linked), NaturalMotion Euphoria
  (`libScumEuphoria_win32_release_DLL.dll`), Pixelux DMM (destructible materials, own thread).

## 2. Getting control at the right moment

- The game imports `DINPUT8.dll` and loads it from its own folder first, so a proxy
  `dinput8.dll` is loaded early. It forwards `DirectInput8Create` to the system DLL.
- `SWTFU Launcher.exe` (.NET, x86) loads `dinput8.dll` from the same folder. The proxy checks the
  process name and image size and stays passive outside `SWTFU.exe`.
- At `DllMain` time the code is still encrypted, and **SteamStub rebuilds the import table after
  `DllMain`**, so import-table hooks installed there are silently lost.
- The CRT entry point (OEP `0x8ED1D5` -> `0x8ECEB8`) calls `GetStartupInfoA` before `_initterm`
  and `WinMain`. The fix places a one-shot jump at the start of `kernel32!GetStartupInfoA`
  itself (which SteamStub doesn't touch). When it fires with the code decrypted, the fix
  verifies the expected bytes and patches. SteamStub calls it twice before decryption, so the
  hook re-arms itself until the code is ready (in practice on call #3).
- Import-table hooks (`Direct3DCreate9`, `CreateWindowExA`) are installed from this hook, when
  the final import table is in place.

## 3. The frame cap

- `WinMain` (`0x401210`) constructs the application object on its stack (`0x426CF0`), which
  sets the frame cap: `0x426E07  mov dword [esi+0x68], 30`.
- The old community FPS patches change only this immediate (40/50/60). Their "Unlimited"
  variant also turns the limiter branch `0x415F95 jbe 0x416056` into a `jmp`.
- The object is created right at the start of `WinMain`, so the patch must be in place before
  that, which is why the startup hook in section 2 is needed.

## 4. Main loop and frame timer

```
WinMain 0x401210:
    while (app.running /* +0xD8 */) {
        if (window active) App::Tick(0x426930); else Sleep(50);
        PeekMessage / TranslateMessage / DispatchMessage
    }

App::Tick 0x426930:
    FrameTimer 0x415DA0          -> writes the timer outputs below, applies the limiter
    renderer vtable +0x40         (renderer object at [0x1240448])
    App::GameUpdate(dt) 0x41BB90  <- called at 0x426AC0; ~1,400 functions, the whole simulation
    0x4CB9F0, 0x423480, 0x414780(dt), 0xC33740, 0x4192C0 ...
    renderer vtable +0x44
```

`App::GameUpdate` is `void __thiscall (App*, float dt)`, `ret 4`.

**Timer outputs.** The frame timer produces several values, and different subsystems read
different ones:

| Address | Meaning |
|---|---|
| `0x10D4008` | float, scaled frame time (game time; 0 during hit-stop) |
| `0x10D400C` | float, unscaled frame time (written at `0x415EE3`) |
| `0x1240290` | float[4], frame time broadcast for SSE code |
| `0x1240280` | float[4], 1 / frame time (1.0 when time is stopped) |
| `App+0x6C`  | float, the timer's dt output passed into `GameUpdate` |

`0x10D4008` alone is read in 354 places across ~250 functions. Fixing every frame-time
dependent bug individually would be a very long tail, which is why the fix keeps the whole
simulation at 30 Hz instead.

**Time scaling.** During hit-stop the game sets game time to 0 and **the limiter is effectively
off** (frames go up to the monitor's refresh rate). Hit-stop duration is counted in
`GameUpdate` *calls*, not seconds.

## 5. Why physics breaks above 30 FPS

Havok runs on its own thread:

```
App::GameUpdate 0x41BB90
  -> App 0x407480 (called at 0x41BEA9)
       -> PhysicsWorld::Step(float dt, bool) 0x651C30   (vtable slot at 0xFA5D4C)
            -> 0x6736F0 -> PhysicsThread::Kick(dt) 0x673200
                 writes dt to [thread+4], signals the worker
  worker thread loop (~0x673877): world->step([thread+4]) via 0x958E60 -> 0x97DE10
```

`0x407480` takes the step from **`App+0x70`**, and if it exceeds **1/45 s** (`0xF50C80`) it
calls `PhysicsWorld::Step` **twice with half the time**.

`App+0x70` is a smoothed value computed in `GameUpdate` at `0x41BDA8`:

```
target   = App+0x6C * timescale (0x10D4014)
App+0x70 = moves toward target, changing by at most the factor App+0x74 per update
```

So:

| Mode | Havok steps per game update | Step size |
|---|---|---|
| Vanilla 30 FPS | 2 | 1/60 s |
| Raw 60 FPS unlock | 1 | 1/60 s |

Game logic that applies forces once per update (for example Force grip's anti-gravity,
`ability_lift_magic_havok_counter_force_scaler`) is tuned for **two** physics substeps per
update. At 60 FPS it gets one, so it over-compensates gravity and gripped objects float upward.

## 6. The fixed 30 Hz simulation

The call at `0x426AC0` is redirected to the fix's `hook_GameUpdate`:

- A **real-time** accumulator (QueryPerformanceCounter) decides when a 30 Hz tick is due. At
  most one tick runs per frame (no catch-up bursts).
- Game time (`dt`, which can be scaled or 0) is accumulated separately, and each tick gets
  `1/30 * (game time / real time)` over the frames it covers, so slow motion behaves exactly as
  in vanilla.
- During the tick **all** timer outputs from section 4 are overridden (`0x10D4008`,
  `0x10D400C`, both vectors and `App+0x6C`) and restored afterwards. The game's own smoothing
  then settles `App+0x70` at 1/30 s and splits it into 2 x 1/60 s Havok steps, exactly like
  vanilla.
- `App+0x70` itself must **not** be written. It is persistent smoothing state; resetting it
  after each tick keeps it rate-limited at ~27.8 ms (physics at ~83% speed, slow upward drift).
- When game time is stopped (`dt == 0`, hit-stop/pause) every frame passes straight through,
  like vanilla, because hit-stop length is counted in update calls.

Verification (runtime probes in the debug build): `PhysicsWorld::Step` 60 calls/s at
16,666 µs, `App+0x70` = 33,333 µs, identical to vanilla 30 FPS.

## 7. Rendering and interpolation

Direct3D 9 facts (from a census build):

- `Direct3DCreate9` -> `IDirect3D9::CreateDevice` (vtable 16). Exclusive fullscreen, vsync
  (presentation interval 1), back buffer `A8R8G8B8`, depth `D24S8`.
- No fixed-function transforms; everything goes through vertex shader constants:
  - `c192`–`c195`: 4x4 matrix per draw (includes the camera)
  - `c196`, `c200`, `c204`: further 4x4 matrices
  - `c0`…: bone palette for skinned meshes, uploaded with up to 174 registers (58 bones x 3
    rows); the UI uploads `c0` and `c128` with 128 registers each
- `D3DPERF_BeginEvent` only marks `BeginScene`/`EndScene`.
- 200–600 draw calls and ~187 render target changes per frame in gameplay.
- **The renderer is pipelined**: per frame the order is `GameUpdate -> Present -> draws for the
  new state`. Frames with and without a tick render the same state with identical draw counts.

Interpolation (`interp_before_draw`):

- Each draw is identified by a hash of (vertex shader, stream-0 VB and offset, IB, texture 0,
  base vertex, start index, primitive count) plus its occurrence number within the frame.
- On the first frame after a tick, `c0`–`c203` are snapshotted per draw into the "current"
  table (the previous one becomes "previous").
- On every frame the bone palette and `c192`–`c203` are uploaded as
  `lerp(previous, current, alpha)` with `alpha = time since last tick / tick length`. Unmatched
  draws, large jumps and menus (no tick for > 8 frames) use the game's values.
- Registers the game changes by itself between ticks are detected and never interpolated. The
  per-draw matrices contain the camera, which the game updates every frame, so they are
  detected as live almost immediately. **The effective result is smooth skeletal animation.**
  Interpolating those matrices per draw instead makes every object jitter against the camera.

## 8. Borderless windowed

- `CreateDevice` and `Reset` (device vtable 16): `Windowed = TRUE`, refresh rate 0, resolution
  unchanged. If the driver refuses: retry with back buffer `X8R8G8B8`, then the original
  fullscreen parameters.
- The window is restyled to `WS_POPUP` covering its monitor.
- The game creates its window with `WS_EX_TOPMOST` (`CreateWindowExA`, called at
  `0xBAB669`/`0xBAB6E8`/`0xBAB7BF`). The fix strips it at creation and calls
  `SetWindowPos(HWND_NOTOPMOST)`. Clearing the style bit with `SetWindowLong` alone doesn't
  change the z-order state.

## 9. Dead ends and lessons

- **Import-table hooks from `DllMain`** are lost under SteamStub (section 2).
- **Hooking `QueryPerformanceCounter` for telemetry** never fires: the game's timer doesn't call
  it through the executable's imports.
- **Driving ticks by game time** deadlocks on the first hit-stop (game time 0, and the code that
  ends hit-stop lives inside `GameUpdate`).
- **Running stopped-time frames at 30 Hz** makes hit-stop several times longer.
- **Overriding only `0x10D4008`** leaves Havok at 1/60 steps (half speed). The physics step
  comes from `App+0x6C` -> `App+0x70`.
- **Writing `App+0x70` directly** fights the game's rate limiter (physics at 83% speed).
- **Resetting the tick flag at `Present`** before the draws of a pipelined renderer: the
  interpolation tables never rotate.
- **Per-draw interpolation of `c192`+** mixes a stale camera into every object.
- Static call-graph tracing into Havok dead-ends on virtual calls; runtime probes with EBP
  call-chain walking (debug build) were much faster.

## 10. Open issues and useful leads

- **30 FPS areas and cutscenes**: some areas still run at 30 FPS (frame time 33 ms) with the
  cap at 60, e.g. parts of Imperial Raxus Prime and the Death Star. Not investigated yet.
- **Particles/effects**: CPU-built vertex buffers aren't interpolated and may move in 30 Hz
  steps.
- **Camera interpolation**: would require separating the camera from the per-draw matrices.
- Interesting strings and functions:
  - Havok step functions by timer name: `TtSimulate` `0x97B1F0`/`0x97B8F0`/`0x97D9F0`/`0x97DE10`,
    `StIntegrateMotions` `0x99A430`, `StSolve` `0x99A610`, `StSolver 1Cpu` `0x99BF40`,
    `StSolveToi` `0x9DC8A0`
  - `DMM:Step` `0xC618C0`/`0xC62D50`, cloth `0xBDB540`, character controller update `0x66FB40`
  - Script functions `SetTimeScale` / `GetDeltaTime`, tuning value
    `Jedi_Default_SlowMotion_Scale`
  - Hardcoded time constants referenced from code: 1/30 (`0xF51B04`, 14 refs), 30.0
    (`0xF51B10`, 61 refs), 1/60 (`0xF52598`), 60.0 (`0xF522F8`)
