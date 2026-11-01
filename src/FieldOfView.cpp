// First-person FOV, as found in Enhanced 1.0.1158.16 and Legacy 1.0.3889.0 (details and
// regeneration steps: docs/patterns.md). Legacy is the same game code compiled differently, so
// each site has Legacy signatures of its own, validated against the Legacy shape of the code.
//
// On foot the game maps the menu slider like this:
//   t   = clamp(PREF_FPS_FIELD_OF_VIEW / 10.0, 0, 1)        // saved slider position 0..10
//   fov = meta->minFov + (meta->maxFov - meta->minFov) * t   // vertical degrees
// We redirect only the load of meta->maxFov to a value we own, so the minimum, the saved
// position and the linear mapping stay native; only the top of the range moves.
//
// Aiming, aim-down-sights, the phone and the in-vehicle first-person camera never followed
// the slider in the original game. At the player's request they are scaled by how much the
// slider now widens the on-foot view at the same position: scale = fovNew(t) / fovOriginal(t).
// Sniper scopes use their own camera and stay native. The wider view also needs a closer
// near plane (see "Near clip" below).
//
// Optional "universal" mode: the aiming cameras (hip aim and aim down sights, with or without
// looking through the sight) do not scale their own FOV, they use the FOV the slider gives on
// foot (the stub replaces the value instead of multiplying it). The phone and vehicles keep the
// proportional scale.
#include "FieldOfView.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

#include "CodeBlock.h"
#include "GameImage.h"
#include "Log.h"
#include "Pattern.h"

namespace {

constexpr float kFallbackAspectRatio = 16.0f / 9.0f;
constexpr float kSliderSteps = 10.0f;
constexpr int kMovssLength = 8;  // F3 0F 10/11 ModRM disp32
constexpr int kSliderContextBytes = 64;

struct Signature {
    const char* name;
    const char* bytes;
    int patchOffset;  // start of the 8-byte movss this site patches
};

// Primary signatures cover the whole block; fallbacks only its tail, to survive the compiler
// reshuffling unrelated instructions. Every hit is re-validated semantically before use.
constexpr Signature kOnFootSignatures[] = {
    {"onfoot_fov_full",
     "F3 0F 2A 05 ?? ?? ?? ?? F3 0F 5E 05 ?? ?? ?? ?? F3 44 0F 10 05 ?? ?? ?? ?? 41 0F 28 C8 "
     "F3 0F 5D C8 F3 0F C2 C6 01 0F 55 C1 48 8B 86 ?? ?? ?? ?? F3 0F 10 48 ?? "
     "F3 0F 10 90 ?? ?? ?? ?? F3 0F 5C D1 F3 0F 59 D0 F3 0F 58 D1",
     53},
    {"onfoot_fov_tail",
     "48 8B ?? ?? ?? ?? ?? F3 0F 10 48 ?? F3 0F 10 90 ?? ?? ?? ?? F3 0F 5C D1 F3 0F 59 D0 "
     "F3 0F 58 D1 F3 0F 11",
     12},
};
// Legacy: t comes from a call, then `max - min` is computed from the maximum first.
constexpr Signature kLegacyOnFootSignatures[] = {
    {"onfoot_fov_legacy_full",
     "48 8B ?? ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? 48 8B D6 48 8B CF F3 0F 10 ?? ?? ?? ?? ?? "
     "F3 0F 5C ?? ?? F3 0F 59 C1 F3 0F 58 ?? ?? F3 0F 11",
     21},
    {"onfoot_fov_legacy_tail",
     "F3 0F 10 ?? ?? ?? ?? ?? F3 0F 5C ?? ?? F3 0F 59 C1 F3 0F 58 ?? ?? F3 0F 11 ?? ?? ?? ?? ?? E8", 0},
};

// Hip aim in first person (aiming without the sights): lerp of the weapon's aim FOV range.
constexpr Signature kHipAimSignatures[] = {
    {"hipaim_fov_full",
     "F3 0F 2A 15 ?? ?? ?? ?? F3 0F 5E 15 ?? ?? ?? ?? F3 0F 10 1D ?? ?? ?? ?? F3 0F 5D DA "
     "0F 57 E4 F3 0F C2 D4 01 0F 55 D3 F3 0F 5C C8 F3 0F 59 CA F3 0F 58 C8 0F 28 D1 "
     "F3 0F 11 96 ?? ?? ?? ??",
     54},
    {"hipaim_fov_tail", "F3 0F 5C C8 F3 0F 59 CA F3 0F 58 C8 0F 28 D1 F3 0F 11 96 ?? ?? ?? ??", 15},
};
constexpr Signature kLegacyHipAimSignatures[] = {
    {"hipaim_fov_legacy_full",
     "E8 ?? ?? ?? ?? F3 44 0F 5C C6 F3 41 0F 59 C0 F3 0F 58 C6 F3 0F 11 ?? ?? ?? ?? ?? F3 0F 10", 19},
    {"hipaim_fov_legacy_tail",
     "F3 0F 10 ?? ?? ?? ?? ?? 0F 2F 35 ?? ?? ?? ?? F3 0F 10 ?? ?? ?? ?? ?? 76 ?? F3 44 0F 10", 0x3E},
};

// Aim down sights: the camera eases towards a target FOV held by a shared aiming object.
#ifdef FPFIX_SIMULATE_SCAN_FAILURE
// Test build (make failtest): proves a broken signature disables only its own feature.
constexpr Signature kAdsSignatures[] = {
    {"ads_fov_full (corrupted on purpose)", "DE AD BE EF DE AD BE EF DE AD BE EF", 0},
    {"ads_fov_tail (corrupted on purpose)", "DE AD BE EF DE AD BE EF DE AD BE EF", 0},
};
constexpr Signature kLegacyAdsSignatures[] = {
    {"ads_fov_legacy_full (corrupted on purpose)", "DE AD BE EF DE AD BE EF DE AD BE EF", 0},
    {"ads_fov_legacy_tail (corrupted on purpose)", "DE AD BE EF DE AD BE EF DE AD BE EF", 0},
};
#else
constexpr Signature kAdsSignatures[] = {
    {"ads_fov_full",
     "80 B8 ?? ?? ?? ?? 00 74 ?? F3 0F 10 80 ?? ?? ?? ?? F3 0F 10 8E ?? ?? ?? ?? F3 0F 5C C1 "
     "F3 0F 59 80 ?? ?? ?? ?? F3 0F 58 C1 EB",
     9},
    {"ads_fov_tail",
     "F3 0F 10 80 ?? ?? ?? ?? F3 0F 10 8E ?? ?? ?? ?? F3 0F 5C C1 F3 0F 59 80 ?? ?? ?? ?? "
     "F3 0F 58 C1",
     0},
};
// Legacy: the current FOV is read through rdx.
constexpr Signature kLegacyAdsSignatures[] = {
    {"ads_fov_legacy_full",
     "80 B8 ?? ?? ?? ?? 00 74 ?? F3 0F 10 80 ?? ?? ?? ?? F3 0F 5C 02 F3 0F 59 80 ?? ?? ?? ?? "
     "F3 0F 58 02 EB",
     9},
    {"ads_fov_legacy_tail", "F3 0F 10 80 ?? ?? ?? ?? F3 0F 5C 02 F3 0F 59 80 ?? ?? ?? ?? F3 0F 58 02", 0},
};
#endif

// Aim down sights while looking through the sight: if the camera holds a sight FOV
// (weapon/attachment scope FOV, >= 1), it jumps to store it as the camera's target FOV.
// The signature finds that check; the patched site is the jump's destination.
constexpr Signature kSightSignatures[] = {
    {"sight_check_full",
     "F3 0F 10 86 ?? ?? ?? ?? 0F 2E 05 ?? ?? ?? ?? 0F 83 ?? ?? ?? ?? 48 8B 8E ?? ?? ?? ?? "
     "F3 0F 10 91",
     8},
    {"sight_check_tail", "0F 2E 05 ?? ?? ?? ?? 0F 83 ?? ?? ?? ?? 48 8B 8E ?? ?? ?? ?? F3 0F 10 91", 0},
};
// Legacy branches over the store instead (`comiss ; jb skip`); these locate the store itself.
constexpr Signature kLegacySightSignatures[] = {
    {"sight_store_legacy_full",
     "F3 0F 10 ?? ?? ?? ?? ?? 0F 2F 05 ?? ?? ?? ?? 72 ?? 8A ?? ?? ?? ?? ?? F3 0F 11 ?? ?? ?? ?? ?? A8 04",
     0x17},
    {"sight_store_legacy_tail",
     "0F 2F 05 ?? ?? ?? ?? 72 ?? 8A ?? ?? ?? ?? ?? F3 0F 11 ?? ?? ?? ?? ?? A8 04 74 ?? A8 02", 0xF},
};

// Reload check for the sight heartbeat. While the player reloads with the aim button held, the
// camera stays in the sight state, so the sight heartbeat (used by the player FOV feature) would
// keep the body and weapon at the sight's narrow FOV and zoom the reload animation. The sight
// stub follows the game's own pointers (camera -> ped -> weapon manager -> weapon object ->
// weapon) and skips the heartbeat while the weapon state is "reloading". Every offset is read
// from the game's instructions:
// - the aim camera's prologue loads the ped: `and byte [rcx+flags],0xDF ; mov ped,[rcx+X]`;
// - further down it reads the weapon manager and calls the equipped-weapon getter:
//   `mov r,[ped+Y] ; test r,r ; je ; mov rcx,r ; call getter ; test rax,rax ; je ; mov rcx,r ; call`;
// - the getter's fast path gives the weapon object and the weapon: `mov rax,[rcx+o] ; ... ;
//   mov r,[rax+w]`;
// - the weapon's reload-state updater gives the state field and the "reloading" value:
//   `cmp byte [rcx+S],RELOADING ; jne ; cmp word [rcx+clip],0 ; ...`.
// Fallbacks for the two camera sites are unique within the aim camera function.
constexpr Signature kAimPedSignatures[] = {
    {"aim_ped_full", "80 A1 ?? ?? 00 00 DF 4C 8B A1 ?? ?? 00 00 E8", 7},
    {"aim_ped_tail", "4C 8B A1 ?? ?? 00 00 E8", 0},
};
constexpr Signature kLegacyAimPedSignatures[] = {
    {"aim_ped_legacy_full", "80 A1 ?? ?? 00 00 DF 48 8B B1 ?? ?? 00 00 0F 29 70", 7},
    {"aim_ped_legacy_tail", "48 8B B1 ?? ?? 00 00 0F 29 70", 0},
};
constexpr Signature kAimWeaponSignatures[] = {
    {"aim_weapon_full",
     "49 8B BC 24 ?? ?? ?? ?? 48 85 FF 74 ?? 48 89 F9 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 89 F9 E8", 0},
    {"aim_weapon_tail", "48 85 FF 74 ?? 48 89 F9 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 89 F9 E8", -8},
};
constexpr Signature kLegacyAimWeaponSignatures[] = {
    {"aim_weapon_legacy_full",
     "48 8B AE ?? ?? ?? ?? 48 85 ED 74 ?? 48 8B CD E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B CD E8", 0},
    {"aim_weapon_legacy_tail", "48 85 ED 74 ?? 48 8B CD E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B CD E8", -7},
};
// The getter's fast path, matched at the call target: {pattern, object disp8, weapon disp32}.
struct GetterShape {
    const char* bytes;
    int objectAt;
    int weaponAt;
};
constexpr GetterShape kWeaponGetter = {"56 57 53 48 83 EC 30 48 8B 41 ?? 48 85 C0 74 ?? 48 8B B8 ?? ?? ?? ??", 10, 19};
constexpr GetterShape kLegacyWeaponGetter = {
    "48 89 5C 24 08 57 48 83 EC 30 48 8B 41 ?? 48 8B D9 48 85 C0 74 ?? 48 8B 80 ?? ?? ?? ??", 13, 25};
#ifdef FPFIX_SIMULATE_SCAN_FAILURE
constexpr Signature kReloadStateSignatures[] = {
    {"weapon_reload_state_full (corrupted on purpose)", "DE AD BE EF DE AD BE EF DE AD BE EF", 0},
    {"weapon_reload_state_tail (corrupted on purpose)", "DE AD BE EF DE AD BE EF DE AD BE EF", 0},
};
constexpr Signature kLegacyReloadStateSignatures[] = {
    {"weapon_reload_state_legacy_full (corrupted on purpose)", "DE AD BE EF DE AD BE EF DE AD BE EF", 0},
    {"weapon_reload_state_legacy_tail (corrupted on purpose)", "DE AD BE EF DE AD BE EF DE AD BE EF", 0},
};
#else
// Both start with `cmp byte [rcx+S],imm8` (state offset at +2, "reloading" at +6).
constexpr Signature kReloadStateSignatures[] = {
    {"weapon_reload_state_full", "80 B9 ?? ?? 00 00 02 75 ?? 66 83 79 ?? 00 74 ?? 31 C0 88 81", 0},
    {"weapon_reload_state_tail", "31 C0 80 B9 ?? ?? ?? ?? 02 0F 44 C2 C3", 2},
};
constexpr Signature kLegacyReloadStateSignatures[] = {
    {"weapon_reload_state_legacy_full", "80 B9 ?? ?? 00 00 02 75 ?? 66 83 79 ?? 00 75 ?? 48 8B 51 ?? 83 7A ?? 02", 0},
    {"weapon_reload_state_legacy_tail", "80 B9 ?? ?? 00 00 02 75 07 8B 41 ?? 2B 41 ?? C3", 0},
};
#endif

// Where the held weapon is relative to the camera (used to tell when it is back at the sight
// after a reload). Offsets from the on-foot camera update:
// - its tail hands the camera position and forward vector to a callback:
//   `mov rcx,[cam+cb] ; test rcx,rcx ; je ; lea ..,[cam+pos] ; ... [cam+forward] ...`;
// - it computes the camera position relative to the attached entity:
//   `movaps ..,[cam+pos] ; ... subps ..,[entity+position]` (every entity, the weapon object too).
// Each signature points at the `lea`/`movaps` that reads the camera position.
struct FrameSignature {
    const char* name;
    const char* bytes;
    int patchOffset;
    int positionAt, otherAt;  // disp8 of the camera position; forward (frame) or entity disp32 (entity)
};
constexpr FrameSignature kCameraFrameSignatures[] = {
    {"camera_frame_full", "48 8B 8E ?? ?? ?? ?? 48 85 C9 74 ?? 48 8D 56 ?? 48 83 C6 ?? 49 89 F0 E8", 12, 3, 7},
    {"camera_frame_tail", "48 8D 56 ?? 48 83 C6 ?? 49 89 F0 E8", 0, 3, 7},
};
constexpr FrameSignature kLegacyCameraFrameSignatures[] = {
    {"camera_frame_legacy_full", "48 8B 8F ?? ?? ?? ?? 48 85 C9 74 ?? 4C 8D 47 ?? 48 8D 57 ?? E8", 12, 7, 3},
    {"camera_frame_legacy_tail", "4C 8D 47 ?? 48 8D 57 ?? E8 ?? ?? ?? ?? 40 8A C5", 0, 7, 3},
};
constexpr FrameSignature kEntityPositionSignatures[] = {
    {"entity_position_full",
     "41 0F 28 5E ?? 41 0F 28 46 ?? 41 0F 28 96 ?? ?? ?? ?? 44 0F 28 46 ?? 45 0F 5C 86 ?? ?? ?? ??", 18, 4, 9},
    {"entity_position_tail", "44 0F 28 46 ?? 45 0F 5C 86 ?? ?? ?? ?? 0F 28 E3 0F C6 E3 C9", 0, 4, 9},
};
constexpr FrameSignature kLegacyEntityPositionSignatures[] = {
    {"entity_position_legacy_full",
     "0F 28 5E ?? 0F 28 56 ?? 0F 28 8E ?? ?? ?? ?? 48 8D 54 24 ?? 48 8D 4C 24 ?? 0F 28 47 ?? 0F 29 4C 24 ?? "
     "0F 29 54 24 ?? 0F 5C 86 ?? ?? ?? ??",
     25, 3, 17},
    {"entity_position_legacy_tail",
     "0F 28 47 ?? 0F 29 4C 24 ?? 0F 29 54 24 ?? 0F 5C 86 ?? ?? ?? ?? 0F 29 44 24 ??", 0, 3, 17},
};

// A camera state with its own fixed target FOV from the metadata (e.g. holding the phone):
// target = metadata->stateFov, bypassing the slider.
constexpr Signature kStateFovSignatures[] = {
    {"state_fov_full",
     "F3 0F 10 89 ?? ?? ?? ?? F3 0F 10 91 ?? ?? ?? ?? F3 0F 59 15 ?? ?? ?? ?? F3 0F 59 C2 "
     "F3 0F 11 8E ?? ?? ?? ??",
     0x1C},
    {"state_fov_tail", "F3 0F 59 15 ?? ?? ?? ?? F3 0F 59 C2 F3 0F 11 8E ?? ?? ?? ?? 0F B6 8E", 0xC},
};
// Legacy copies the state FOV through eax: `mov eax,[meta+stateFov] ... mov [camera+target],eax`.
constexpr Signature kLegacyStateFovSignatures[] = {
    {"state_fov_legacy_full",
     "F3 0F 10 ?? ?? ?? ?? ?? 8B 80 ?? ?? ?? ?? 80 A3 ?? ?? ?? ?? F3 80 A3 ?? ?? ?? ?? EF 89 83 ?? ?? ?? ??",
     0x1C},
    {"state_fov_legacy_tail",
     "8B 80 ?? ?? ?? ?? 80 A3 ?? ?? ?? ?? F3 80 A3 ?? ?? ?? ?? EF 89 83 ?? ?? ?? ?? F3 41 0F 59 F9", 0x14},
};

constexpr Signature kVehicleSignatures[] = {
    {"vehicle_fov_full",
     "F3 0F 10 8E ?? ?? ?? ?? F3 0F 5E C8 F3 0F 11 8E ?? ?? ?? ?? F3 0F 10 B6 ?? ?? ?? ?? "
     "0F 2E 35 ?? ?? ?? ?? 0F 82 ?? ?? ?? ?? F3 0F 10 05 ?? ?? ?? ?? 0F 2E C6 0F 28 C6 "
     "0F 82 ?? ?? ?? ?? F3 0F 11 86 ?? ?? ?? ??",
     20},
    {"vehicle_fov_tail",
     "F3 0F 10 B6 ?? ?? ?? ?? 0F 2E 35 ?? ?? ?? ?? 0F 82 ?? ?? ?? ?? F3 0F 10 05 ?? ?? ?? ?? "
     "0F 2E C6 0F 28 C6 0F 82",
     0},
};
// Legacy clamps with branches: load, then `movss xmm1,[rip+130] ; comiss xmm0,[rip+1.0] ; jb`.
constexpr Signature kLegacyVehicleSignatures[] = {
    {"vehicle_fov_legacy_full",
     "F3 0F 10 ?? ?? ?? ?? ?? F3 0F 5E C8 F3 0F 11 ?? ?? ?? ?? ?? F3 0F 10 ?? ?? ?? ?? ?? "
     "F3 0F 10 0D ?? ?? ?? ?? 0F 2F 05 ?? ?? ?? ?? 72",
     20},
    {"vehicle_fov_legacy_tail",
     "F3 0F 10 ?? ?? ?? ?? ?? F3 0F 10 0D ?? ?? ?? ?? 0F 2F 05 ?? ?? ?? ?? 72 ?? 0F 2F C1 76 ?? 0F 2F 05",
     0},
};

// Near clip: at 120 deg the car's side window, a suit collar on a bike and the sides of a
// weapon held at the sights come closer to the camera than its near plane and get cut.
// The first-person on-foot and vehicle cameras use the engine's own minimum instead.
constexpr float kNearClipFloor = 0.01f;  // the game's minimum (maxss right before the store)
constexpr float kNearClipCap = kNearClipFloor;
constexpr bool kVehicleNearClipEnabled = true;

// Only vehicles with a cabin get the lower near plane: on open ones (bikes, quads, jet
// skis...) it shows the inside of the character's collar instead of cutting it.
// Values of the game's vehicle type enum; the game's own aircraft mask (planes, helis,
// blimps, autogyros = 0x702) in the same camera confirms this numbering.
enum VehicleType : int {
    kCar = 0, kPlane = 1, kSubmarineCar = 5, kAmphibiousCar = 6, kHeli = 8, kBlimp = 9,
    kAutogyro = 10, kTrain = 14, kSubmarine = 15,
};
constexpr std::uint32_t kCabinVehicleMask =
    1u << kCar | 1u << kPlane | 1u << kSubmarineCar | 1u << kAmphibiousCar | 1u << kHeli |
    1u << kBlimp | 1u << kAutogyro | 1u << kTrain | 1u << kSubmarine;
constexpr std::uint32_t kGameAircraftMask = 1u << kPlane | 1u << kHeli | 1u << kBlimp | 1u << kAutogyro;
constexpr int kEntityTypeVehicle = 3;

// The camera's own vehicle-type check, used to learn where the camera keeps its vehicle,
// where an entity stores its type, and where a vehicle stores its vehicle type.
constexpr Signature kVehicleTypeCheckSignatures[] = {
    {"vehicle_type_check_full",
     "48 8B BE ?? ?? ?? ?? 48 85 FF 0F 84 ?? ?? ?? ?? 80 7F ?? 03 75 ?? 8B 87 ?? ?? ?? ?? "
     "83 F8 0A 77 ?? B9 02 07 00 00 0F A3 C1",
     0x10},
    {"vehicle_type_check_tail", "80 7F ?? 03 75 ?? 8B 87 ?? ?? ?? ?? 83 F8 0A 77 ?? B9 02 07 00 00 0F A3 C1", 0},
};
// Legacy tests the aircraft types with compares: `type - 8 <= 2 || type == 1` (the same 0x702).
constexpr Signature kLegacyVehicleTypeCheckSignatures[] = {
    {"vehicle_type_check_legacy_full",
     "48 8B 99 ?? ?? ?? ?? 48 85 DB 74 ?? 80 7B ?? 03 74 ?? 33 DB 48 85 DB 74 ?? 8B 8B ?? ?? ?? ?? "
     "8D 41 F8 83 F8 02 76 ?? 83 F9 01 75",
     0},
    {"vehicle_type_check_legacy_tail",
     "80 7B ?? 03 74 ?? 33 DB 48 85 DB 74 ?? 8B 8B ?? ?? ?? ?? 8D 41 F8 83 F8 02 76 ?? 83 F9 01 75", -12},
};

struct VehicleTypeLayout {
    std::int32_t cameraVehicle;  // camera -> attached vehicle entity
    std::uint8_t entityType;     // entity -> type byte (3 = vehicle)
    std::int32_t vehicleType;    // vehicle -> VehicleType
    int cameraBase;              // register holding the camera in that code
};

struct NearClipSignature {
    const char* name;
    const char* bytes;
    int storeOffset;  // `movss [camera+nearClip], xmmN` right after `maxss xmmN, [rip+0.01]`
};

constexpr NearClipSignature kVehicleNearClipSignatures[] = {
    {"vehicle_nearclip_full",
     "48 8B 86 ?? ?? ?? ?? 80 B8 ?? ?? ?? ?? 00 74 ?? F3 0F 5D 35 ?? ?? ?? ?? "
     "F3 0F 5F 35 ?? ?? ?? ?? F3 0F 11 B6 ?? ?? ?? ??",
     0x20},
    {"vehicle_nearclip_tail",
     "F3 0F 5D 35 ?? ?? ?? ?? F3 0F 5F 35 ?? ?? ?? ?? F3 0F 11 B6 ?? ?? ?? ?? 0F 28 74 24", 0x10},
};
// Legacy: the floor is loaded into another register and applied with comiss/movaps.
constexpr NearClipSignature kLegacyVehicleNearClipSignatures[] = {
    {"vehicle_nearclip_legacy_full",
     "F3 0F 10 05 ?? ?? ?? ?? 0F 2F F0 77 ?? 0F 28 F0 48 8B 5C 24 ?? 0F 28 7C 24 ?? F3 0F 11 B7 ?? ?? ?? ??",
     0x1A},
    {"vehicle_nearclip_legacy_tail",
     "0F 2F F0 77 ?? 0F 28 F0 48 8B 5C 24 ?? 0F 28 7C 24 ?? F3 0F 11 ?? ?? ?? ?? ?? 0F 28 74 24", 0x12},
};

// Shared by every camera: near clip = metadata->nearClip (or an override), at least 0.01.
constexpr NearClipSignature kCameraNearClipSignatures[] = {
    {"camera_nearclip_full",
     "48 8B 8E ?? ?? ?? ?? 48 83 C1 ?? 48 8D 96 ?? ?? ?? ?? A8 ?? 48 0F 44 D1 F3 0F 10 02 "
     "F3 0F 5F 05 ?? ?? ?? ?? F3 0F 11 86 ?? ?? ?? ??",
     0x24},
    {"camera_nearclip_tail", "48 0F 44 D1 F3 0F 10 02 F3 0F 5F 05 ?? ?? ?? ?? F3 0F 11 86 ?? ?? ?? ??",
     0x10},
};
// Legacy branches between the override and `metadata->nearClip`; these locate the 5-byte
// `movss xmmN,[meta+imm8]` load of the latter (patched instead of the store).
constexpr Signature kLegacyCameraNearClipSignatures[] = {
    {"camera_nearclip_legacy_full",
     "F3 0F 10 0D ?? ?? ?? ?? F6 C1 10 74 ?? F3 0F 10 83 ?? ?? ?? ?? EB ?? 48 8B 83 ?? ?? ?? ?? "
     "F3 0F 10 40 ?? 0F 2F C1 77 ?? 0F 28 C1",
     0x1E},
    {"camera_nearclip_legacy_tail",
     "48 8B 83 ?? ?? ?? ?? F3 0F 10 40 ?? 0F 2F C1 77 ?? 0F 28 C1 80 E1 ?? F3 0F 11 83", 7},
};

// Values shared with the stubs; lives at the start of the CodeBlock page.
struct Shared {
    volatile float maxVerticalFov;          // read by the on-foot stub
    volatile float scale;                   // read by the aim and vehicle stubs
    const std::uint8_t* volatile footMeta;  // written by the on-foot stub: camera metadata
    volatile float nearClipCap;             // read by the near-clip stubs
    volatile float chosenFov;               // universal mode: on-foot FOV for the slider; 0 = unknown
    volatile std::uint32_t heartbeats[3];   // bumped by the vehicle / sight stubs on every update
    volatile float sightGameFov;            // the game's own sight FOV, before our adjustment
    const std::uint8_t* volatile sightCamera;   // written by the sight stub: the aim camera
    const std::uint8_t* volatile weaponObject;  // written by the sight stub: the held weapon, or null
};
static_assert(sizeof(Shared) <= CodeBlock::kDataSize);

CodeBlock g_block;
Shared* g_shared = nullptr;
const volatile std::int32_t* g_sliderPosition = nullptr;
int g_minFovOffset = -1, g_maxFovOffset = -1;  // inside the on-foot camera metadata

int Reg(std::uint8_t modrm) { return (modrm >> 3) & 7; }
int Base(std::uint8_t modrm) { return modrm & 7; }

// movss with a [base+disp32] operand and no SIB byte: rewritable 1:1 to other forms.
bool IsMovssDisp32(const std::uint8_t* p, std::uint8_t opcode) {
    return p[0] == 0xF3 && p[1] == 0x0F && p[2] == opcode && (p[3] & 0xC0) == 0x80 &&
           Base(p[3]) != 4;
}

std::uint8_t RipModrm(int reg) { return std::uint8_t(reg << 3 | 5); }

std::int32_t Disp32(const std::uint8_t* p) {
    std::int32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

float RipFloat(const std::uint8_t* instruction, int operandOffset, int length) {
    float f;
    std::memcpy(&f, Pattern::ResolveRip(instruction, operandOffset, length), sizeof f);
    return f;
}

// `cvtsi2ss x,[rip+pref]; divss x,[rip+10.0f]` just before `p`: the slider conversion.
const volatile std::int32_t* SliderReadBefore(const std::uint8_t* p) {
    for (const auto* q = p - kSliderContextBytes; q < p - 16; ++q) {
        if (q[0] != 0xF3 || q[1] != 0x0F || q[2] != 0x2A || (q[3] & 0xC7) != 0x05) continue;
        if (q[8] != 0xF3 || q[9] != 0x0F || q[10] != 0x5E || (q[11] & 0xC7) != 0x05) continue;
        if (RipFloat(q + 8, 4, 8) != kSliderSteps) continue;
        return reinterpret_cast<const volatile std::int32_t*>(Pattern::ResolveRip(q, 4, 8));
    }
    return nullptr;
}

bool InCode(const std::uint8_t* p, size_t length) {
    auto code = GameImage::Code();
    return p >= code.data() && p + length <= code.data() + code.size();
}

// Legacy computes t in a small function instead:
//   movd x,[rip+pref] ; xorps ; cvtdq2ps x,x ; mulss x,[rip+0.1] ; clamp to [0,1]
const volatile std::int32_t* SliderFunctionRead(const std::uint8_t* fn) {
    constexpr int kSearch = 12;
    if (!InCode(fn, 8 + kSearch + 8) || fn[0] != 0x66 || fn[1] != 0x0F || fn[2] != 0x6E ||
        (fn[3] & 0xC7) != 0x05)
        return nullptr;
    for (const auto* q = fn + 8; q < fn + 8 + kSearch; ++q)
        if (q[0] == 0xF3 && q[1] == 0x0F && q[2] == 0x59 && (q[3] & 0xC7) == 0x05 &&
            RipFloat(q, 4, 8) == 1.0f / kSliderSteps)
            return reinterpret_cast<const volatile std::int32_t*>(Pattern::ResolveRip(fn, 4, 8));
    return nullptr;
}

// The slider position read just before `p`: inline (Enhanced) or by a call (Legacy).
const volatile std::int32_t* SliderBefore(const std::uint8_t* p) {
    if (auto* inline_ = SliderReadBefore(p)) return inline_;
    for (const auto* q = p - kSliderContextBytes; q <= p - 5; ++q)
        if (q[0] == 0xE8)
            if (auto* read = SliderFunctionRead(Pattern::ResolveRip(q, 1, 5))) return read;
    return nullptr;
}

template <typename Validate>
std::uint8_t* FindSite(std::span<const Signature> signatures, Validate isValid) {
    for (const auto& sig : signatures) {
        auto hits = Pattern::FindAll(GameImage::Code(), sig.bytes);
        Log::Info("scan %s: %zu match(es)", sig.name, hits.size());
        if (hits.size() != 1) continue;
        auto* site = const_cast<std::uint8_t*>(hits[0] + sig.patchOffset);
        if (isValid(site)) return site;
        Log::Info("scan %s: match failed semantic validation", sig.name);
    }
    return nullptr;
}

std::span<const Signature> ForEdition(std::span<const Signature> enhanced, std::span<const Signature> legacy) {
    return GameImage::IsLegacy() ? legacy : enhanced;
}

size_t ExeOffset(const void* p) {
    return size_t(static_cast<const std::uint8_t*>(p) -
                  reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr)));
}

std::span<const std::uint8_t> Original(const std::uint8_t* site, int length = kMovssLength) {
    return {site, size_t(length)};
}

float g_maxHorizontalFovDegrees = 0;  // from the ini, set by Apply()
bool g_universal = false;             // from the ini, set by Apply()
std::int32_t g_cameraTargetFovOffset = -1;  // camera field the hip-aim lerp stores into
bool g_heartbeatTracked[3] = {};            // the stub of that camera counts its updates

// Where the on-foot formula reads the maximum; also learns the minimum's offset and the slider.
std::uint8_t* FindOnFootMaxLoad() {
    if (GameImage::IsLegacy()) {
        auto* load = FindSite(kLegacyOnFootSignatures, [](const std::uint8_t* p) {
            // Right after: `subss xmmN,[base+imm8]` subtracting the minimum of the same metadata.
            return IsMovssDisp32(p, 0x10) && p[8] == 0xF3 && p[9] == 0x0F && p[10] == 0x5C &&
                   (p[11] & 0xC0) == 0x40 && Base(p[11]) == Base(p[3]) && Reg(p[11]) == Reg(p[3]) &&
                   (g_sliderPosition = SliderBefore(p));
        });
        if (load) g_minFovOffset = load[12];
        return load;
    }
    auto* load = FindSite(kOnFootSignatures, [](const std::uint8_t* p) {
        // Right before: `movss xmm1,[base+imm8]` loading the minimum from the same metadata.
        return IsMovssDisp32(p, 0x10) && p[-5] == 0xF3 && p[-4] == 0x0F && p[-3] == 0x10 &&
               (p[-2] & 0xC0) == 0x40 && Base(p[-2]) == Base(p[3]) &&
               (g_sliderPosition = SliderReadBefore(p));
    });
    if (load) g_minFovOffset = load[-1];
    return load;
}

bool PatchOnFoot() {
    auto* load = FindOnFootMaxLoad();
    if (!load) return false;
    std::memcpy(&g_maxFovOffset, load + 4, 4);

    // Remember the metadata pointer (so the original maximum can be read), then load ours.
    const auto* stub = g_block.Cursor();
    g_block.Emit({0x48, 0x89, RipModrm(Base(load[3]))});
    g_block.EmitRip32(const_cast<const std::uint8_t**>(&g_shared->footMeta));
    g_block.Emit({0xF3, 0x0F, 0x10, RipModrm(Reg(load[3]))});
    g_block.EmitRip32(const_cast<float*>(&g_shared->maxVerticalFov));
    g_block.EmitJmp(load + kMovssLength);
    if (!CodePatch::JumpToStub(load, Original(load), stub)) return false;
    Log::Info("FOV on foot: patched at exe+0x%zx (metadata min +0x%x, max +0x%x)",
              ExeOffset(load), g_minFovOffset, g_maxFovOffset);
    return true;
}

// Counts one update of `beat`'s camera, for other features (see FieldOfView::Heartbeat); flags kept.
void EmitHeartbeat(FieldOfView::Heartbeat beat) {
    g_block.Emit({0x9C, 0xF0, 0xFF, 0x05});  // pushfq; lock inc dword [rip+counter]
    g_block.EmitRip32(const_cast<std::uint32_t*>(&g_shared->heartbeats[int(beat)]));
    g_block.Emit({0x9D});  // popfq
}

// Makes the FOV held in xmmN follow the slider: scaled by the slider's widening or, in universal
// mode and for `aiming` cameras, replaced by the on-foot FOV (kept as the game computed it while
// that is still unknown).
void EmitFovAdjust(int reg, bool aiming) {
    if (!(g_universal && aiming)) {
        g_block.Emit({0xF3, 0x0F, 0x59, RipModrm(reg)});  // mulss xmmN, scale
        g_block.EmitRip32(const_cast<float*>(&g_shared->scale));
        return;
    }
    g_block.Emit({0x9C, 0x83, 0x3D});  // pushfq; cmp dword [rip+chosenFov], 0 (trailing imm8)
    g_block.EmitRip32(reinterpret_cast<const std::uint8_t*>(const_cast<float*>(&g_shared->chosenFov)) - 1);
    g_block.Emit({0x00});
    auto* unknown = g_block.EmitBranch({0x0F, 0x84});  // je unknown
    g_block.Emit({0xF3, 0x0F, 0x10, RipModrm(reg)});  // movss xmmN, chosenFov
    g_block.EmitRip32(const_cast<float*>(&g_shared->chosenFov));
    g_block.Bind(unknown);
    g_block.Emit({0x9D});  // popfq
}

// Stub: FOV adjustment around the original movss (after a load, before a store).
const std::uint8_t* EmitScaled(const std::uint8_t* site, bool scaleBefore, int length, bool aiming) {
    const auto* stub = g_block.Cursor();
    if (!scaleBefore) g_block.Emit(Original(site, length));
    EmitFovAdjust(Reg(site[3]), aiming);
    if (scaleBefore) g_block.Emit(Original(site, length));
    g_block.EmitJmp(site + length);
    return stub;
}

void PatchScaled(const char* what, std::uint8_t* site, bool scaleBefore, bool aiming = false,
                 int length = kMovssLength) {
    if (!site) {
        Log::Info("FOV %s: not found; stays original", what);
        return;
    }
    if (CodePatch::JumpToStub(site, Original(site, length), EmitScaled(site, scaleBefore, length, aiming)))
        Log::Info("FOV %s: patched at exe+0x%zx", what, ExeOffset(site));
    else
        Log::Info("FOV %s: patch refused; stays original", what);
}

void PatchHipAim() {
    auto* store = FindSite(ForEdition(kHipAimSignatures, kLegacyHipAimSignatures), [](const std::uint8_t* p) {
        return IsMovssDisp32(p, 0x11) && SliderBefore(p) == g_sliderPosition;
    });
    if (store) std::memcpy(&g_cameraTargetFovOffset, store + 4, 4);
    PatchScaled("hip aim", store, /*scaleBefore=*/true, /*aiming=*/true);
}

// True if the instruction at `p` writes the camera target FOV (its disp32 at `dispAt`).
bool WritesTargetFov(const std::uint8_t* p, int dispAt) {
    std::int32_t offset;
    std::memcpy(&offset, p + dispAt, 4);
    return g_cameraTargetFovOffset >= 0 && offset == g_cameraTargetFovOffset;
}

// The store of the sight FOV as the camera target FOV (when the sight FOV is >= 1).
std::uint8_t* FindSightStore() {
    if (GameImage::IsLegacy())
        // `movss xmmN,[cam+sightFov] ; comiss xmmN,[rip+1.0] ; jb skip ; mov al,.. ; movss [cam+target],xmmN`
        return FindSite(kLegacySightSignatures, [](const std::uint8_t* store) {
            const auto* load = store - 0x17;
            const auto* check = store - 0xF;
            return IsMovssDisp32(load, 0x10) && check[0] == 0x0F && check[1] == 0x2F &&
                   (check[2] & 0xC7) == 0x05 && Reg(check[2]) == Reg(load[3]) &&
                   RipFloat(check, 3, 7) == 1.0f && IsMovssDisp32(store, 0x11) &&
                   Reg(store[3]) == Reg(load[3]) && Base(store[3]) == Base(load[3]) &&
                   WritesTargetFov(store, 4);
        });
    // Hit is `ucomiss xmm,[rip+1.0]; jae store`; the store must write the camera target FOV.
    auto* check = FindSite(kSightSignatures, [](const std::uint8_t* p) {
        if (RipFloat(p, 3, 7) != 1.0f || p[7] != 0x0F || p[8] != 0x83) return false;
        const auto* store = Pattern::ResolveRip(p + 7, 2, 6);
        return IsMovssDisp32(store, 0x11) && Reg(store[3]) == Reg(p[2]) && WritesTargetFov(store, 4);
    });
    return check ? const_cast<std::uint8_t*>(Pattern::ResolveRip(check + 7, 2, 6)) : nullptr;
}

// Offsets of the reload check (see kAimPedSignatures); valid only when `found`.
struct ReloadCheck {
    bool found = false;
    std::int32_t ped = 0, weaponManager = 0, weaponObject = 0, weapon = 0, state = 0;
    std::uint8_t reloading = 0;
};

// The game function containing `p` (from the image's unwind data), or {} if none.
std::span<const std::uint8_t> FunctionOf(const std::uint8_t* p) {
    DWORD64 base = 0;
    const auto* entry = RtlLookupFunctionEntry(DWORD64(p), &base, nullptr);
    if (!entry) return {};
    return {reinterpret_cast<const std::uint8_t*>(base + entry->BeginAddress), entry->EndAddress - entry->BeginAddress};
}

// Offsets for FieldOfView::SightWeaponOffset; valid only when `found`.
struct WeaponPose {
    bool found = false;
    int cameraPosition = 0, cameraForward = 0;
    std::int32_t entityPosition = 0;
};
WeaponPose g_weaponPose;

const std::uint8_t* FindFrameSite(std::span<const FrameSignature> signatures) {
    for (const auto& sig : signatures) {
        auto hits = Pattern::FindAll(GameImage::Code(), sig.bytes);
        Log::Info("scan %s: %zu match(es)", sig.name, hits.size());
        if (hits.size() == 1) return hits[0] + sig.patchOffset;
    }
    return nullptr;
}

WeaponPose FindWeaponPose() {
    WeaponPose out;
    bool legacy = GameImage::IsLegacy();
    const auto& frameSigs = legacy ? kLegacyCameraFrameSignatures : kCameraFrameSignatures;
    const auto& entitySigs = legacy ? kLegacyEntityPositionSignatures : kEntityPositionSignatures;
    const auto* frame = FindFrameSite(frameSigs);
    const auto* entity = FindFrameSite(entitySigs);
    if (!frame || !entity) return out;
    // Same function, same camera position field: the two reads describe one camera.
    const auto& fs = frameSigs[0];
    const auto& es = entitySigs[0];
    int position = frame[fs.positionAt], forward = frame[fs.otherAt];
    int positionAgain = entity[es.positionAt];
    std::int32_t entityPosition = Disp32(entity + es.otherAt);
    auto fn = FunctionOf(frame);
    bool sameFunction = !fn.empty() && fn.data() == FunctionOf(entity).data();
    if (!sameFunction || position != positionAgain || position % 16 || forward % 16 || entityPosition <= 0 ||
        entityPosition % 16)
        return out;
    out = {true, position, forward, entityPosition};
    return out;
}

// The first signature with exactly one match in `scope` (whole code for primaries, the aim
// camera function for fallbacks) that passes `isValid`.
template <size_t N, typename Validate>
const std::uint8_t* FindInAimCamera(const Signature (&signatures)[N], std::span<const std::uint8_t> function,
                                    Validate isValid) {
    for (size_t i = 0; i < N; ++i) {
        const auto& sig = signatures[i];
        auto hits = Pattern::FindAll(i == 0 ? GameImage::Code() : function, sig.bytes);
        Log::Info("scan %s: %zu match(es)", sig.name, hits.size());
        if (hits.size() != 1) continue;
        const auto* site = hits[0] + sig.patchOffset;
        if (site >= function.data() && site < function.data() + function.size() && isValid(site)) return site;
        Log::Info("scan %s: match failed semantic validation", sig.name);
    }
    return nullptr;
}

// `mov r64,[base+disp32]` with a REX.W prefix: destination and base registers (0-15) and the
// displacement; false for other forms.
bool ReadMovLoad(const std::uint8_t* p, int& dest, int& base, std::int32_t& disp, int& length) {
    if ((p[0] & 0xF8) != 0x48 || p[1] != 0x8B || (p[2] & 0xC0) != 0x80) return false;
    dest = ((p[2] >> 3) & 7) | (p[0] & 4) << 1;
    int rm = p[2] & 7;
    bool sib = rm == 4;
    if (sib && (p[3] & 0x3F) != 0x24) return false;  // only [base+disp32], no index
    base = (sib ? p[3] & 7 : rm) | (p[0] & 1) << 3;
    disp = Disp32(p + (sib ? 4 : 3));
    length = sib ? 8 : 7;
    return true;
}

ReloadCheck FindReloadCheck(const std::uint8_t* sightStore) {
    ReloadCheck out;
    bool legacy = GameImage::IsLegacy();
    auto function = FunctionOf(sightStore);
    if (function.empty()) return out;
    int pedReg = -1;
    const auto* pedLoad = legacy ? FindInAimCamera(kLegacyAimPedSignatures, function, [&](const std::uint8_t* p) {
        int base, length;
        return ReadMovLoad(p, pedReg, base, out.ped, length) && base == 1 && out.ped > 0;  // [rcx+X]
    }) : FindInAimCamera(kAimPedSignatures, function, [&](const std::uint8_t* p) {
        int base, length;
        return ReadMovLoad(p, pedReg, base, out.ped, length) && base == 1 && out.ped > 0;
    });
    if (!pedLoad) return out;
    const std::uint8_t* getter = nullptr;
    auto weaponAccess = [&](const std::uint8_t* p) {
        int dest, base, length;
        if (!ReadMovLoad(p, dest, base, out.weaponManager, length) || base != pedReg || out.weaponManager <= 0)
            return false;
        const auto* call = p + length + 8;  // test r,r ; je ; mov rcx,r ; call getter
        if (call[0] != 0xE8) return false;
        getter = call + 5 + Disp32(call + 1);
        return true;
    };
    const auto* access = legacy ? FindInAimCamera(kLegacyAimWeaponSignatures, function, weaponAccess)
                                : FindInAimCamera(kAimWeaponSignatures, function, weaponAccess);
    if (!access) return out;
    const auto& shape = legacy ? kLegacyWeaponGetter : kWeaponGetter;
    auto code = GameImage::Code();
    if (getter < code.data() || getter + 32 > code.data() + code.size()) return out;
    auto match = Pattern::FindAll({getter, 32}, shape.bytes);
    if (match.size() != 1 || match[0] != getter) {
        Log::Info("FOV sight: weapon getter did not validate");
        return out;
    }
    out.weaponObject = getter[shape.objectAt];
    out.weapon = Disp32(getter + shape.weaponAt);
    const auto* state = FindSite(ForEdition(kReloadStateSignatures, kLegacyReloadStateSignatures),
                                 [&](const std::uint8_t* p) {
        if (p[0] != 0x80 || p[1] != 0xB9 || p[6] == 0) return false;  // cmp byte [rcx+S],RELOADING
        std::int32_t field = Disp32(p + 2);
        // The full patterns end in the updater's own store to the same field; check it there.
        if (legacy && p[0x25] == 0xC6 && p[0x26] == 0x81 && Disp32(p + 0x27) != field) return false;
        if (!legacy && p[0x12] == 0x88 && p[0x13] == 0x81 && Disp32(p + 0x14) != field) return false;
        return field > 0;
    });
    if (!state) return out;
    out.state = Disp32(state + 2);
    out.reloading = state[6];
    out.found = out.weaponObject > 0 && out.weapon > 0;
    return out;
}

// The sight heartbeat, or, while the player's weapon is reloading (when `check` is valid), the
// reload heartbeat instead.
// `camera` is the register that holds the aim camera at the sight store. Uses one scratch
// register (saved) and keeps the flags.
void EmitSightHeartbeat(int camera, const ReloadCheck& check) {
    if (!check.found) {
        EmitHeartbeat(FieldOfView::Heartbeat::SightCamera);
        return;
    }
    const std::uint8_t r = camera == 0 ? 1 : 0;  // rax, or rcx if the camera is in rax
    auto load = [&](int base, std::int32_t disp) {  // mov r,[base+disp32] ; test r,r ; jz beat
        g_block.Emit({0x48, 0x8B, std::uint8_t(0x80 | r << 3 | base)});
        g_block.Emit(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&disp), 4));
        g_block.Emit({0x48, 0x85, std::uint8_t(0xC0 | r << 3 | r)});
        return g_block.EmitBranch({0x0F, 0x84});
    };
    g_block.Emit({0x9C, std::uint8_t(0x50 + r)});  // pushfq ; push r
    g_block.Emit({0x48, 0x89, std::uint8_t(0x05 | camera << 3)});  // mov [rip+sightCamera],camera
    g_block.EmitRip32(const_cast<const std::uint8_t**>(&g_shared->sightCamera));
    g_block.Emit({0x48, 0xC7, 0x05});  // mov qword [rip+weaponObject],0 (imm32 follows the disp32)
    g_block.EmitRip32(reinterpret_cast<const std::uint8_t*>(const_cast<const std::uint8_t**>(&g_shared->weaponObject)) - 4);
    g_block.Emit({0, 0, 0, 0});
    std::uint8_t* toBeat[4] = {load(camera, check.ped), load(r, check.weaponManager),
                               load(r, check.weaponObject), nullptr};
    g_block.Emit({0x48, 0x89, std::uint8_t(0x05 | r << 3)});  // mov [rip+weaponObject],r
    g_block.EmitRip32(const_cast<const std::uint8_t**>(&g_shared->weaponObject));
    toBeat[3] = load(r, check.weapon);
    g_block.Emit({0x80, std::uint8_t(0xB8 | r)});  // cmp byte [r+state],RELOADING
    g_block.Emit(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&check.state), 4));
    g_block.Emit({check.reloading});
    auto* reloading = g_block.EmitBranch({0x0F, 0x84});  // je reloading
    for (auto* branch : toBeat) g_block.Bind(branch);
    g_block.Emit({0xF0, 0xFF, 0x05});  // lock inc dword [rip+sight counter]
    g_block.EmitRip32(const_cast<std::uint32_t*>(&g_shared->heartbeats[int(FieldOfView::Heartbeat::SightCamera)]));
    auto* done = g_block.EmitBranch({0xE9});
    g_block.Bind(reloading);
    g_block.Emit({0xF0, 0xFF, 0x05});  // lock inc dword [rip+reload counter]
    g_block.EmitRip32(const_cast<std::uint32_t*>(&g_shared->heartbeats[int(FieldOfView::Heartbeat::SightReloading)]));
    g_block.Bind(done);
    g_block.Emit({std::uint8_t(0x58 + r), 0x9D});  // pop r ; popfq
}

void PatchSight() {
    auto* store = FindSightStore();
    if (!store) {
        Log::Info("FOV sight: not found; stays original");
        return;
    }
    ReloadCheck reload = FindReloadCheck(store);
    if (reload.found)
        Log::Info("FOV sight: reload check (ped +0x%x, weapons +0x%x, object +0x%x, weapon +0x%x, state +0x%x == %u)",
                  reload.ped, reload.weaponManager, reload.weaponObject, reload.weapon, reload.state, reload.reloading);
    else
        Log::Info("FOV sight: reload check not found; aiming while reloading keeps the sight FOV");
    if (reload.found) {
        g_weaponPose = FindWeaponPose();
        if (g_weaponPose.found)
            Log::Info("FOV sight: weapon pose (camera position +0x%x, forward +0x%x, entity position +0x%x)",
                      g_weaponPose.cameraPosition, g_weaponPose.cameraForward, g_weaponPose.entityPosition);
        else
            Log::Info("FOV sight: weapon pose not found; the sight returns a fixed time after a reload");
    }
    // Before our adjustment the register holds the game's own sight FOV: keep it for the player
    // FOV feature (the weapon then looks as in the original game), and count the update.
    const auto* stub = g_block.Cursor();
    g_block.Emit({0xF3, 0x0F, 0x11, RipModrm(Reg(store[3]))});  // movss [rip+sightGameFov], xmmN
    g_block.EmitRip32(const_cast<float*>(&g_shared->sightGameFov));
    EmitFovAdjust(Reg(store[3]), /*aiming=*/true);
    g_block.Emit(Original(store));
    EmitSightHeartbeat(Base(store[3]), reload);
    g_block.EmitJmp(store + kMovssLength);
    if (CodePatch::JumpToStub(store, Original(store), stub)) {
        g_heartbeatTracked[int(FieldOfView::Heartbeat::SightCamera)] = true;
        g_heartbeatTracked[int(FieldOfView::Heartbeat::SightReloading)] = reload.found;
        Log::Info("FOV sight: patched at exe+0x%zx", ExeOffset(store));
    } else {
        Log::Info("FOV sight: patch refused; stays original");
    }
}

// Legacy: `mov [camera+target],eax` (6 bytes) holds the state FOV as raw bits. The stub keeps the
// store, then scales the stored float through xmm0, saved on the stack (no flags touched).
void PatchLegacyStateFov() {
    constexpr int kLength = 6;
    auto* store = FindSite(kLegacyStateFovSignatures, [](const std::uint8_t* p) {
        const auto* load = p - 0x14;  // mov eax,[meta+stateFov]
        return load[0] == 0x8B && (load[1] & 0xC0) == 0x80 && Base(load[1]) != 4 && p[0] == 0x89 &&
               (p[1] & 0xC0) == 0x80 && Base(p[1]) != 4 && Reg(p[1]) == Reg(load[1]) &&
               WritesTargetFov(p, 2);
    });
    if (!store) {
        Log::Info("FOV fixed-FOV state (phone): not found; stays original");
        return;
    }
    const std::uint8_t xmm0AtTarget = std::uint8_t(0x80 | Base(store[1]));  // xmm0, [base+disp32]
    const std::span<const std::uint8_t> disp(store + 2, 4);
    const auto* stub = g_block.Cursor();
    g_block.Emit(Original(store, kLength));
    g_block.Emit({0x48, 0x8D, 0x64, 0x24, 0xF0});  // lea rsp,[rsp-16]
    g_block.Emit({0xF3, 0x0F, 0x11, 0x04, 0x24});  // movss [rsp],xmm0
    g_block.Emit({0xF3, 0x0F, 0x10, xmm0AtTarget});
    g_block.Emit(disp);
    EmitFovAdjust(0, /*aiming=*/false);
    g_block.Emit({0xF3, 0x0F, 0x11, xmm0AtTarget});
    g_block.Emit(disp);
    g_block.Emit({0xF3, 0x0F, 0x10, 0x04, 0x24});  // movss xmm0,[rsp]
    g_block.Emit({0x48, 0x8D, 0x64, 0x24, 0x10});  // lea rsp,[rsp+16]
    g_block.EmitJmp(store + kLength);
    if (CodePatch::JumpToStub(store, Original(store, kLength), stub))
        Log::Info("FOV fixed-FOV state (phone): patched at exe+0x%zx", ExeOffset(store));
    else
        Log::Info("FOV fixed-FOV state (phone): patch refused; stays original");
}

void PatchStateFov() {
    if (GameImage::IsLegacy()) {
        PatchLegacyStateFov();
        return;
    }
    auto* store = FindSite(kStateFovSignatures, [](const std::uint8_t* p) {
        // Load of metadata->stateFov into the same register, then stored as the target FOV.
        const auto* load = p - 0x1C;
        return IsMovssDisp32(load, 0x10) && IsMovssDisp32(p, 0x11) && Reg(load[3]) == Reg(p[3]) &&
               WritesTargetFov(p, 4);
    });
    PatchScaled("fixed-FOV state (phone)", store, /*scaleBefore=*/true);
}

void PatchAds() {
    // target = aiming->adsFov; current + (target-current)*blend, the current FOV read from the
    // camera (Enhanced) or through rdx (Legacy).
    auto* load = GameImage::IsLegacy()
        ? FindSite(kLegacyAdsSignatures, [](const std::uint8_t* p) {
              return IsMovssDisp32(p, 0x10) && p[8] == 0xF3 && p[9] == 0x0F && p[10] == 0x5C &&
                     IsMovssDisp32(p + 12, 0x59);  // mulss xmm0,[aiming+blend]
          })
        : FindSite(kAdsSignatures, [](const std::uint8_t* p) {
              return IsMovssDisp32(p, 0x10) && IsMovssDisp32(p + 8, 0x10) && p[16] == 0xF3 &&
                     p[17] == 0x0F && p[18] == 0x5C && p[20] == 0xF3 && p[21] == 0x0F && p[22] == 0x59;
          });
    PatchScaled("ADS", load, /*scaleBefore=*/false, /*aiming=*/true);
}

constexpr float kFovClampMin = 1.0f, kFovClampMax = 130.0f;
// Offset of the camera frame FOV, read from the `movss [base+disp32],xmm0` that stores the
// clamped value (0x29 bytes after the vehicle FOV load). The near-clip checks rely on it.
constexpr int kFrameFovStoreOffset = 0x29;
int g_frameFovOffset = -1;

// Legacy: after the clamp the camera reloads the unscaled field (`movss xmm1,[cam+fov]`) to blend
// transitions, then stores the clamped FOV in the frame; both within a few instructions.
constexpr int kLegacyVehicleTailBytes = 0x60;
struct LegacyVehicleTail { std::uint8_t* reload; const std::uint8_t* frameStore; };
LegacyVehicleTail FindLegacyVehicleTail(std::uint8_t* load) {
    LegacyVehicleTail tail{};
    for (auto* q = load + kMovssLength; q < load + kLegacyVehicleTailBytes && !tail.frameStore; ++q) {
        if (IsMovssDisp32(q, 0x10) && Base(q[3]) == Base(load[3]) && Disp32(q + 4) == Disp32(load + 4))
            tail.reload = q;
        else if (tail.reload && IsMovssDisp32(q, 0x11) && Base(q[3]) == Base(load[3]) &&
                 Reg(q[3]) == Reg(load[3]))
            tail.frameStore = q;
    }
    return tail;
}

void PatchVehicle() {
    LegacyVehicleTail legacyTail{};
    auto* load = GameImage::IsLegacy()
        ? FindSite(kLegacyVehicleSignatures, [&](const std::uint8_t* p) {
              // Followed by the camera FOV clamp [1, 130] (with branches) and the frame store.
              legacyTail = FindLegacyVehicleTail(const_cast<std::uint8_t*>(p));
              return IsMovssDisp32(p, 0x10) && p[8] == 0xF3 && p[9] == 0x0F && p[10] == 0x10 &&
                     (p[11] & 0xC7) == 0x05 && RipFloat(p + 8, 4, 8) == kFovClampMax && p[16] == 0x0F &&
                     p[17] == 0x2F && (p[18] & 0xC7) == 0x05 && RipFloat(p + 16, 3, 7) == kFovClampMin &&
                     legacyTail.frameStore;
          })
        : FindSite(kVehicleSignatures, [](const std::uint8_t* p) {
              // Followed by the camera FOV clamp [1, 130] before the frame FOV is written.
              return IsMovssDisp32(p, 0x10) && p[8] == 0x0F && p[9] == 0x2E && p[10] == 0x35 &&
                     RipFloat(p + 8, 3, 7) == kFovClampMin && p[21] == 0xF3 && p[22] == 0x0F &&
                     p[23] == 0x10 && p[24] == 0x05 && RipFloat(p + 21, 4, 8) == kFovClampMax;
          });
    if (!load) {
        Log::Info("FOV vehicle: not found; vehicle FOV stays original");
        return;
    }
    const auto* frameStore = GameImage::IsLegacy() ? legacyTail.frameStore : load + kFrameFovStoreOffset;
    if (IsMovssDisp32(frameStore, 0x11)) g_frameFovOffset = Disp32(frameStore + 4);
    const auto* stub = g_block.Cursor();
    g_block.Emit(Original(load));
    EmitFovAdjust(Reg(load[3]), /*aiming=*/false);
    EmitHeartbeat(FieldOfView::Heartbeat::VehicleCamera);
    g_block.EmitJmp(load + kMovssLength);
    if (CodePatch::JumpToStub(load, Original(load), stub)) {
        g_heartbeatTracked[int(FieldOfView::Heartbeat::VehicleCamera)] = true;
        Log::Info("FOV vehicle: patched at exe+0x%zx", ExeOffset(load));
        if (legacyTail.reload) PatchScaled("vehicle transition", legacyTail.reload, /*scaleBefore=*/false);
    }
    else
        Log::Info("FOV vehicle: patch refused; vehicle FOV stays original");
}

// The near clip sits right after the frame FOV field.
bool StoresNearClip(const std::uint8_t* store) {
    return g_frameFovOffset >= 0 && Disp32(store + 4) == g_frameFovOffset + 4;
}

// Enhanced: `maxss xmmN,[rip+0.01]` right before the store of xmmN.
bool ClampedByMaxssBefore(const std::uint8_t* store) {
    const auto* maxss = store - kMovssLength;
    return maxss[0] == 0xF3 && maxss[1] == 0x0F && maxss[2] == 0x5F && (maxss[3] & 0xC7) == 0x05 &&
           RipFloat(maxss, 4, 8) == kNearClipFloor && Reg(store[3]) == Reg(maxss[3]);
}

// Legacy: `movss xmmT,[rip+0.01]` shortly before, then comiss/movaps into the stored register.
constexpr int kNearClipFloorSearchBytes = 0x30;
bool NearClipFloorLoadedBefore(const std::uint8_t* at) {
    for (const auto* q = at - kNearClipFloorSearchBytes; q <= at - kMovssLength; ++q)
        if (q[0] == 0xF3 && q[1] == 0x0F && q[2] == 0x10 && (q[3] & 0xC7) == 0x05 &&
            RipFloat(q, 4, 8) == kNearClipFloor)
            return true;
    return false;
}

// Store of the clamped near clip into the camera frame.
std::uint8_t* FindNearClipStore(std::span<const NearClipSignature> signatures) {
    for (const auto& sig : signatures) {
        auto hits = Pattern::FindAll(GameImage::Code(), sig.bytes);
        Log::Info("scan %s: %zu match(es)", sig.name, hits.size());
        if (hits.size() != 1) continue;
        auto* p = const_cast<std::uint8_t*>(hits[0] + sig.storeOffset);
        if (IsMovssDisp32(p, 0x11) && StoresNearClip(p) &&
            (GameImage::IsLegacy() ? NearClipFloorLoadedBefore(p) : ClampedByMaxssBefore(p)))
            return p;
        Log::Info("scan %s: match failed semantic validation", sig.name);
    }
    return nullptr;
}

// Legacy: `mov rbx,[rcx+veh] ; .. cmp byte [rbx+type],3 ; .. mov ecx,[rbx+vtype] ;
// lea eax,[rcx-8] ; cmp eax,2 ; jbe ; cmp ecx,1` -> aircraft = 8..10 or 1.
bool FindLegacyVehicleTypeLayout(VehicleTypeLayout& out) {
    auto* check = FindSite(kLegacyVehicleTypeCheckSignatures, [](const std::uint8_t* p) {
        if (p[0] != 0x48 || p[1] != 0x8B || (p[2] & 0xC0) != 0x80 || p[0xC] != 0x80 ||
            p[0xF] != kEntityTypeVehicle || p[0x19] != 0x8B || p[0x1F] != 0x8D || p[0x22] != 0x83)
            return false;
        int first = -static_cast<std::int8_t>(p[0x21]), span = p[0x24], single = p[0x29];
        std::uint32_t mask = ((1u << (span + 1)) - 1) << first | 1u << single;
        return first > 0 && first + span < 32 && single < 32 && mask == kGameAircraftMask;
    });
    if (!check) return false;
    std::memcpy(&out.cameraVehicle, check + 3, 4);
    out.entityType = check[0xE];
    std::memcpy(&out.vehicleType, check + 0x1B, 4);
    out.cameraBase = Base(check[2]);
    return true;
}

bool FindVehicleTypeLayout(VehicleTypeLayout& out) {
    if (GameImage::IsLegacy()) return FindLegacyVehicleTypeLayout(out);
    auto* check = FindSite(kVehicleTypeCheckSignatures, [](const std::uint8_t* p) {
        const auto* parent = p - 0x10;  // mov rdi,[camera+disp32] before the check
        std::uint32_t mask;
        std::memcpy(&mask, p + 0x12, 4);
        return parent[0] == 0x48 && parent[1] == 0x8B && (parent[2] & 0xC0) == 0x80 &&
               p[0] == 0x80 && p[3] == kEntityTypeVehicle && p[6] == 0x8B && p[0x11] == 0xB9 &&
               mask == kGameAircraftMask && p[2] < 0x80;
    });
    if (!check) return false;
    const auto* parent = check - 0x10;
    std::memcpy(&out.cameraVehicle, parent + 3, 4);
    out.entityType = check[2];
    std::memcpy(&out.vehicleType, check + 8, 4);
    out.cameraBase = Base(parent[2]);
    return true;
}

// The type check must read the camera from the store's base register. Legacy copies the camera
// (`mov rdi,rcx`) at the start of the same function, so check for that copy there.
bool SameCameraRegister(VehicleTypeLayout& layout, const std::uint8_t* store) {
    if (layout.cameraBase == Base(store[3])) return true;
    if (!GameImage::IsLegacy()) return false;
    DWORD64 imageBase = 0;
    const auto* fn = RtlLookupFunctionEntry(DWORD64(store), &imageBase, nullptr);
    if (!fn) return false;
    const auto* begin = reinterpret_cast<const std::uint8_t*>(imageBase + fn->BeginAddress);
    const auto* end = reinterpret_cast<const std::uint8_t*>(imageBase + fn->EndAddress);
    const std::uint8_t copy[] = {0x48, 0x8B, std::uint8_t(0xC0 | Base(store[3]) << 3 | layout.cameraBase)};
    constexpr int kPrologueBytes = 0x30;
    for (const auto* q = begin; q < begin + kPrologueBytes && q + 3 <= end; ++q)
        if (std::memcmp(q, copy, 3) == 0) {
            layout.cameraBase = Base(store[3]);
            return true;
        }
    return false;
}

void PatchVehicleNearClip() {
    if (!kVehicleNearClipEnabled) {
        Log::Info("near clip vehicle: disabled in this build; stays original");
        return;
    }
    auto* store = FindNearClipStore(GameImage::IsLegacy() ? std::span<const NearClipSignature>(kLegacyVehicleNearClipSignatures)
                                                          : std::span<const NearClipSignature>(kVehicleNearClipSignatures));
    VehicleTypeLayout layout{};
    if (!store || !FindVehicleTypeLayout(layout) || !SameCameraRegister(layout, store)) {
        Log::Info("near clip vehicle: not found; stays original");
        return;
    }
    auto rel8 = [](std::uint8_t* at, const std::uint8_t* target) {
        *at = std::uint8_t(target - (at + 1));
    };
    auto emit32 = [](std::int32_t v) {
        g_block.Emit({std::uint8_t(v), std::uint8_t(v >> 8), std::uint8_t(v >> 16), std::uint8_t(v >> 24)});
    };
    std::uint8_t rip = RipModrm(Reg(store[3]));
    const auto* stub = g_block.Cursor();
    g_block.Emit({0x9C, 0x50, 0x51});  // pushfq; push rax; push rcx
    g_block.Emit({0x48, 0x8B, std::uint8_t(0x80 | layout.cameraBase)});  // mov rax,[camera+veh]
    emit32(layout.cameraVehicle);
    g_block.Emit({0x48, 0x85, 0xC0, 0x74, 0});  // test rax,rax; jz skip
    auto* jz = const_cast<std::uint8_t*>(g_block.Cursor()) - 1;
    g_block.Emit({0x80, 0x78, layout.entityType, kEntityTypeVehicle, 0x75, 0});  // cmp byte; jne skip
    auto* jne = const_cast<std::uint8_t*>(g_block.Cursor()) - 1;
    g_block.Emit({0x8B, 0x80});  // mov eax,[rax+vehicleType]
    emit32(layout.vehicleType);
    g_block.Emit({0x83, 0xF8, 0x1F, 0x77, 0});  // cmp eax,31; ja skip
    auto* ja = const_cast<std::uint8_t*>(g_block.Cursor()) - 1;
    g_block.Emit({0xB9});  // mov ecx, kCabinVehicleMask
    emit32(std::int32_t(kCabinVehicleMask));
    g_block.Emit({0x0F, 0xA3, 0xC1, 0x73, 0});  // bt ecx,eax; jnc skip
    auto* jnc = const_cast<std::uint8_t*>(g_block.Cursor()) - 1;
    g_block.Emit({0xF3, 0x0F, 0x5D, rip});  // minss xmmN, [rip+cap]
    g_block.EmitRip32(const_cast<float*>(&g_shared->nearClipCap));
    const auto* skip = g_block.Cursor();
    for (auto* j : {jz, jne, ja, jnc}) rel8(j, skip);
    g_block.Emit({0x59, 0x58, 0x9D});  // pop rcx; pop rax; popfq
    g_block.Emit(Original(store));
    g_block.EmitJmp(store + kMovssLength);
    if (CodePatch::JumpToStub(store, Original(store), stub))
        Log::Info("near clip vehicle: capped at %.2f for cabin vehicles at exe+0x%zx "
                  "(vehicle +0x%x, entity type +0x%x, vehicle type +0x%x)",
                  kNearClipCap, ExeOffset(store), layout.cameraVehicle, layout.entityType,
                  layout.vehicleType);
    else
        Log::Info("near clip vehicle: patch refused; stays original");
}

// Legacy: `mov R,[camera+meta] ; movss xmmN,[R+nearClip]` (5 bytes) is patched; the stub caps
// xmmN when R is the on-foot metadata. The clamp to >= 0.01 and the store follow.
void PatchLegacyOnFootNearClip() {
    constexpr int kLength = 5;
    constexpr int kStoreSearchBytes = 0x18;
    auto* load = FindSite(kLegacyCameraNearClipSignatures, [](const std::uint8_t* p) {
        const auto* meta = p - 7;  // mov R,[camera+disp32]
        if (p[0] != 0xF3 || p[1] != 0x0F || p[2] != 0x10 || (p[3] & 0xC0) != 0x40 || Base(p[3]) == 4 ||
            meta[0] != 0x48 || meta[1] != 0x8B || (meta[2] & 0xC0) != 0x80 || Reg(meta[2]) != Base(p[3]) ||
            !NearClipFloorLoadedBefore(p))
            return false;
        for (const auto* q = p + kLength; q < p + kLength + kStoreSearchBytes; ++q)
            if (IsMovssDisp32(q, 0x11) && Base(q[3]) == Base(meta[2]) && StoresNearClip(q)) return true;
        return false;
    });
    if (!load) {
        Log::Info("near clip on foot: not found; stays original");
        return;
    }
    const std::uint8_t field = load[4];
    const auto* stub = g_block.Cursor();
    g_block.Emit(Original(load, kLength));
    g_block.Emit({0x9C, 0x48, 0x3B, RipModrm(Base(load[3]))});  // pushfq; cmp R,[rip+footMeta]
    g_block.EmitRip32(const_cast<const std::uint8_t**>(&g_shared->footMeta));
    g_block.Emit({0x75, 8});                                    // jne past the minss below
    g_block.Emit({0xF3, 0x0F, 0x5D, RipModrm(Reg(load[3]))});   // minss xmmN, [rip+cap]
    g_block.EmitRip32(const_cast<float*>(&g_shared->nearClipCap));
    g_block.Emit({0x9D});                                       // popfq
    g_block.EmitJmp(load + kLength);
    if (CodePatch::JumpToStub(load, Original(load, kLength), stub))
        Log::Info("near clip on foot: capped at %.2f at exe+0x%zx (metadata field +0x%x)",
                  kNearClipCap, ExeOffset(load), field);
    else
        Log::Info("near clip on foot: patch refused; stays original");
}

// The shared camera near-clip code is patched only for the on-foot first-person camera:
// rcx holds &metadata->nearClip, and our on-foot stub already knows that metadata.
void PatchOnFootNearClip() {
    if (GameImage::IsLegacy()) {
        PatchLegacyOnFootNearClip();
        return;
    }
    auto* store = FindNearClipStore(kCameraNearClipSignatures);
    // Expected before it: mov rcx,[rsi+meta]; add rcx,imm8 (field); ... cmove rdx,rcx.
    const auto* load = store ? store - 0x24 : nullptr;
    if (!store || load[0] != 0x48 || load[1] != 0x8B || load[2] != 0x8E || load[7] != 0x48 ||
        load[8] != 0x83 || load[9] != 0xC1 || load[0x14] != 0x48 || load[0x15] != 0x0F ||
        load[0x16] != 0x44 || load[0x17] != 0xD1) {
        Log::Info("near clip on foot: not found; stays original");
        return;
    }
    std::uint8_t field = load[10];
    const auto* stub = g_block.Cursor();
    g_block.Emit({0x9C, 0x50, 0x48, 0x8B, 0x05});  // pushfq; push rax; mov rax,[rip+footMeta]
    g_block.EmitRip32(const_cast<const std::uint8_t**>(&g_shared->footMeta));
    g_block.Emit({0x48, 0x83, 0xC0, field, 0x48, 0x39, 0xC8, 0x58});  // add rax,f; cmp rax,rcx; pop rax
    g_block.Emit({0x75, 8});  // jne past the minss below
    g_block.Emit({0xF3, 0x0F, 0x5D, RipModrm(Reg(store[3]))});  // minss xmmN, [rip+cap]
    g_block.EmitRip32(const_cast<float*>(&g_shared->nearClipCap));
    g_block.Emit({0x9D});  // popfq
    g_block.Emit(Original(store));
    g_block.EmitJmp(store + kMovssLength);
    if (CodePatch::JumpToStub(store, Original(store), stub))
        Log::Info("near clip on foot: capped at %.2f at exe+0x%zx (metadata field +0x%x)",
                  kNearClipCap, ExeOffset(store), field);
    else
        Log::Info("near clip on foot: patch refused; stays original");
}

float CurrentAspectRatio() {
    struct Search { DWORD pid; float aspect; long area; } s{GetCurrentProcessId(), 0, 0};
    EnumWindows(
        [](HWND w, LPARAM param) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(param);
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            RECT r{};
            if (pid != s.pid || !IsWindowVisible(w) || !GetClientRect(w, &r)) return TRUE;
            long area = long(r.right) * r.bottom;
            if (area > s.area && r.bottom > 0) s = {s.pid, float(r.right) / r.bottom, area};
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&s));
    return s.aspect > 0 ? s.aspect : kFallbackAspectRatio;
}

// The game's camera FOV is vertical; the player's target is horizontal.
float VerticalFromHorizontal(float horizontalDegrees, float aspect) {
    constexpr float kToRadians = std::numbers::pi_v<float> / 180.0f;
    return 2.0f * std::atan(std::tan(horizontalDegrees * kToRadians / 2.0f) / aspect) /
           kToRadians;
}

// Game heap objects are read from our own thread, so check the page before touching it.
template <typename T>
bool ReadAt(const std::uint8_t* base, int offset, T& out) {
    MEMORY_BASIC_INFORMATION m{};
    const auto* p = base + offset;
    if (!base || !VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT ||
        (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    std::memcpy(&out, p, sizeof out);
    return true;
}

// The on-foot FOV at slider position t, as the game would give it (original) and with our
// maximum (extended). False until the on-foot camera has run once (metadata unknown).
// The on-foot FOV at slider position t, as the game would give it (original) and with our
// maximum (extended). False until the on-foot camera has run once (metadata unknown).
struct SliderFov { float originalMax, original, extended; };
bool ReadSliderFov(float t, SliderFov& fov) {
    float minFov;
    if (!ReadAt(g_shared->footMeta, g_minFovOffset, minFov) ||
        !ReadAt(g_shared->footMeta, g_maxFovOffset, fov.originalMax) || !(minFov > 0) ||
        !(fov.originalMax >= minFov))
        return false;
    fov.original = minFov + (fov.originalMax - minFov) * t;
    fov.extended = minFov + (g_shared->maxVerticalFov - minFov) * t;
    return true;
}

}  // namespace

bool FieldOfView::Apply(float maxHorizontalDegrees, bool universal) {
    g_maxHorizontalFovDegrees = maxHorizontalDegrees;
    g_universal = universal;
    auto code = GameImage::Code();
    if (!g_block.Allocate(code.data())) {
        Log::Info("FOV: no memory within RIP reach; feature disabled, game untouched");
        return false;
    }
    g_shared = static_cast<Shared*>(g_block.Data());
    g_shared->maxVerticalFov = VerticalFromHorizontal(g_maxHorizontalFovDegrees, kFallbackAspectRatio);
    g_shared->scale = 1.0f;
    g_shared->chosenFov = 0.0f;
    g_shared->nearClipCap = kNearClipCap;
    if (!PatchOnFoot()) {
        Log::Info("FOV: on-foot slider formula not found or refused; FOV feature disabled");
        return false;
    }
    Log::Info("FOV: %s", g_universal ? "universal mode (hip aim, ADS and sights use the slider FOV)"
                                       : "proportional mode (aim and vehicle cameras scale with the slider)");
    PatchHipAim();
    PatchAds();
    PatchSight();
    PatchStateFov();
    PatchVehicle();  // also finds the frame FOV offset the near-clip checks rely on
    PatchVehicleNearClip();
    PatchOnFootNearClip();
    return true;
}

int FieldOfView::FrameFovOffset() { return g_frameFovOffset; }

float FieldOfView::SliderWidening() { return g_shared ? g_shared->scale : 1.0f; }

bool FieldOfView::Tracked(Heartbeat beat) { return g_heartbeatTracked[int(beat)]; }

std::uint32_t FieldOfView::Updates(Heartbeat beat) { return g_shared ? g_shared->heartbeats[int(beat)] : 0; }

float FieldOfView::SightGameFov() { return g_shared ? g_shared->sightGameFov : 0.0f; }


void FieldOfView::Track() {
    float lastAspect = 0, lastShown = 0;
    std::int32_t lastPosition = -1;
    for (;;) {
        float aspect = CurrentAspectRatio();
        if (std::fabs(aspect - lastAspect) > 0.001f) {
            g_shared->maxVerticalFov = VerticalFromHorizontal(g_maxHorizontalFovDegrees, aspect);
            Log::Info("FOV: aspect %.4f -> max %.2f deg vertical", aspect, g_shared->maxVerticalFov);
            lastAspect = aspect;
        }
        std::int32_t position = *g_sliderPosition;
        if (position != lastPosition) {
            Log::Info("FOV: slider position %d/%.0f", position, kSliderSteps);
            lastPosition = position;
        }
        float t = std::fmin(std::fmax(position / kSliderSteps, 0.0f), 1.0f);
        SliderFov fov{};
        bool known = ReadSliderFov(t, fov);
        g_shared->scale = known ? fov.extended / fov.original : 1.0f;
        g_shared->chosenFov = known && g_universal ? fov.extended : 0.0f;
        float shown = g_universal ? g_shared->chosenFov : g_shared->scale;
        if (std::fabs(shown - lastShown) > 0.0005f) {
            if (g_universal)
                Log::Info("FOV: original max %.2f, universal FOV %.2f deg vertical", fov.originalMax, shown);
            else
                Log::Info("FOV: original max %.2f, aim/vehicle scale %.4f", fov.originalMax, shown);
            lastShown = shown;
        }
        Sleep(250);
    }
}

bool FieldOfView::SightWeaponOffset(float out[3]) {
    if (!g_weaponPose.found || !g_shared) return false;
    const std::uint8_t* camera = g_shared->sightCamera;
    const std::uint8_t* weapon = g_shared->weaponObject;
    if (!camera || !weapon) return false;
    // Read with ReadProcessMemory: the pointers come from the game thread and may be stale here.
    auto read3 = [](const std::uint8_t* at, float (&v)[3]) {
        SIZE_T got = 0;
        return ReadProcessMemory(GetCurrentProcess(), at, v, sizeof v, &got) && got == sizeof v;
    };
    float position[3], forward[3], weaponPosition[3];
    if (!read3(camera + g_weaponPose.cameraPosition, position) || !read3(camera + g_weaponPose.cameraForward, forward) ||
        !read3(weapon + g_weaponPose.entityPosition, weaponPosition))
        return false;
    // First-person cameras do not roll: right = forward x up(z), up = right x forward.
    float fl = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    float right[3] = {forward[1], -forward[0], 0.0f};
    float rl = std::sqrt(right[0] * right[0] + right[1] * right[1]);
    if (!(fl > 0.5f && fl < 2.0f) || rl < 1e-3f) return false;
    for (float& f : forward) f /= fl;
    right[0] /= rl;
    right[1] /= rl;
    float up[3] = {right[1] * forward[2] - right[2] * forward[1], right[2] * forward[0] - right[0] * forward[2],
                   right[0] * forward[1] - right[1] * forward[0]};
    float d[3] = {weaponPosition[0] - position[0], weaponPosition[1] - position[1], weaponPosition[2] - position[2]};
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    out[0] = dot(d, right);
    out[1] = dot(d, forward);
    out[2] = dot(d, up);
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

const void* FieldOfView::SightWeapon() { return g_shared ? g_shared->weaponObject : nullptr; }
