# GTAV.FirstPersonExtended: first-person FOV up to 120° (GTA V Enhanced)

[Versão em português](README.pt-BR.md)

Extends the game's **native** first-person field-of-view slider (Settings > Camera) up to **120° horizontal**. The slider minimum and its linear mapping stay as in the game, and the position is still saved by the game itself.

The same proportion applies to:
- hip aim, aim down sights and aiming through the sight (sniper scopes keep their original behavior);
- the phone while on foot;
- the first-person camera in vehicles.

So the wider view does not clip, the near clip plane is lowered to the engine's minimum (0.01 m) for the on-foot camera and for cabin vehicles (cars, planes, helicopters, blimps, submarines, trains). Bikes, quad bikes and boats/jet skis keep the native near clip.

## Diagonal sprint
In first person, **W+A+Shift** and **W+D+Shift** keep sprinting (in the original game they drop to a run). The character follows the direction of the keys, and footstep sounds and the minimap noise circle work during that sprint.

Unchanged: A or D alone with Shift, S with Shift (the original game already sprints forward with it), walking backwards, third person and the rest of the movement.

**Story mode only. Do not use in GTA Online.**

## Requirements
- GTA V **Enhanced** (tested on 1.0.1158.16, Steam).
- An ASI loader for Enhanced. Any one of these:
  - `xinput1_4.dll` from Ultimate ASI Loader (https://github.com/ThirteenAG/Ultimate-ASI-Loader, x64 build);
  - `xinput1_4.dll` that ships with ScriptHookV (dev-c.com);
  - `dsound.dll` that ships with OpenRPF.

  Use **only one** loader.
- BattlEye turned off through the officially supported flow: the BattlEye option in the Rockstar Games Launcher, or the `-nobattleye` launch option on Steam. With it off, GTA Online is unavailable, as it should be with mods.
- ScriptHookV is **not** required. The mod works with or without it.

## Install
1. Copy the chosen loader to the game folder (where `GTA5_Enhanced.exe` is).
2. Copy `GTAV.FirstPersonExtended.asi` to the same folder. `GTAV.FirstPersonExtended.ini` is optional (see below).
3. Start the game in story mode and set the slider in Settings > Camera.

## Options (`GTAV.FirstPersonExtended.ini`)
Next to the `.asi`. Without the file, or with invalid values (logged), the defaults apply. Changes take effect the next time the game starts.

| Key | Values | Default | What it does |
|---|---|---|---|
| `MaxFov` | whole number, 60 to 140 | 120 | Top of the first-person FOV slider, in **horizontal** degrees. The minimum is the game's. |
| `DiagonalSprint` | 0 or 1 | 1 | 1: W+A / W+D keep sprinting. 0: original game behavior. |

## Uninstall
Delete `GTAV.FirstPersonExtended.asi`, `GTAV.FirstPersonExtended.ini` and `GTAV.FirstPersonExtended.log` from the game folder. If you use no other `.asi` mods, delete the loader too. The mod does not change any original game file.

## Troubleshooting
The mod writes `GTAV.FirstPersonExtended.log` to the game folder on every start. Check it:
- **The first line shows the mod and game versions.** If nothing is written, the loader is not loading the `.asi`. Make sure exactly one loader is installed.
- **Every feature logs `scan …: 1 match(es)` and `patched at …`.** After a Rockstar update a feature may show `not found; stays original`. Only that feature is turned off, the game runs normally, and the patterns need to be regenerated (see `docs/patterns.md`, in Portuguese).
- **`FOV feature disabled`** means the main spot (on-foot FOV) was not found. The mod changes nothing then.
- **Crash or odd behavior:** remove the `.asi` to confirm the cause, and send the `GTAV.FirstPersonExtended.log`.

## How it works
No hooking library and no ScriptHookV: a plain `.asi` that finds each spot in the game's code by byte signature (wildcards, a primary and a fallback pattern per spot, uniqueness checked), verifies the original bytes and then patches. If a signature does not match after a game update, only that feature is disabled and the reason is logged. Details, the list of patterns and how to regenerate them: [`docs/patterns.md`](docs/patterns.md).

## Build (developers)
Cross-compiled on Linux with mingw-w64:
- `make`: release build;
- `make probe`, `make sprintprobe`, `make gateprobe`, `make footstepprobe`: diagnostic builds used to find the spots (not distributed);
- `make failtest`: safe-failure test build (signatures corrupted on purpose);
- `make dumper`: dumps the decrypted game image for offline analysis.

## License and credits
Open source under the MIT license (see `LICENSE`). Author: **paulopatine**. Repository: https://github.com/paulopatine/GTAV.FirstPersonExtended

A fan-made mod, not affiliated with Rockstar Games or Take-Two Interactive. "Grand Theft Auto" is a trademark of its owners.
