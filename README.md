# GTAV.FirstPersonExtended: first-person FOV up to 120° (GTA V Enhanced and Legacy)

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
- GTA V **Enhanced** (tested on 1.0.1158.16, Steam) or GTA V **Legacy** (tested on 1.0.3889.0, Steam). The mod detects the edition by itself; every feature works on both.
- An ASI loader. On **Legacy**: `dinput8.dll` from Ultimate ASI Loader (the usual Legacy loader). On **Enhanced**, any one of these:
  - `xinput1_4.dll` from Ultimate ASI Loader (https://github.com/ThirteenAG/Ultimate-ASI-Loader, x64 build);
  - `xinput1_4.dll` that ships with ScriptHookV (dev-c.com);
  - `dsound.dll` that ships with OpenRPF.

  Use **only one** loader.
- BattlEye turned off through the officially supported flow: the BattlEye option in the Rockstar Games Launcher, or the `-nobattleye` launch option on Steam. With it off, GTA Online is unavailable, as it should be with mods.
- ScriptHookV is **not** required. The mod works with or without it.

## Install
1. Copy the chosen loader to the game folder (where `GTA5_Enhanced.exe` or, on Legacy, `GTA5.exe` is).
2. Copy `GTAV.FirstPersonExtended.asi` to the same folder. `GTAV.FirstPersonExtended.ini` is optional (see below).
3. Start the game in story mode and set the slider in Settings > Camera.

## Options (`GTAV.FirstPersonExtended.ini`)
Next to the `.asi`. Without the file, or with invalid values (logged), the defaults apply. Changes take effect the next time the game starts.

| Key | Values | Default | What it does |
|---|---|---|---|
| `MaxFov` | whole number, 60 to 140 | 120 | Top of the first-person FOV slider, in **horizontal** degrees. The minimum is the game's. |
| `DiagonalSprint` | 0 or 1 | 1 | 1: W+A / W+D keep sprinting. 0: original game behavior. |
| `UniversalFov` | 0 or 1 | 0 | 1: hip aim (aiming without the sights) and aim down sights use exactly the slider's FOV (at most `MaxFov`), the same as walking, instead of scaling their own. The phone and vehicle cameras always stay proportional; sniper scopes stay native. 0: every camera keeps its own FOV, widened in proportion. |
| `AdsFov` | 0 or 1 | 1 | 1: while looking through the weapon's sight, the body and the weapon use the game's original FOV, even with the slider at its maximum: the weapon stays close to the camera at its original size and the sights stay aligned, while the world keeps the wide FOV. While reloading, the weapon leaves the sight and uses the world's FOV until it is back at the sight. Works with `UniversalFov` 0 or 1. 0: the weapon uses the world's FOV and looks smaller at wide FOV. |
| `PlayerFov` | 0 or 1 | 0 | **Experimental.** 1: in first person, the character's body and the weapon in the hands use the FOV the same camera had in the original game (arms and weapon at their original size); the world keeps the slider's FOV. Not inside vehicles. The character's and the weapon's shadows do not follow the larger size (known effect). Needs the mod's FOV feature. 0: body and weapon use the world's FOV (except while looking through the sight with `AdsFov=1`). |

## Known issues
These come from drawing the character's body and weapon with a different FOV than the world. They affect `PlayerFov=1` all the time, and `AdsFov=1` while looking through the weapon's sight.
- **Muzzle effects misaligned.** The muzzle flash and smoke are drawn with the world's FOV, so they do not line up with the weapon. With `PlayerFov` this is very visible when firing from the hip. With either option it shows when aiming down sights with weapons that have no suppressor and a modified sight.
- **Shadows on the character and weapon misaligned**, most visibly on Enhanced with ray-traced shadows. Turning ray-traced shadows off is recommended when playing with these options.

## Uninstall
Delete `GTAV.FirstPersonExtended.asi`, `GTAV.FirstPersonExtended.ini` and `GTAV.FirstPersonExtended.log` from the game folder. If you use no other `.asi` mods, delete the loader too. The mod does not change any original game file.

## Troubleshooting
The mod writes `GTAV.FirstPersonExtended.log` to the game folder on every start. Check it:
- **The first line shows the mod and game versions.** If nothing is written, the loader is not loading the `.asi`. Make sure exactly one loader is installed.
- **Every feature logs `scan …: 1 match(es)` and `patched at …`.** After a Rockstar update a feature may show `not found; stays original`. Only that feature is turned off, the game runs normally, and the patterns need to be regenerated (see `docs/patterns.md`).
- **`FOV feature disabled`** means the main spot (on-foot FOV) was not found. The mod changes nothing then.
- **Crash or odd behavior:** remove the `.asi` to confirm the cause, and send the `GTAV.FirstPersonExtended.log`.

## How it works
No hooking library and no ScriptHookV: a plain `.asi` that finds each spot in the game's code by byte signature (wildcards, a primary and a fallback pattern per spot, uniqueness checked), verifies the original bytes and then patches. If a signature does not match after a game update, only that feature is disabled and the reason is logged. Details, the list of patterns and how to regenerate them: [`docs/patterns.md`](docs/patterns.md).

## Build (developers)
Cross-compiled on Linux with mingw-w64:
- `make`: release build;
- `make probe`, `make sprintprobe`, `make gateprobe`, `make footstepprobe`, `make vmprobe`: diagnostic builds used to find the spots (not distributed);
- `make failtest`: safe-failure test build (signatures corrupted on purpose);
- `make dumper`: dumps the decrypted game image for offline analysis.

## License and credits
Open source under the MIT license (see `LICENSE`). Author: **paulopatine**. Repository: https://github.com/paulopatine/GTAV.FirstPersonExtended

A fan-made mod, not affiliated with Rockstar Games or Take-Two Interactive. "Grand Theft Auto" is a trademark of its owners.
