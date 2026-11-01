// Player FOV ("viewmodel"), as found in 1.0.1158.16 (study: docs/viewmodel-study.md, patterns and
// regeneration: docs/patterns.md).
//
// In first person the game draws the player's body (draw-list command 0x84) and the weapon in the
// hands (command 0x82) through a private copy of the current viewport: it copies the viewport,
// makes the copy camera-relative with SetCameraMtx(copy, ...), makes it current with
// SetCurrent(copy, regenerate), draws, and restores the previous one. Both call sites look alike:
//     call SetCameraMtx ; mov rcx, <copy> ; mov dl, 1 ; xor r8d, r8d ; call SetCurrent
// Each `call SetCameraMtx` is redirected to a stub that first narrows the copy's FOV back to what
// the same camera had in the original game (copy FOV / slider widening) and has the game rebuild
// the copy's projection; then it continues into SetCameraMtx. The shared viewport, the world,
// shadows, reflections and third person never pass here, so they keep the wide FOV.
//
// The body and the weapon are patched together or not at all: one without the other would take
// the hands off the weapon. In vehicles the player keeps the world's FOV (the hands hold parts of
// the vehicle, which is world), detected through the FOV feature's vehicle-camera heartbeat.
//
// Two scopes: every first-person camera (PlayerFov=1), or only while looking through the weapon's
// sight (the default when UniversalFov=1: the sight then uses the wide slider FOV, and this keeps
// the weapon as in the original game so the sights stay aligned). Through the sight the player
// gets the sight's own FOV as the game set it (kept by the FOV feature's sight stub); elsewhere,
// the camera's FOV divided by the slider widening. The sight is detected through the FOV
// feature's sight-camera heartbeat.
#include "PlayerFov.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "CodeBlock.h"
#include "FieldOfView.h"
#include "GameImage.h"
#include "Log.h"
#include "MemoryPatch.h"
#include "Pattern.h"

namespace {

constexpr float kHalfDegreeInRadians = 0.00872665f;  // pi/360: the builder's fov -> tan(fov/2) factor
constexpr int kCallLength = 5;                         // E8 rel32
// The weapon is narrowed only while the first-person body was drawn this recently: the body
// passes here once per frame in first person and never in third person.
constexpr ULONGLONG kBodyRecentMs = 250;
// A heartbeat counts as active while its camera updated this recently (cameras update every
// frame; measured: the sight heartbeat never gaps while looking through the sight).
constexpr ULONGLONG kCameraRecentMs = 100;
// Changes of the player's FOV are smoothed (critically damped, settling in about 0.25 s) so the
// weapon eases between sizes instead of snapping.
constexpr float kSmoothSeconds = 0.08f;
// Cube-map and reflection faces are square; they are never the player's own view.
constexpr float kSquareAspectTolerance = 0.01f;

struct Signature {
    const char* name;
    const char* bytes;
    int offset;  // from the match to the site this signature locates
};

// Projection builder (grcViewport): reads near/far, aspect and fov from the viewport and writes
// both projection matrices. The located site is `mov rsi, rcx` right after the prologue.
constexpr Signature kProjectionBuilderSignatures[] = {
    {"player_projection_full",
     "56 57 48 81 EC ?? ?? ?? ?? 44 0F 29 AC 24 ?? ?? ?? ?? 44 0F 29 A4 24 ?? ?? ?? ?? 44 0F 29 5C 24 ?? "
     "44 0F 29 54 24 ?? 44 0F 29 4C 24 ?? 44 0F 29 44 24 ?? 0F 29 7C 24 ?? 0F 29 74 24 ?? "
     "48 89 CE 80 B9 ?? ?? ?? ?? 00 F3 0F 10 81 ?? ?? ?? ?? F3 44 0F 10 A1 ?? ?? ?? ?? 74 05 0F 28 F8 "
     "EB 08 41 0F 28 FC 44 0F 28 E0 F3 44 0F 10 8E ?? ?? ?? ?? F3 44 0F 10 05 ?? ?? ?? ?? "
     "F3 0F 10 86 ?? ?? ?? ?? F3 41 0F 59 C0",
     0x3d},
    {"player_projection_tail",
     "48 89 CE 80 B9 ?? ?? ?? ?? 00 F3 0F 10 81 ?? ?? ?? ?? F3 44 0F 10 A1 ?? ?? ?? ?? 74 ?? 0F 28 F8 "
     "EB ?? 41 0F 28 FC 44 0F 28 E0 F3 44 0F 10 8E ?? ?? ?? ?? F3 44 0F 10 05 ?? ?? ?? ?? "
     "F3 0F 10 86 ?? ?? ?? ?? F3 41 0F 59 C0",
     0},
};
// Positions inside the matched block, counted from `mov rsi, rcx`.
constexpr int kAspectDisp = 0x2f;      // movss xmm9, [rsi+aspect]
constexpr int kHalfDegreeLoad = 0x33;  // movss xmm8, [rip+pi/360] (9 bytes, disp at +5)
constexpr int kFovDisp = 0x40;         // movss xmm0, [rsi+fov]
constexpr int kSecondFovSearch = 0x180;
constexpr const char* kSecondFovLoad = "F3 44 0F 59 86";  // mulss xmm8, [rsi+secondFov]

// The two `call SetCameraMtx` on the private copy. Primary signatures include the leas that
// prepare the camera; fallbacks start at `mov rcx, <copy>`. Located site: the call.
constexpr Signature kBodySignatures[] = {
    {"player_body_full",
     "48 8D AC 24 ?? ?? ?? ?? 48 8D 94 24 ?? ?? ?? ?? 4C 8D 84 24 ?? ?? ?? ?? 48 89 E9 E8 ?? ?? ?? ?? "
     "48 89 E9 B2 01 45 31 C0 E8 ?? ?? ?? ?? 49 89 C7",
     27},
    {"player_body_tail", "48 89 E9 E8 ?? ?? ?? ?? 48 89 E9 B2 01 45 31 C0 E8 ?? ?? ?? ?? 49 89 C7", 3},
};
#ifdef FPFIX_SIMULATE_SCAN_FAILURE
// Test build (make failtest): the weapon site is missing, so the body must stay original too.
constexpr Signature kWeaponSignatures[] = {
    {"player_weapon_full (corrupted on purpose)", "DE AD BE EF DE AD BE EF DE AD BE EF", 0},
    {"player_weapon_tail (corrupted on purpose)", "DE AD BE EF DE AD BE EF DE AD BE EF", 0},
};
#else
constexpr Signature kWeaponSignatures[] = {
    {"player_weapon_full",
     "48 8D 9C 24 ?? ?? ?? ?? 48 8D 94 24 ?? ?? ?? ?? 4C 8D 84 24 ?? ?? ?? ?? 48 89 D9 E8 ?? ?? ?? ?? "
     "48 89 D9 B2 01 45 31 C0 E8 ?? ?? ?? ?? 48 89 84 24",
     27},
    {"player_weapon_tail", "48 89 D9 E8 ?? ?? ?? ?? 48 89 D9 B2 01 45 31 C0 E8 ?? ?? ?? ?? 48 89 84 24", 3},
};
#endif
constexpr int kSetCurrentCall = 13;  // from `call SetCameraMtx` to `call SetCurrent`

enum class PrivateDraw : int { Body = 0, Weapon = 1 };

struct Projection {
    std::int32_t aspect = 0, fov = 0, secondFov = 0;  // viewport fields (offsets)
    void (*rebuild)(std::uint8_t* viewport) = nullptr;
};

Projection g_projection;
CodeBlock g_block;
std::atomic<ULONGLONG> g_lastBodyMs{0};
bool g_everyCamera = false;  // false: only while looking through the sight

// Tells whether a FOV-feature camera is active: its update count changed within kCameraRecentMs.
class RecentCamera {
public:
    explicit RecentCamera(FieldOfView::Heartbeat beat) : m_beat(beat) {}
    bool Active(ULONGLONG now) {
        std::uint32_t updates = FieldOfView::Updates(m_beat);
        if (m_seen.exchange(updates, std::memory_order_relaxed) != updates)
            m_lastChangeMs.store(now, std::memory_order_relaxed);
        return now - m_lastChangeMs.load(std::memory_order_relaxed) < kCameraRecentMs;
    }

private:
    FieldOfView::Heartbeat m_beat;
    std::atomic<std::uint32_t> m_seen{0};
    std::atomic<ULONGLONG> m_lastChangeMs{0};
};
// In a vehicle the hands hold the wheel or handlebars, which belong to the world.
RecentCamera g_vehicleCamera{FieldOfView::Heartbeat::VehicleCamera};
RecentCamera g_sightCamera{FieldOfView::Heartbeat::SightCamera};

double NowSeconds() {
    static const double ticksPerSecond = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) / ticksPerSecond;
}

// The player's FOV is the world's minus an offset; the offset is what gets smoothed, so with the
// feature off (offset 0) the player follows the world exactly, even while the game zooms it.
// Shared by every render thread so the body and the weapon move together.
class SmoothedOffset {
public:
    float Towards(float target, double now) {
        std::lock_guard lock(m_mutex);
        float dt = m_last > 0 ? float(now - m_last) : 0.0f;
        m_last = now;
        if (dt <= 0.0f) return m_value;
        // Critically damped spring (no overshoot, zero speed at both ends).
        float omega = 2.0f / kSmoothSeconds, x = omega * dt;
        float decay = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
        float change = m_value - target, temp = (m_speed + omega * change) * dt;
        m_speed = (m_speed - omega * temp) * decay;
        float next = target + (change + temp) * decay;
        if ((target - m_value > 0.0f) == (next > target)) next = target, m_speed = 0.0f;
        m_value = next;
        return m_value;
    }

private:
    std::mutex m_mutex;
    float m_value = 0.0f, m_speed = 0.0f;
    double m_last = 0.0;
};
SmoothedOffset g_offset;
std::atomic<bool> g_loggedBody{false}, g_loggedWeapon{false};
std::atomic<int> g_bodyNarrowed{-1};  // last on/off logged for the body (-1: none yet)

size_t ExeOffset(const void* p) {
    return size_t(static_cast<const std::uint8_t*>(p) -
                  reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr)));
}

template <typename Validate>
const std::uint8_t* FindSite(std::span<const Signature> signatures, Validate isValid) {
    for (const auto& sig : signatures) {
        auto hits = Pattern::FindAll(GameImage::Code(), sig.bytes);
        Log::Info("scan %s: %zu match(es)", sig.name, hits.size());
        if (hits.size() != 1) continue;
        const auto* site = hits[0] + sig.offset;
        if (isValid(site)) return site;
        Log::Info("scan %s: match failed semantic validation", sig.name);
    }
    return nullptr;
}

const std::uint8_t* FunctionStart(const std::uint8_t* p) {
    DWORD64 base = 0;
    auto* fn = RtlLookupFunctionEntry(DWORD64(p), &base, nullptr);
    return fn ? reinterpret_cast<const std::uint8_t*>(base + fn->BeginAddress) : nullptr;
}

const std::uint8_t* CallTarget(const std::uint8_t* call) {
    return call[0] == 0xE8 ? Pattern::ResolveRip(call, 1, kCallLength) : nullptr;
}

std::int32_t Disp32(const std::uint8_t* p) {
    std::int32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

bool IsViewportField(std::int32_t offset) { return offset > 0 && offset < 0x1000; }

bool FindProjectionBuilder() {
    const auto* site = FindSite(kProjectionBuilderSignatures, [](const std::uint8_t* p) {
        float halfDegree;
        std::memcpy(&halfDegree, Pattern::ResolveRip(p + kHalfDegreeLoad, 5, 9), 4);
        auto start = FunctionStart(p);
        auto rest = std::span<const std::uint8_t>(p, kSecondFovSearch);
        auto second = Pattern::FindAll(rest, kSecondFovLoad);
        if (std::fabs(halfDegree - kHalfDegreeInRadians) > 1e-7f || !start || p - start >= 0x80 ||
            second.size() != 1)
            return false;
        Projection found{Disp32(p + kAspectDisp), Disp32(p + kFovDisp), Disp32(second[0] + 5),
                         reinterpret_cast<void (*)(std::uint8_t*)>(const_cast<std::uint8_t*>(start))};
        if (!IsViewportField(found.aspect) || !IsViewportField(found.fov) ||
            !IsViewportField(found.secondFov) || found.fov == found.secondFov)
            return false;
        g_projection = found;
        return true;
    });
    if (site)
        Log::Info("player FOV: projection builder at exe+0x%zx (aspect +0x%x, fov +0x%x, second fov +0x%x)",
                  ExeOffset(reinterpret_cast<const void*>(g_projection.rebuild)), g_projection.aspect,
                  g_projection.fov, g_projection.secondFov);
    return site != nullptr;
}

float ReadFloat(const std::uint8_t* p, std::int32_t offset) {
    float v;
    std::memcpy(&v, p + offset, 4);
    return v;
}

void WriteFloat(std::uint8_t* p, std::int32_t offset, float v) { std::memcpy(p + offset, &v, 4); }

bool IsCameraFov(float degrees) { return degrees > 1.0f && degrees < 130.0f; }

}  // namespace

// Runs on render threads right before SetCameraMtx(copy, ...): `viewport` is the game's private
// copy for this draw, so changing it affects nothing else.
extern "C" void PlayerFovOnPrivateViewport(std::uint8_t* viewport, int kind) {
    float widening = FieldOfView::SliderWidening();
    if (!(widening > 1.0005f)) return;  // slider at the original range: nothing to undo
    if (std::fabs(ReadFloat(viewport, g_projection.aspect) - 1.0f) < kSquareAspectTolerance) return;
    ULONGLONG now = GetTickCount64();
    bool body = PrivateDraw(kind) == PrivateDraw::Body;
    if (body)
        g_lastBodyMs.store(now, std::memory_order_relaxed);
    else if (now - g_lastBodyMs.load(std::memory_order_relaxed) > kBodyRecentMs)
        return;  // a camera-relative object without the first-person body: not the player's weapon
    float fov = ReadFloat(viewport, g_projection.fov);
    float secondFov = ReadFloat(viewport, g_projection.secondFov);
    if (!IsCameraFov(fov) || !IsCameraFov(secondFov)) return;
    // Both checks run every time so each heartbeat's last change stays current.
    bool inVehicle = g_vehicleCamera.Active(now), throughSight = g_sightCamera.Active(now);
    bool narrow = !inVehicle && (g_everyCamera || throughSight);
    // Through the sight the weapon looks exactly as in the original game: the sight's own FOV.
    // Elsewhere: the FOV this camera had before the slider widened it.
    float sightFov = FieldOfView::SightGameFov();
    float playerFov = !narrow ? fov : throughSight && IsCameraFov(sightFov) ? sightFov : fov / widening;
    // Transitions only (rare): shows in the log when the original FOV turns on and off.
    if (body && g_bodyNarrowed.exchange(narrow) != int(narrow))
        Log::Info("player FOV: %s%s (%.2f deg vertical)", narrow ? "on" : "off",
                  inVehicle ? " (vehicle camera)" : throughSight ? " (looking through the sight)" : "", playerFov);
    float offset = g_offset.Towards(fov - playerFov, NowSeconds());
    if (std::fabs(offset) < 0.01f) return;  // at the world's FOV: leave the copy as the game made it
    float narrowed = fov - offset;
    if (!IsCameraFov(narrowed)) return;
    WriteFloat(viewport, g_projection.fov, narrowed);
    WriteFloat(viewport, g_projection.secondFov, secondFov * (narrowed / fov));
    g_projection.rebuild(viewport);
    auto& logged = body ? g_loggedBody : g_loggedWeapon;
    if (!logged.exchange(true))
        Log::Info("player FOV: first %s draw narrowed (%.2f -> %.2f deg vertical)", body ? "body" : "weapon",
                  fov, narrowed);
}

namespace {

// Stub reached by the redirected `call`: keeps SetCameraMtx's arguments, runs the adjustment,
// then continues into SetCameraMtx as if called directly. Entry rsp is 8 mod 16 (after the call):
// 4 pushes + sub 0x28 realign it for the C++ call.
const std::uint8_t* EmitStub(PrivateDraw kind, const std::uint8_t* setCameraMtx) {
    const auto* stub = g_block.Cursor();
    g_block.Emit({0x51, 0x52, 0x41, 0x50, 0x41, 0x51});  // push rcx, rdx, r8, r9
    g_block.Emit({0x48, 0x83, 0xEC, 0x28});              // sub rsp, 0x28
    g_block.Emit({0xBA});                                // mov edx, kind
    std::int32_t k = std::int32_t(kind);
    g_block.Emit(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&k), 4));
    g_block.Emit({0x48, 0xB8});                          // mov rax, handler ; call rax
    auto handler = reinterpret_cast<std::uint64_t>(&PlayerFovOnPrivateViewport);
    g_block.Emit(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&handler), 8));
    g_block.Emit({0xFF, 0xD0});
    g_block.Emit({0x48, 0x83, 0xC4, 0x28});              // add rsp, 0x28
    g_block.Emit({0x41, 0x59, 0x41, 0x58, 0x5A, 0x59});  // pop r9, r8, rdx, rcx
    g_block.EmitJmp(setCameraMtx);
    return stub;
}

bool RedirectCall(std::uint8_t* call, const std::uint8_t* stub) {
    std::uint8_t replacement[kCallLength] = {0xE8};
    std::int32_t rel = std::int32_t(stub - (call + kCallLength));
    std::memcpy(replacement + 1, &rel, 4);
    return MemoryPatch::Replace(call, std::span<const std::uint8_t>(call, kCallLength), replacement);
}

}  // namespace

bool PlayerFov::Apply(bool everyCamera) {
    g_everyCamera = everyCamera;
    if (!FieldOfView::Tracked(FieldOfView::Heartbeat::VehicleCamera)) {
        Log::Info("player FOV: vehicle camera not tracked (FOV vehicle site missing); stays original");
        return false;
    }
    if (!everyCamera && !FieldOfView::Tracked(FieldOfView::Heartbeat::SightCamera)) {
        Log::Info("player FOV: sight camera not tracked (FOV sight site missing); stays original");
        return false;
    }
    if (!FindProjectionBuilder()) {
        Log::Info("player FOV: projection builder not found or did not validate; stays original");
        return false;
    }
    auto isPrivateCall = [](const std::uint8_t* p) {
        auto* target = CallTarget(p);
        return target && FunctionStart(target) == target && CallTarget(p + kSetCurrentCall);
    };
    auto* body = const_cast<std::uint8_t*>(FindSite(kBodySignatures, isPrivateCall));
    auto* weapon = const_cast<std::uint8_t*>(FindSite(kWeaponSignatures, isPrivateCall));
    // Same SetCameraMtx and same SetCurrent at both sites: they are the two private viewports.
    if (!body || !weapon || CallTarget(body) != CallTarget(weapon) ||
        CallTarget(body + kSetCurrentCall) != CallTarget(weapon + kSetCurrentCall)) {
        Log::Info("player FOV: a private viewport site was not found or did not validate; stays original");
        return false;
    }
    if (!g_block.Allocate(GameImage::Code().data())) {
        Log::Info("player FOV: no memory within RIP reach; stays original");
        return false;
    }
    const auto* setCameraMtx = CallTarget(body);
    const auto* bodyStub = EmitStub(PrivateDraw::Body, setCameraMtx);
    const auto* weaponStub = EmitStub(PrivateDraw::Weapon, setCameraMtx);
    if (!RedirectCall(body, bodyStub)) {
        Log::Info("player FOV: body patch refused; stays original");
        return false;
    }
    if (!RedirectCall(weapon, weaponStub)) {
        // Undo the body: never leave one without the other.
        std::uint8_t original[kCallLength] = {0xE8};
        std::int32_t rel = std::int32_t(setCameraMtx - (body + kCallLength));
        std::memcpy(original + 1, &rel, 4);
        MemoryPatch::Replace(body, std::span<const std::uint8_t>(body, kCallLength), original);
        Log::Info("player FOV: weapon patch refused; body restored; stays original");
        return false;
    }
    Log::Info("player FOV: body patched at exe+0x%zx, weapon at exe+0x%zx (%s)", ExeOffset(body), ExeOffset(weapon),
              everyCamera ? "every first-person camera" : "only while looking through the sight");
    return true;
}
