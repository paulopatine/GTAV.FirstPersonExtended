# GTAV.FirstPersonExtended patterns

Validated on **GTA V Enhanced 1.0.1158.16** (this section and the following ones) and on **GTA V Legacy 1.0.3889.0** (its own section at the end). Every spot has a primary pattern (`*_full`) and a fallback (`*_tail`). In this version both were checked to have **exactly 1 match** in the `.text` of the in-memory image. Offline check: `tools/re/scan.py` over the dump. Runtime check: the `scan …: 1 match(es)` lines in `GTAV.FirstPersonExtended.log`.

Rules that apply to every spot:
- **No fixed address or offset in the code.** Struct offsets (metadata, camera fields) are read from the matched instructions themselves.
- **Semantic validation after the match.** A unique pattern is not enough: each spot also checks constants (10.0, 1.0, 130.0, 0.01, mask `0x702`), registers and the destination field.
- **Conditional write.** A patch is applied only if the current bytes equal the ones read (`MemoryPatch::Replace`).
- **Isolated failure.** If anything fails, the log records the pattern and the reason, and **only that feature** is turned off. Exception: the on-foot spot is the base of the FOV feature. Without it the whole mod stays inactive and the game is not touched.

## Spots

| Feature | Patterns | What it finds | Semantic anchors (validation) |
|---|---|---|---|
| On-foot FOV (base) | `onfoot_fov_full` / `_tail` | `movss xmm2,[meta+max]` of the formula `min + (max−min)·clamp(pref/10)` | before it: `cvtsi2ss x,[rip+pref]; divss x,[rip+10.0]` (gives the slider address) and `movss xmm1,[meta+min]` with the same base register |
| Hip aim (not through the sight) | `hipaim_fov_*` | store of the weapon aim-range lerp into the camera target FOV | the slider read before it is the **same** address as the on-foot spot; it gives the target FOV offset |
| ADS | `ads_fov_*` | read of the ADS target FOV in the shared aim object | followed by a read of the camera's current FOV, `subss`/`mulss` (interpolation) |
| ADS through the sight | `sight_check_*` | `ucomiss x,[rip+1.0]; jae store`: the jump destination is the spot | constant 1.0; the destination is a `movss` store of the same register into the **same** target FOV field |
| Phone (fixed-FOV state) | `state_fov_*` | `metadata->stateFov` stored as the target FOV | load and store through the same register; destination = target FOV field |
| Vehicle FOV | `vehicle_fov_*` | load of the base FOV of the mounted camera (first person in a vehicle) | followed by the clamp `[1.0, 130.0]`; the store 0x29 bytes later gives the FOV offset in the frame |
| Vehicle near clip | `vehicle_nearclip_*` | `maxss x,[rip+0.01]; movss [cam+near],x` of the mounted camera | constant 0.01; `near` = frame FOV + 4 |
| Vehicle type | `vehicle_type_check_*` | the mounted camera's own aircraft test | the game's mask `0x702` (plane, helicopter, blimp, autogyro) and entity type 3; it gives the offsets of the vehicle, the entity type and the vehicle type |
| On-foot near clip | `camera_nearclip_*` | generic camera near clip (`metadata->nearClip`) | constant 0.01; `near` = frame FOV + 4; `mov rcx,[cam+meta]; add rcx,imm8` before it. Acts only when `rcx` = the on-foot camera metadata |

Dependency order inside `FieldOfView::Apply()`:
- **on foot** provides the slider, `min`/`max` and the metadata pointer;
- **hip aim** provides the target FOV offset, used by aiming and the phone;
- **vehicle** provides the frame FOV offset, used by both near clips.

## How to regenerate after an update

1. **Dump the in-memory image.** The EXE on disk has an encrypted `.text` (entropy ~8.0), so the analysis runs on the loaded image.
   - Build with `make dumper`, which produces `build/GTAV.FirstPersonExtended_ImageDump.asi`.
   - Copy the `.asi` to the game folder next to the loader and open story mode, with BattlEye off through the official flow (`-nobattleye` or the Rockstar Games Launcher option).
   - It writes `GTAV.FirstPersonExtended_image.bin` (layout RVA = file offset) and `GTAV.FirstPersonExtended_dump.log`.
   - Remove the dumper afterwards.
2. **Preparation.** Linear disassembly of `.text`: `objdump -D -b binary -m i386:x86-64 -M intel --adjust-vma=0x1000 text.bin`. Use `tools/re/relib.py` for function bounds (`.pdata`) and xrefs.
3. **Test the current patterns:** `python3 tools/re/scan.py "<pattern>"`. If any does not have exactly 1 match, find the spot again through each feature's anchors:
   - **on foot:** the `g_prefs` array is the `lea` base with the most indexed direct reads. `PREF_FPS_FIELD_OF_VIEW` = `joaat` hash `0xa114b6e8`; its index comes from the jump table of the index→hash switch (133 today). The `cvtsi2ss` readers of that address lead to the formula;
   - **hip aim, ADS, sight view, phone:** all writers of the on-foot camera's target FOV field (`grep "+0x<target>\],xmm"`);
   - **vehicle:** readers of the prefs `PREF_FPS_VEH_AUTO_CENTER` and `PREF_FPS_RELATIVE_VEHICLE_CAMERA_DRIVEBY_AIMING` point to the mounted camera's `Update`, which calls the FOV function (clamp 130.0) and the near clip function (mask `0x702`);
   - **on-foot near clip:** the generic function with `maxss …0.01` storing frame FOV + 4, called by every camera.
4. **When static analysis is not enough,** build `make probe`. It counts, per second, the executions of the FOV clamp instructions (130.0) and the near clip stores, with the last value written. Run scenarios separated by pauses (Esc) and compare. This is how real ADS, the phone and the vehicle cameras were told apart.
5. **New patterns:** copy the bytes of the new spot, replace displacements, RIP-relative addresses and jumps with `??`, and keep opcodes, registers and structure. Validate with `scan.py`, `make`, an installation checked by hash and the log.

## Diagonal sprint (W+A / W+D in first person)

Implemented in `src/DiagonalSprint.cpp`, independent of the FOV feature. In the original game the cause is **three decisions** that drop a sprint to a run when A or D is held, plus a fourth one that silences the footsteps. The first three patches are applied **together or not at all** (with fewer, the character sprints straight or stops sprinting); the fourth is a separate feature.

| Patch | Patterns | What it finds | Why | Anchors |
|---|---|---|---|---|
| 1. Ramp | `diagonal_ramp_full` / `_tail` | `or r12b,sil ; xor r12b,1` (7 bytes) | `noLateralInput = !(left \| right)` clears the "forward" flag and the ramp. It becomes a stub that also accepts **W** (`MOVE_UP_ONLY`) as "no lateral input" | `lea rcx,[r13+disp]` and the button-test call right after |
| 1b. Button reader | `control_check_left` / `_right` | the game's sequence that tests `MOVE_LEFT_ONLY`/`RIGHT_ONLY` (`getControl`, table, `isDown`) | the stub calls the **same game functions** for W; the three addresses come from the sequence itself | `mov edx,0x22` (or `0x23`), `lea rbx,[table]`, three calls; the table is outside `.text` |
| 2. Input | `diagonal_input_full` / `_tail` | `cmp byte [obj+disp],0 ; je forceForward` | the game replaces the input with "straight ahead" when an internal boolean is off. It becomes a stub that keeps the input if the boolean is on **or** the input points forward (`xmm9 > 0`) | `mulss xmm6,xmm6 … ucomiss … jne … jp … mov sil,1` right after |
| 3. Heading | `diagonal_heading_full` / `_tail` | `test sil,sil ; je skip` before the desired heading is stored | the game turns the body toward the camera while sprinting. It becomes a stub that skips that overwrite if the input points forward. It also keeps the `diagonalSprinting` flag (forward + lateral) used by patch 4 | `mov rcx,rdi ; movaps xmm1,xmm10 ; call ; movss [r14+disp],xmm10 ; test byte [rbx+disp],1` |
| 4. Footsteps | `diagonal_steps_full` / `_tail` | `ucomiss xmm6,[r14] ; jbe` of the footstep detector | the game registers a step only if the foot is low and its world speed is below ~4 m/s. Sprinting diagonally the planted foot slides (~5.3 m/s at 45°) and never passes. With `diagonalSprinting` and only for the player, the limit rises to 9 m/s (`kSlideSpeedLimit`) | `or eax,r13d ; mov [rsi+rbp*4+disp],eax ; cmp byte [rsi+disp],0 ; je` |
| 4b. Player test | `diagonal_steps_player` | `mov rcx,[helper+X] … movzx eax,byte [rcx+Y] ; and al,3 ; cmp al,2` | gives the two offsets (`X` = ped, `Y` = flags) the stub uses to restrict itself to the player; no fixed offset in the code | `cmp edi,1 ; jne ; movzx` |

For patches 2 and 3 and for the flag, the game uses `xmm8` = lateral input and `xmm9` = forward input (positive = forward) at the patch point.

Specific rules:
- If any spot of patches 1 to 3 is missing or does not validate, **none of the three** is applied and the log says `a site was not found or did not validate; stays original`.
- Patch 4 is only attempted after the first three. If it is missing, the log says `diagonal footsteps: site not found …; sprint stays silent` and the diagonal sprint works, only without sound.
- `S+Shift` already sprints forward in the original game (the input is replaced by "forward"); the patches do not change that, because they only keep the input when it points forward.

### How to regenerate after an update

1. Dump the image as above (`make dumper`) and use `tools/re/relib.py` for function bounds.
2. **Ramp and input:** `make sprintprobe` counts, per key combination, the loads of the movement pace constants (2.0 and 3.0). The code that runs on W+Shift and stops on W+A+Shift is in the generated `.csv`. The following functions (ramp decision, `getControl`/`isDown`) come from there.
3. **Heading and footsteps:** `make gateprobe` records, before the heading store, `sil`, the lateral and forward input, the camera heading and the stored one. `make footstepprobe` counts the functions of the footstep chain and records **both sides** of the detector's comparisons (foot value and foot speed) per key combination.
4. `tools/re/sigmake.py <rva>` generates a wildcard pattern for the start of a function, extending it until it is unique.
5. Validate every new pattern with `tools/re/scan.py`, install with a hash check and read the log.

### Diagonal sprint fail-safe test

`make failtest` also corrupts the `diagonal_heading_*` patterns. Expected log result:
- `scan diagonal_heading_full (corrupted on purpose): 0 match(es)` and the same for `_tail`;
- `diagonal sprint: a site was not found or did not validate; stays original`;
- no `diagonal sprint: ramp patched` line and no `diagonal footsteps` line.

In the game: W+A+Shift goes back to **running** (not sprinting), as in the original; the FOV and everything else keep working.

## Fail-safe test (FOV)

`make failtest` builds a version with the ADS patterns (and the diagonal sprint heading ones, see above) corrupted on purpose. Expected log result:
- `scan ads_fov_full (corrupted on purpose): 0 match(es)` and the same for `_tail`;
- `FOV ADS: not found; stays original`;
- every other spot applied normally, no crash.

In the game, ADS goes back to the native FOV and the rest keeps working.

## Player FOV (`PlayerFov`, experimental)

Implemented in `src/PlayerFov.cpp`, optional (`PlayerFov=1`) and dependent on the on-foot FOV. In first person the game draws the body (draw list command 0x84) and the weapon in hand (command 0x82) with a **private copy of the viewport**: it copies the current viewport, calls `SetCameraMtx(copy, …)` (camera-relative), `SetCurrent(copy, regenerate)`, draws and restores. The mod diverts the two `SetCameraMtx` calls to a stub. The stub divides the two FOV fields **of the copy** by the slider widening (the same scale as the rest of the mod) and calls the game's own projection builder. The shared viewport, the world, shadows, reflections and third person do not go through there. Full study: `docs/viewmodel-study.md`.

| Spot | Patterns | What it finds | Semantic anchors (validation) |
|---|---|---|---|
| Projection builder | `player_projection_full` / `_tail` | `mov rsi,rcx` of the builder of the viewport's two projection matrices | loads the constant π/360 (0.00872665); it gives the aspect, FOV and second FOV offsets (`mulss xmm8,[rsi+…]`, single match); the function start (via `.pdata`) is less than 0x80 bytes before |
| First-person body | `player_body_full` / `_tail` | `call SetCameraMtx` on the private copy of the ped draw | the target is a function start; 13 bytes later, `call SetCurrent`; both equal to those of the weapon spot |
| First-person weapon | `player_weapon_full` / `_tail` | `call SetCameraMtx` on the private copy of the object executor | same |

Specific rules:
- **Body and weapon together or not at all.** If the weapon fails after the body was applied, the body is restored.
- The weapon is adjusted only if the first-person body was drawn less than 250 ms ago. That excludes third person, where the body never goes through this path (measured with `make vmprobe`).
- Square viewports (aspect 1: cubemaps, reflections) and FOVs outside (1°, 130°) are left untouched.
- **Vehicles:** the vehicle FOV stub counts every update of the first-person vehicle camera (`lock inc`, flags preserved). While that count changes (last 250 ms), the player keeps the world's FOV. Without that spot, `PlayerFov` is not applied.
- **Only through the sight:** with `UniversalFov=1` and `PlayerFov=0`, the `sight` stub counts its updates the same way, and the player FOV applies only while that count changes. Without the `sight` spot, this mode is not applied.
- The log shows every transition: `player FOV: on (looking through the sight)`, `player FOV: off`.
- **Target FOV:** through the sight, the sight's own FOV as the game set it. The `sight` stub saves the register before adjusting it, so the result equals vanilla. For the other cameras, the camera FOV divided by the slider widening.
- **Smooth transition:** the difference between the world FOV and the player FOV is smoothed with a critically damped spring (`kSmoothSeconds` = 0.08 s, settles in ~0.25 s, no overshoot), shared by all render threads. When off, the difference is 0 and the player follows the world exactly. The sight signal stays active for 100 ms after the last update (`kCameraRecentMs`); measured: no gaps while looking through the sight.

### How to regenerate after an update
1. Dump (`make dumper`) and `tools/re/relib.py`.
2. **Builder:** the only reader of π/360 that builds two projections. Callers of `tanf` with `mulss …,[rip+π/360]`; it starts with `cmp byte [rcx+flagZ],0` and reads near, far, aspect and FOV.
3. **`SetCurrent`:** stores the viewport at `TLS+…` (the same field `SetWorldMtx` compares) and, if asked, regenerates. **`SetCameraMtx`:** ends by calling the view·proj composition and the regeneration.
4. **Body and weapon spots:** among the callers of `SetCameraMtx`, the two followed by `mov rcx,<copy>; mov dl,1; xor r8d,r8d; call SetCurrent` in draw functions. The ped one tests a bit of the draw data (`0x80000`) and the object one another (`0x40`). To confirm in the game, use `make vmprobe` (it instruments the draw list executor table and the `0x40` bit test).
5. Validate with `tools/re/scan.py` (exactly 1 match each), install with a hash check and read the log (`player FOV: body patched…`, `first body/weapon draw narrowed…`).

### Fail-safe test
`make failtest` also corrupts `player_weapon_*`. Expected log (with `PlayerFov=1`): `scan player_weapon_full (corrupted on purpose): 0 match(es)`, `player FOV: a private viewport site was not found or did not validate; stays original` and no `player FOV: body patched` line.

## GTA V Legacy (1.0.3889.0)

The mod detects the edition from the executable name (`GTA5.exe` = Legacy, `GTA5_Enhanced.exe` = Enhanced) and writes `(Legacy)` or `(Enhanced)` on the first log line. The game's code is the same on both editions, but Legacy was compiled differently (MSVC, other registers and block order). So every spot has its own patterns with the `_legacy` suffix, and the stubs change only the registers and the neighbouring instructions. The final behavior is the same. All the patterns below were validated on the Legacy dump with **exactly 1 match** (the mod's own scanner, `tests/ScanDump.cpp`, runs over any dump: it reads `.text` from the section table). They were also confirmed in the game's log.

Loader on Legacy: `dinput8.dll` from Ultimate ASI Loader (the same file as `xinput1_4.dll`, renamed).

### FOV and player FOV

| Spot | Patterns | Difference from Enhanced |
|---|---|---|
| On foot (base) | `onfoot_fov_legacy_*` | the slider read lives in a small function (`movd`, `cvtdq2ps`, `mulss 0.1`). The spot is the load of `max` in the caller's formula `min + (max − min)·t` |
| Hip aim, ADS, sight view, phone | `hipaim_fov_legacy_*`, `ads_fov_legacy_*`, `sight_store_legacy_*`, `state_fov_legacy_*` | the phone and the fixed-FOV state use a stub that saves and restores `xmm0` on the stack |
| Vehicle, type, near clips | `vehicle_fov_legacy_*`, `vehicle_type_check_legacy_*`, `vehicle_nearclip_legacy_*`, `camera_nearclip_legacy_*` | the vehicle FOV is reloaded in a transition right after the clamp. The same adjustment is applied there too (`vehicle transition` in the log) |
| Player FOV | `player_perspective_legacy_*`, `player_body_legacy_*`, `player_weapon_legacy_*` | the projection does not have Enhanced's two matrices: the viewport has FOV, aspect, near, far and a perspective flag, and is built by `Perspective(vp, fovY, aspect, near, far)`. The stub calls that function on the private copy |

### Diagonal sprint

The same four decisions as Enhanced, in the same order. Equivalent functions, checked side by side:
- movement dispatcher: Enhanced `0x18bf6e0`, Legacy `0xb09db8`;
- lateral veto: Enhanced `0xd87300`, Legacy `0x72dd80`;
- sprint movement: Enhanced `0x18c1b90`, Legacy `0xafa65c`.

| Patch | Patterns | What it finds on Legacy |
|---|---|---|
| 1. Ramp | `diagonal_ramp_legacy_*` | the 16-byte block that stores `noLateralInput = !(left \|\| right)` into `r15b` (`test bl,bl ; jne ; test al,al ; jne ; mov r15b,1 ; jmp ; xor r15b,r15b`). The stub rewrites the **flag itself** (`r15b \|= isDown(MOVE_UP_ONLY)`), because the game reads it **three times** afterwards: in the ramp, in the run/sprint choice and in keeping the sprint |
| 1b. Button reader | `control_check_legacy_left` / `_right` | the second of the three key tests (`getControl`, `isDown`, 0.5 threshold). The three addresses come from the sequence, and the threshold is checked (= 0.5) |
| 2. Input | `diagonal_input_legacy_*` | `cmp byte [rax+disp],r12b ; je forceForward`. Before patching, the mod checks that the jump destination is `movaps xmm15,xmm7 ; movaps xmm14,xmm12` (lateral 0, forward 1). That guarantees `xmm14` is the forward input |
| 3. Heading | `diagonal_heading_legacy_*` | `test dil,dil ; je skip` (long jump). The `diagonalSprinting` flag uses `xmm15` (lateral) |
| 4. Footsteps | `diagonal_steps_legacy_*`, `diagonal_steps_legacy_player` | `comiss xmm6,[r12] ; jbe` (11 bytes). Player test: `mov rdx,[rbx+ped] … mov al,[rdx+flags] ; test al,1 ; jne ; test al,2 ; je`. The stub uses `rax` (`rcx` is live here) |

**Lesson recorded:** bit 6 of `[ped+0x5e0]`, which the dispatcher tests to choose the non-sprint movement, does **not** need a patch. It only reflects the veto: with the `noLateralInput` flag fixed in all three uses, the game no longer sets that bit on diagonals. The first attempts on Legacy changed only the first use of the flag, or ignored the bit in the dispatcher. Ignoring the bit made S walk forward.

### Fail-safe test on Legacy

`make failtest` also corrupts `diagonal_heading_legacy_*`. Result measured on 2026-11-01: `scan diagonal_heading_legacy_full (corrupted on purpose): 0 match(es)` (same for `_tail`), `diagonal sprint: a site was not found or did not validate; stays original`, FOV working and no crash.

## Reloading while looking through the sight (player FOV)

Problem: reloading while holding the aim button keeps the camera in the "looking through the sight" state. The sight signal stayed active, and the body and weapon kept the sight's narrow FOV: the reload animation showed zoomed in.

The fix has two parts:
1. **Reload signal (`sight` stub, game thread).** The stub follows the game's own pointers: camera → character → weapon manager → weapon object → weapon. While the weapon state is "reloading", it counts its own signal (`SightReloading`) instead of the sight signal. It also stores the camera and the weapon object for part 2.
2. **Return to the sight (`PlayerFov`, render thread).** Each reload opens an episode during which the sight FOV is off. The end of the "reloading" state is not a good criterion on its own. Measured with `make sprintprobe` (recording the weapon position relative to the camera every 33 ms), the state ends 0.5 s to 1.1 s before the weapon is back at the sight, depending on the weapon. A reload interrupted by a dodge roll can also leave the state set with the weapon already at the sight. The episode ends:
   - when the weapon is back within 1.5 cm of the place it had at the sight (learnt while looking through the sight, only with the weapon still: less than 1 cm in 0.15 s), after the state has ended;
   - or when the weapon settles (stops moving) after the state ends, even without a learnt place;
   - or if the weapon never leaves the sight within 0.4 s, or stays at it for 0.4 s with the state set (stuck state);
   - at most 3 s after the state ends. Without the weapon position, a fixed 0.4 s delay applies.

   The log records one line per reload with the reason (`weapon back at the sight`, `weapon settled`, `weapon never left the sight`, `stale reload state…`, `time limit…`).

| Spot | Patterns (Enhanced / Legacy) | What it finds | Validation |
|---|---|---|---|
| Camera's ped | `aim_ped_*` / `aim_ped_legacy_*` | prologue of the aim camera: `and byte [rcx+flags],0xDF ; mov ped,[rcx+X]` | `mov r64,[rcx+disp32]`; the fallback is unique inside the aim camera function (the same one as the `sight` spot, via `RtlLookupFunctionEntry`) |
| Weapon manager and getter | `aim_weapon_*` / `aim_weapon_legacy_*` | `mov r,[ped+Y] ; test ; je ; mov rcx,r ; call getter` | the base is the ped register; the getter is checked by the shape of its fast path (`mov rax,[rcx+o] ; … mov r,[rax+w]`), which gives the object and weapon offsets |
| Weapon state | `weapon_reload_state_*` / `weapon_reload_state_legacy_*` | `cmp byte [rcx+S],RELOADING` of the reload state updater (Legacy also has the store `mov byte [rcx+S],3`) | the "reloading" value is the comparison immediate; in the full patterns, the store to the same field is checked |
| Camera position and direction | `camera_frame_*` / `camera_frame_legacy_*` | end of the on-foot camera update: `lea …,[cam+pos]` and `[cam+forward]` passed to a callback | same function as the spot below; the position field must be the same in both |
| Entity position | `entity_position_*` / `entity_position_legacy_*` | `movaps …,[cam+pos] ; … subps …,[entity+P]` (camera position relative to the attached entity) | offsets aligned to 16 |

Current values, the same on both editions except the state: ped `+0x210`, weapons `+0x10b8`, object `+0x78`, weapon `+0x320`, state `+0x200` (Enhanced) / `+0x1f0` (Legacy) `== 2`, camera position `+0x60` and forward `+0x40`, entity `+0x90`. Because the mod reads these game pointers from the render thread, it uses `ReadProcessMemory`: a stale pointer only makes the read fail, without crashing the game.

**Fail-safe:** `make failtest` corrupts `weapon_reload_state_*` on both editions. Measured on Enhanced: `0 match(es)`, `FOV sight: reload check not found; aiming while reloading keeps the sight FOV`, the rest applied, no crash. Without the position patterns, the return uses the fixed delay and the log says `weapon pose not found`.
