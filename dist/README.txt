TFU-FPS-Fix 1.0
High frame rate fix for Star Wars: The Force Unleashed (PC)
=====================================================================

The PC version of The Force Unleashed is locked to 30 FPS, and the
existing FPS unlock patches break the physics: Force-gripped objects
float upward, ragdolls and debris behave strangely, and so on.

TFU-FPS-Fix runs the game at 60 FPS with physics that behave exactly
like the original 30 FPS game.

Features
--------
- 60 FPS (configurable frame cap)
- Physics fix: the game simulation runs at a fixed 30 Hz, exactly like
  the original game, while the game renders at 60 FPS. Gripped objects
  stay where you hold them, hit-stop and slow motion work as intended.
- Smooth animation between simulation steps (interpolation)
- Borderless windowed mode: instant alt-tab, other windows can be shown
  on top of the game
- SWTFU.exe is never modified. Everything is applied in memory and can
  be removed by deleting two files.

Requirements
------------
- Star Wars: The Force Unleashed - Ultimate Sith Edition, PC,
  version 1.2 (the current Steam version). Tested on Steam.
- The GOG and retail versions use the same game code and should work,
  but have not been tested. The fix checks the game code before
  patching and does nothing if it doesn't match.
- Do NOT combine with the old "FPS cap fix" executables
  (40/50/60/Unlimited FPS SWTFU.exe). Use the original SWTFU.exe.

Installation
------------
1. Find the game folder. In Steam: right-click the game > Manage >
   Browse local files.
2. Copy dinput8.dll and tfufix.ini into that folder (next to SWTFU.exe).
3. Start the game as usual.

Uninstallation
--------------
Delete dinput8.dll and tfufix.ini (and tfufix.log, if present) from the
game folder.

Settings
--------
All settings are in tfufix.ini, with explanations. The defaults are the
recommended ones:

  FPSCap=60          frame cap (60 is the tested value)
  FixedTimestep=1    the physics fix
  Enabled=1          smooth animation (interpolation)
  Borderless=1       borderless windowed (0 = original fullscreen)

Known limitations
-----------------
- Some areas and cutscenes are still limited to 30 FPS by the game
  itself (for example parts of Imperial Raxus Prime and the Death Star).
- The game world is simulated at 30 Hz. Character animation is
  smoothed, but some things (for example particles and certain effects)
  may still move in 30 Hz steps.
- Smoothing adds about one simulation step (33 ms) of display delay.
- Frame caps other than 60 have not been tested.

Troubleshooting
---------------
- tfufix.log in the game folder shows what the fix did at startup.
  "game code verified" means the fix is active.
- If the game misbehaves, set the feature in question to 0 in
  tfufix.ini, or uninstall the fix by deleting dinput8.dll.
- Some antivirus programs are suspicious of unsigned DLL files that
  load into games. The full source code is available on the project's
  GitHub page, so you can check it or build it yourself.

How it works (short version)
----------------------------
The game loads dinput8.dll from its own folder. The fix forwards all
DirectInput calls to the real Windows DLL, waits until the Steam DRM has
decrypted the game code, verifies the code, and then patches it in
memory:
- the 30 FPS cap is raised;
- the game's main update is driven by a fixed 30 Hz clock, with every
  timer value the game uses (including the one that drives Havok
  physics) set exactly as in the original game;
- at the Direct3D 9 level, animation is blended between the last two
  simulation steps on the frames in between;
- the game window is turned into a borderless window.

Credits
-------
Created by Eric (Goat), with Claude (Anthropic).
Released under the MIT License (see LICENSE).
