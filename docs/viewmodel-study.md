# 02 — Study: the player's own FOV ("viewmodel") in GTA V Enhanced

Analyzed build: **1.0.1158.16** (in-memory dump of 2026-10-31 and 6 probe rounds in the game). The RVAs below are **analysis notes**. The mod's code uses none of them: everything is located by byte pattern.
Legend: **[V]** verified (dump or probe in the game) · **[S]** assumption · **[W]** web source.

## Conclusion (TL;DR)
**It is feasible, and the weapon goes through the same mechanism as the body, because the game already does it.** In first person, the game draws the player's body **and** the weapon (all its parts) with a **private viewport**: a copy of the camera viewport, camera-relative, built only for those draws. It is enough to replace the projection of that copy with the original FOV. The world keeps the high FOV, and the shared viewport object is not touched.

## 1. How the game draws

- [V] **Projection:** `grcViewport::Perspective(fov, aspect, near, far)` (`0x1c22b20`) stores the parameters in the viewport (+0x400…). `0x1c20cd0` builds the two projection matrices (+0x80 and +0x180): `xx = 1/(tan·aspect)` and `yy = 1/tan`. Depth depends only on near and far.
- [V] **Upload to the GPU:** a single function (`0x1c21010`) reads the current viewport (`TLS+0x15c48`) and uploads World·View·Proj. `SetCurrent(vp, regenerate)` = `0x294560`; `SetCameraMtx(vp, …)` = `0x1c21e10`.
- [V] **World:** on the main camera, objects are instanced, with the projection coming from the view constants.
- [V] **Draw lists:** commands are dispatched through a table of 256 executors, by id, in a heap object whose pointer lives in a global (the constructor stores the vtable and that global). Registration: `0x3bc7b0`, which calls `0x13faae0`.
- [V] **First-person body:** command **0x84** (executor `0x3c2760`). Bit 7 of `cmd+0xcd` goes to `TLS+0x712` during the draw and makes the per-component draw (`0x10eb1c0`) skip head, mask, hair and teeth. The ped draw (`0x3d0590`), when `drawData+0x114` has bit `0x80000`, **copies the current viewport, builds a zero-translation camera (`SetCameraMtx`), makes the copy current, draws and restores**.
- [V] **First-person weapon:** command **0x82** (executor `0x3ca330`). Object matrices in the command at +0x38/+0x40 and +0x78/+0x80, 20–35 cm in front of and 20–65 cm below the eye. Pistol: 1 object per frame. Carbine: several parts. Unarmed: none. With bit `0x40` in `drawData+0x108`, the executor does **the same**: viewport copy, `SetCameraMtx`, `SetCurrent`, draw and restore.
  - Probe 9: on the main camera, that test is reached **only** by the weapon draws (~40/s with the pistol, ~180/s with the carbine), and **always** with the private path (`flag40=1, sil=0`).
- [V] **Shadows:** the ped is drawn without the first-person flag, with the light's projection, and does not go through these private viewports. Reflections and cubemaps use other viewports (aspect 1, 90°).
- [W] No public mod has a viewmodel FOV in GTA V. The FOV mods (Rose/Flawless Widescreen, Enhanced Camera, DrDaxxy) only touch the camera.

## 2. Proposed intervention point

The two `call SetCameraMtx` calls on the private copy have the same shape: `call SetCameraMtx; mov rcx,<copy>; mov dl,1; xor r8d,r8d; call SetCurrent`.

| Spot | Pattern (validated: 1 match) | Semantic validation |
|---|---|---|
| Body (`0x3d0ed6`) | `E8 ?? ?? ?? ?? 48 89 E9 B2 01 45 31 C0 E8 ?? ?? ?? ?? 49 89 C7` | both `call`s point to the same `SetCameraMtx`/`SetCurrent` as the weapon spot |
| Weapon (`0x3cafdc`) | `E8 ?? ?? ?? ?? 48 89 D9 B2 01 45 31 C0 E8 ?? ?? ?? ?? 48 89 84 24` | same |

Each `call` (5 bytes, only the rel32 changes) is redirected to a stub. When the conditions hold, the stub rewrites the projection **of the copy** (player vertical FOV = copy FOV ÷ mod scale, through the game's own `Perspective` or `xx/yy` directly) and continues to `SetCameraMtx`. Conditions: first person active, main camera viewport (screen aspect, FOV equal to the one the mod applied), mod scale > 1. Fallbacks: patterns with more context before the call (to do during implementation).

Any missing pattern turns off only that feature, and the rest of the mod continues (project rule).

## 3. What was not measured yet (goes to the implementation tests)
- **Vehicles:** it is not known whether the cabin also goes through the private path. If it does not, the hands would leave the wheel, and the proposal is to turn it off inside vehicles (the mod already knows when the vehicle camera is active).
- **Phone and held objects:** the hypothesis [S] is that they use the same command 0x82 with bit 0x40, like the weapon.
- **Lighting:** the position reconstructed from depth is shifted sideways ×k (k = 1.87 at 120°). That shifts shadows and local lights on the hands and weapon a little; the sun is practically right. It is inherent to any viewmodel in a deferred renderer. It can only be judged by looking.
- **Legs and feet when looking down:** enlarged around the screen center, they do not match the ground. Proposal: reduce k → 1 as the pitch goes past ~45° downward.
- **Cover, ladder, climbing:** the hands touch the world. Proposal: k → 1 with a ramp in those states.
- **TAA/DLSS:** abrupt changes of k can produce one frame of trailing. Hence the ramps.

## 4. Discarded alternatives
- **Separate pass** (clearing depth): needs an extra G-buffer and lighting in the DX12 renderer. Cost and risk out of proportion.
- **Scaling the model** around a point on the line of sight: the same defect as the earlier attempt (hands grow more than the weapon tip).
- **Scale (k, k, 1) in view space:** equivalent to the projection, but distorts normals (~34° error at 45°).
- **Taking the weapon out of the instanced batch:** unnecessary, because the game already draws the first-person weapon through the private path.

## 5. Implementation result (tested by the user)
- Aligned sights and hands on the weapon: **perfect**. Third person intact. The phone works.
- **Vehicles:** the hands left the wheel and the handlebars, so the player FOV is turned off in vehicles (the mod's vehicle camera signal).
- **Shadows:** misaligned (on the ground and on the weapon), as predicted. User's decision: leave it as is and document it.
- **Feet when looking down:** hard to judge with the current vertical FOV. Considered fine.
- **Scope (user's decision):** `PlayerFov=1` applies to every first-person camera. With `UniversalFov=1`, even with `PlayerFov=0`, it applies **while looking through the sight**, so the sights stay aligned without the weapon shrinking.
- **Transition:** 0.25 s, smooth. Approved.
- **Looking through the sight:** the player uses the sight's own FOV (vanilla, saved by the `sight` stub before the mod's adjustment). The weapon looks like vanilla, with the wide world. Approved by the user with `UniversalFov=1` (`PlayerFov=0`) and with `PlayerFov=1`. The transition between targets is smoothed (critically damped spring over the difference between the world FOV and the player FOV).

## 6. Original recommendation
Implement the **player projection in the two private viewports** (body and weapon), behind a new `.ini` option, off by default and fail-safe. Start without the ramps (pitch, cover, vehicle off by a simple rule) and evaluate in the game: sight alignment, hands on the weapon, lighting and feet.

## Probes used (never distributed)
`make vmprobe` (`src/ViewmodelProbe.cpp`, final version): instruments the draw list executor table (swapping entries for stubs that count and look for matrices near the camera) and the bit 0x40 test in the object executor. Earlier rounds: a detour on the upload function (by viewport and first-person flag). Logs: `D:\FirstPersonExtended\backup\2026-10-31_viewmodel\vmprobe{1,2,5,7,8,9}.log`.
