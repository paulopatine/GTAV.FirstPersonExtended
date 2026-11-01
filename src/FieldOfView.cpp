// First-person FOV, as found in 1.0.1158.16 (details and regeneration steps: docs/patterns.md).
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

// Hip aim in first person (aiming without the sights): lerp of the weapon's aim FOV range.
constexpr Signature kHipAimSignatures[] = {
    {"hipaim_fov_full",
     "F3 0F 2A 15 ?? ?? ?? ?? F3 0F 5E 15 ?? ?? ?? ?? F3 0F 10 1D ?? ?? ?? ?? F3 0F 5D DA "
     "0F 57 E4 F3 0F C2 D4 01 0F 55 D3 F3 0F 5C C8 F3 0F 59 CA F3 0F 58 C8 0F 28 D1 "
     "F3 0F 11 96 ?? ?? ?? ??",
     54},
    {"hipaim_fov_tail", "F3 0F 5C C8 F3 0F 59 CA F3 0F 58 C8 0F 28 D1 F3 0F 11 96 ?? ?? ?? ??", 15},
};

// Aim down sights: the camera eases towards a target FOV held by a shared aiming object.
#ifdef FPFIX_SIMULATE_SCAN_FAILURE
// Test build (make failtest): proves a broken signature disables only its own feature.
constexpr Signature kAdsSignatures[] = {
    {"ads_fov_full (corrupted on purpose)", "DE AD BE EF DE AD BE EF DE AD BE EF", 0},
    {"ads_fov_tail (corrupted on purpose)", "DE AD BE EF DE AD BE EF DE AD BE EF", 0},
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

// A camera state with its own fixed target FOV from the metadata (e.g. holding the phone):
// target = metadata->stateFov, bypassing the slider.
constexpr Signature kStateFovSignatures[] = {
    {"state_fov_full",
     "F3 0F 10 89 ?? ?? ?? ?? F3 0F 10 91 ?? ?? ?? ?? F3 0F 59 15 ?? ?? ?? ?? F3 0F 59 C2 "
     "F3 0F 11 8E ?? ?? ?? ??",
     0x1C},
    {"state_fov_tail", "F3 0F 59 15 ?? ?? ?? ?? F3 0F 59 C2 F3 0F 11 8E ?? ?? ?? ?? 0F B6 8E", 0xC},
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

// Shared by every camera: near clip = metadata->nearClip (or an override), at least 0.01.
constexpr NearClipSignature kCameraNearClipSignatures[] = {
    {"camera_nearclip_full",
     "48 8B 8E ?? ?? ?? ?? 48 83 C1 ?? 48 8D 96 ?? ?? ?? ?? A8 ?? 48 0F 44 D1 F3 0F 10 02 "
     "F3 0F 5F 05 ?? ?? ?? ?? F3 0F 11 86 ?? ?? ?? ??",
     0x24},
    {"camera_nearclip_tail", "48 0F 44 D1 F3 0F 10 02 F3 0F 5F 05 ?? ?? ?? ?? F3 0F 11 86 ?? ?? ?? ??",
     0x10},
};

// Values shared with the stubs; lives at the start of the CodeBlock page.
struct Shared {
    volatile float maxVerticalFov;          // read by the on-foot stub
    volatile float scale;                   // read by the aim and vehicle stubs
    const std::uint8_t* volatile footMeta;  // written by the on-foot stub: camera metadata
    volatile float nearClipCap;             // read by the near-clip stubs
    volatile float chosenFov;               // universal mode: on-foot FOV for the slider; 0 = unknown
    volatile std::uint32_t heartbeats[2];   // bumped by the vehicle / sight stubs on every update
    volatile float sightGameFov;            // the game's own sight FOV, before our adjustment
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
bool g_heartbeatTracked[2] = {};            // the stub of that camera counts its updates

bool PatchOnFoot() {
    auto* load = FindSite(kOnFootSignatures, [](const std::uint8_t* p) {
        // Right before: `movss xmm1,[base+imm8]` loading the minimum from the same metadata.
        return IsMovssDisp32(p, 0x10) && p[-5] == 0xF3 && p[-4] == 0x0F && p[-3] == 0x10 &&
               (p[-2] & 0xC0) == 0x40 && Base(p[-2]) == Base(p[3]) &&
               (g_sliderPosition = SliderReadBefore(p));
    });
    if (!load) return false;
    g_minFovOffset = load[-1];
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
    auto* store = FindSite(kHipAimSignatures, [](const std::uint8_t* p) {
        return IsMovssDisp32(p, 0x11) && SliderReadBefore(p) == g_sliderPosition;
    });
    if (store) std::memcpy(&g_cameraTargetFovOffset, store + 4, 4);
    PatchScaled("hip aim", store, /*scaleBefore=*/true, /*aiming=*/true);
}

void PatchSight() {
    // Hit is `ucomiss xmm,[rip+1.0]; jae store`; the store must write the camera target FOV.
    auto* check = FindSite(kSightSignatures, [](const std::uint8_t* p) {
        if (RipFloat(p, 3, 7) != 1.0f || p[7] != 0x0F || p[8] != 0x83) return false;
        const auto* store = Pattern::ResolveRip(p + 7, 2, 6);
        std::int32_t storeOffset;
        std::memcpy(&storeOffset, store + 4, 4);
        return IsMovssDisp32(store, 0x11) && Reg(store[3]) == Reg(p[2]) &&
               g_cameraTargetFovOffset >= 0 && storeOffset == g_cameraTargetFovOffset;
    });
    auto* store = check ? const_cast<std::uint8_t*>(Pattern::ResolveRip(check + 7, 2, 6)) : nullptr;
    if (!store) {
        Log::Info("FOV sight: not found; stays original");
        return;
    }
    // Before our adjustment the register holds the game's own sight FOV: keep it for the player
    // FOV feature (the weapon then looks as in the original game), and count the update.
    const auto* stub = g_block.Cursor();
    g_block.Emit({0xF3, 0x0F, 0x11, RipModrm(Reg(store[3]))});  // movss [rip+sightGameFov], xmmN
    g_block.EmitRip32(const_cast<float*>(&g_shared->sightGameFov));
    EmitFovAdjust(Reg(store[3]), /*aiming=*/true);
    g_block.Emit(Original(store));
    EmitHeartbeat(FieldOfView::Heartbeat::SightCamera);
    g_block.EmitJmp(store + kMovssLength);
    if (CodePatch::JumpToStub(store, Original(store), stub)) {
        g_heartbeatTracked[int(FieldOfView::Heartbeat::SightCamera)] = true;
        Log::Info("FOV sight: patched at exe+0x%zx", ExeOffset(store));
    } else {
        Log::Info("FOV sight: patch refused; stays original");
    }
}

void PatchStateFov() {
    auto* store = FindSite(kStateFovSignatures, [](const std::uint8_t* p) {
        // Load of metadata->stateFov into the same register, then stored as the target FOV.
        const auto* load = p - 0x1C;
        std::int32_t storeOffset;
        std::memcpy(&storeOffset, p + 4, 4);
        return IsMovssDisp32(load, 0x10) && IsMovssDisp32(p, 0x11) && Reg(load[3]) == Reg(p[3]) &&
               g_cameraTargetFovOffset >= 0 && storeOffset == g_cameraTargetFovOffset;
    });
    PatchScaled("fixed-FOV state (phone)", store, /*scaleBefore=*/true);
}

void PatchAds() {
    auto* load = FindSite(kAdsSignatures, [](const std::uint8_t* p) {
        // target = aiming->adsFov; current = camera->fov; current + (target-current)*blend
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

void PatchVehicle() {
    auto* load = FindSite(kVehicleSignatures, [](const std::uint8_t* p) {
        // Followed by the camera FOV clamp [1, 130] before the frame FOV is written.
        return IsMovssDisp32(p, 0x10) && p[8] == 0x0F && p[9] == 0x2E && p[10] == 0x35 &&
               RipFloat(p + 8, 3, 7) == kFovClampMin && p[21] == 0xF3 && p[22] == 0x0F &&
               p[23] == 0x10 && p[24] == 0x05 && RipFloat(p + 21, 4, 8) == kFovClampMax;
    });
    if (!load) {
        Log::Info("FOV vehicle: not found; vehicle FOV stays original");
        return;
    }
    if (const auto* store = load + kFrameFovStoreOffset; IsMovssDisp32(store, 0x11))
        std::memcpy(&g_frameFovOffset, store + 4, 4);
    const auto* stub = g_block.Cursor();
    g_block.Emit(Original(load));
    EmitFovAdjust(Reg(load[3]), /*aiming=*/false);
    EmitHeartbeat(FieldOfView::Heartbeat::VehicleCamera);
    g_block.EmitJmp(load + kMovssLength);
    if (CodePatch::JumpToStub(load, Original(load), stub)) {
        g_heartbeatTracked[int(FieldOfView::Heartbeat::VehicleCamera)] = true;
        Log::Info("FOV vehicle: patched at exe+0x%zx", ExeOffset(load));
    }
    else
        Log::Info("FOV vehicle: patch refused; vehicle FOV stays original");
}

// Store of the clamped near clip: `maxss xmmN,[rip+0.01]; movss [camera+off],xmmN`, where
// off sits right after the frame FOV field.
std::uint8_t* FindNearClipStore(std::span<const NearClipSignature> signatures) {
    for (const auto& sig : signatures) {
        auto hits = Pattern::FindAll(GameImage::Code(), sig.bytes);
        Log::Info("scan %s: %zu match(es)", sig.name, hits.size());
        if (hits.size() != 1) continue;
        auto* p = const_cast<std::uint8_t*>(hits[0] + sig.storeOffset);
        const auto* maxss = p - kMovssLength;
        std::int32_t offset;
        std::memcpy(&offset, p + 4, 4);
        if (maxss[0] == 0xF3 && maxss[1] == 0x0F && maxss[2] == 0x5F && (maxss[3] & 0xC7) == 0x05 &&
            RipFloat(maxss, 4, 8) == kNearClipFloor && IsMovssDisp32(p, 0x11) &&
            Reg(p[3]) == Reg(maxss[3]) && g_frameFovOffset >= 0 && offset == g_frameFovOffset + 4)
            return p;
        Log::Info("scan %s: match failed semantic validation", sig.name);
    }
    return nullptr;
}

bool FindVehicleTypeLayout(VehicleTypeLayout& out) {
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

void PatchVehicleNearClip() {
    if (!kVehicleNearClipEnabled) {
        Log::Info("near clip vehicle: disabled in this build; stays original");
        return;
    }
    auto* store = FindNearClipStore(kVehicleNearClipSignatures);
    VehicleTypeLayout layout{};
    if (!store || !FindVehicleTypeLayout(layout) || layout.cameraBase != Base(store[3])) {
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

// The shared camera near-clip code is patched only for the on-foot first-person camera:
// rcx holds &metadata->nearClip, and our on-foot stub already knows that metadata.
void PatchOnFootNearClip() {
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
