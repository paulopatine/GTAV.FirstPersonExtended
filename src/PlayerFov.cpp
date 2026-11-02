// Player FOV ("viewmodel"), as found in Enhanced 1.0.1158.16 and Legacy 1.0.3889.0 (study:
// docs/viewmodel-study.md, patterns and regeneration: docs/patterns.md).
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
// Enhanced rebuilds the copy's projection with its builder after the FOV fields are written. Legacy
// keeps a different viewport layout and only builds the projection in grcViewport::Perspective(vp,
// fovY, aspect, near, far), so there the copy is narrowed by calling Perspective with its own aspect
// and clip planes. Either way only the copy changes (Legacy uploads constants only for the current
// viewport, which the copy is not yet).
//
// The body and the weapon are patched together or not at all: one without the other would take
// the hands off the weapon. In vehicles the player keeps the world's FOV (the hands hold parts of
// the vehicle, which is world), detected through the FOV feature's vehicle-camera heartbeat.
//
// Two scopes: every first-person camera (PlayerFov=1), or only while looking through the weapon's
// sight (the default when AdsFov=1: the world then keeps the wide slider FOV, and this keeps
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
// After a reload the weapon is raised back to the sight; the sight FOV returns once it is back
// where it was before the reload (the game's reload state ends 0.5 s to 1.1 s before that, by
// weapon, so no fixed delay fits). Within kBackAtSightMetres of that place counts as back.
constexpr float kBackAtSightMetres = 0.015f;
// That place is the weapon's rest at the sight: only a pose held still (within kStillMetres of
// where it was kStillMs ago, whatever the frame rate) is learnt, so recoil before a reload is not.
constexpr float kStillMetres = 0.01f;
constexpr ULONGLONG kStillMs = 150;
// Without the weapon's position (patterns not found): a fixed delay after the reload state.
constexpr ULONGLONG kAfterReloadMs = 400;
// Never wait longer than this after the reload state for the weapon to come back.
constexpr ULONGLONG kMaxReturnMs = 3000;
// A reload moves the weapon well away from the sight within ~0.2 s (measured: 3-15 cm). If it is
// still at the sight kLeaveSightMs after the state was set, the state is stale.
constexpr float kLeftSightMetres = 0.025f;
constexpr ULONGLONG kLeaveSightMs = 400;
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

// Legacy grcViewport::Perspective: stores fovY, near, aspect and far into the viewport, then
// tan(fovY * 0.5 * pi/180) and the projection. The located site is the function start.
constexpr Signature kLegacyPerspectiveSignatures[] = {
    {"player_perspective_legacy_full",
     "40 53 48 83 EC 30 F3 0F 10 44 24 ?? 0F 29 74 24 ?? 0F 28 F2 F3 0F 11 89 ?? ?? ?? ?? F3 0F 11 99 ?? ?? ?? ?? "
     "48 8B D9 F3 0F 11 B1 ?? ?? ?? ?? F3 0F 11 81 ?? ?? ?? ?? E8",
     0},
    {"player_perspective_legacy_tail",
     "F3 0F 11 89 ?? ?? ?? ?? F3 0F 11 99 ?? ?? ?? ?? 48 8B D9 F3 0F 11 B1 ?? ?? ?? ?? F3 0F 11 81 ?? ?? ?? ?? "
     "E8 ?? ?? ?? ?? F3 0F 10 83",
     -0x14},
};
// Positions inside Legacy Perspective, from its start.
constexpr int kLegacyFovStore = 0x14;     // movss [rcx+fov], xmm1
constexpr int kLegacyNearStore = 0x1C;    // movss [rcx+near], xmm3
constexpr int kLegacyAspectStore = 0x27;  // movss [rcx+aspect], xmm6 (= aspect argument)
constexpr int kLegacyFarStore = 0x2F;     // movss [rcx+far], xmm0 (= far argument)
constexpr int kLegacyFovReload = 0x3C;    // movss xmm0, [rbx+fov]
constexpr int kLegacyHalfLoad = 0x44;     // mulss xmm0, [rip+0.5]
constexpr int kLegacyDegreeLoad = 0x4C;   // mulss xmm0, [rip+pi/180]
constexpr int kLegacyPerspectiveFlag = 0x5C;  // mov byte [rbx+perspective], 1
constexpr float kDegreeInRadians = 0.0174533f;

// The two `call SetCameraMtx` on the private copy, each followed (setCurrentCall bytes later) by
// `call SetCurrent`. Primary signatures include the code that prepares the camera; fallbacks start
// close to the call. Located site: the `call SetCameraMtx`.
struct PrivateCallSignature {
    const char* name;
    const char* bytes;
    int offset;
    int setCurrentCall;
};
constexpr PrivateCallSignature kBodySignatures[] = {
    {"player_body_full",
     "48 8D AC 24 ?? ?? ?? ?? 48 8D 94 24 ?? ?? ?? ?? 4C 8D 84 24 ?? ?? ?? ?? 48 89 E9 E8 ?? ?? ?? ?? "
     "48 89 E9 B2 01 45 31 C0 E8 ?? ?? ?? ?? 49 89 C7",
     27, 13},
    {"player_body_tail", "48 89 E9 E8 ?? ?? ?? ?? 48 89 E9 B2 01 45 31 C0 E8 ?? ?? ?? ?? 49 89 C7", 3, 13},
};
// Legacy: the camera matrix is built on the stack (translation zeroed), then SetCameraMtx(copy, &m).
constexpr PrivateCallSignature kLegacyBodySignatures[] = {
    {"player_body_legacy_full",
     "48 8D 8D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 55 ?? 48 8D 8D ?? ?? ?? ?? 0F 28 85 ?? ?? ?? ?? "
     "0F 28 8D ?? ?? ?? ?? 0F 29 45 ?? 0F 28 85 ?? ?? ?? ?? 0F 29 4D ?? 0F 57 C9 0F 29 45 ?? 0F 29 4D ?? "
     "E8 ?? ?? ?? ?? 48 8D 8D ?? ?? ?? ?? B2 01 E8",
     0x3F, 14},
    {"player_body_legacy_tail", "0F 57 C9 0F 29 45 ?? 0F 29 4D ?? E8 ?? ?? ?? ?? 48 8D 8D ?? ?? ?? ?? B2 01 E8",
     11, 14},
};
#ifdef FPFIX_SIMULATE_SCAN_FAILURE
// Test build (make failtest): the weapon site is missing, so the body must stay original too.
constexpr PrivateCallSignature kWeaponSignatures[] = {
    {"player_weapon_full (corrupted on purpose)", "DE AD BE EF DE AD BE EF DE AD BE EF", 0, 0},
    {"player_weapon_tail (corrupted on purpose)", "DE AD BE EF DE AD BE EF DE AD BE EF", 0, 0},
};
constexpr PrivateCallSignature kLegacyWeaponSignatures[] = {
    {"player_weapon_legacy_full (corrupted on purpose)", "DE AD BE EF DE AD BE EF DE AD BE EF", 0, 0},
    {"player_weapon_legacy_tail (corrupted on purpose)", "DE AD BE EF DE AD BE EF DE AD BE EF", 0, 0},
};
#else
constexpr PrivateCallSignature kWeaponSignatures[] = {
    {"player_weapon_full",
     "48 8D 9C 24 ?? ?? ?? ?? 48 8D 94 24 ?? ?? ?? ?? 4C 8D 84 24 ?? ?? ?? ?? 48 89 D9 E8 ?? ?? ?? ?? "
     "48 89 D9 B2 01 45 31 C0 E8 ?? ?? ?? ?? 48 89 84 24",
     27, 13},
    {"player_weapon_tail", "48 89 D9 E8 ?? ?? ?? ?? 48 89 D9 B2 01 45 31 C0 E8 ?? ?? ?? ?? 48 89 84 24", 3, 13},
};
constexpr PrivateCallSignature kLegacyWeaponSignatures[] = {
    {"player_weapon_legacy_full",
     "48 8D 4D ?? E8 ?? ?? ?? ?? 48 8D 55 ?? 48 8D 4D ?? 0F 28 85 ?? ?? ?? ?? 0F 28 8D ?? ?? ?? ?? "
     "0F 29 45 ?? 0F 28 85 ?? ?? ?? ?? 0F 29 4D ?? 0F 57 C9 0F 29 45 ?? 0F 29 4D ?? "
     "E8 ?? ?? ?? ?? 48 8D 4D ?? 40 8A D7 E8",
     0x39, 12},
    {"player_weapon_legacy_tail", "0F 57 C9 0F 29 45 ?? 0F 29 4D ?? E8 ?? ?? ?? ?? 48 8D 4D ?? 40 8A D7 E8",
     11, 12},
};
#endif

enum class PrivateDraw : int { Body = 0, Weapon = 1 };

struct Projection {
    std::int32_t aspect = 0, fov = 0, secondFov = 0;  // viewport fields (offsets)
    void (*rebuild)(std::uint8_t* viewport) = nullptr;  // Enhanced: rebuilds from the fields
    // Legacy: Perspective(viewport, fovY, aspect, near, far) and the fields it needs.
    void (*perspective)(std::uint8_t* viewport, float, float, float, float) = nullptr;
    std::int32_t nearClip = 0, farClip = 0, perspectiveFlag = 0;
};

Projection g_projection;
CodeBlock g_block;
std::atomic<ULONGLONG> g_lastBodyMs{0};
bool g_everyCamera = false;  // false: only while looking through the sight

// Tells whether a FOV-feature camera is active: its update count changed within kCameraRecentMs.
class RecentCamera {
public:
    explicit RecentCamera(FieldOfView::Heartbeat beat, ULONGLONG windowMs = kCameraRecentMs)
        : m_beat(beat), m_windowMs(windowMs) {}
    bool Active(ULONGLONG now) {
        std::uint32_t updates = FieldOfView::Updates(m_beat);
        if (m_seen.exchange(updates, std::memory_order_relaxed) != updates)
            m_lastChangeMs.store(now, std::memory_order_relaxed);
        return now - m_lastChangeMs.load(std::memory_order_relaxed) < m_windowMs;
    }

private:
    FieldOfView::Heartbeat m_beat;
    ULONGLONG m_windowMs;
    std::atomic<std::uint32_t> m_seen{0};
    std::atomic<ULONGLONG> m_lastChangeMs{0};
};
// In a vehicle the hands hold the wheel or handlebars, which belong to the world.
RecentCamera g_vehicleCamera{FieldOfView::Heartbeat::VehicleCamera};
RecentCamera g_sightCamera{FieldOfView::Heartbeat::SightCamera};
// Reloading while aiming: the weapon is not at the sight, so it keeps the world's FOV (or, with
// PlayerFov, the camera's original FOV) instead of the sight's.
RecentCamera g_sightReloading{FieldOfView::Heartbeat::SightReloading};

// Tells when the weapon is at the sight. A reload (the weapon state, seen while aiming) starts an
// episode in which the sight FOV is off; the episode ends when the weapon is back at the place it
// had at the sight before (learnt while looking through the sight, from a still weapon). The
// state alone is not enough: it ends 0.5 s to 1.1 s before the weapon is back, by weapon, and a
// reload interrupted (e.g. by a dodge roll) can leave it set while the weapon is at the sight.
// So an episode in which the weapon never leaves the sight ends at once.
class WeaponAtSight {
public:
    bool Check(bool sight, bool reloading, ULONGLONG now) {
        float pose[3] = {};
        bool havePose = FieldOfView::SightWeaponOffset(pose);
        const void* weapon = FieldOfView::SightWeapon();
        AcquireSRWLockExclusive(&m_lock);
        if (weapon != m_weapon) {  // another weapon: its sight place is not known yet
            m_weapon = weapon;
            m_haveRef = false;
        }
        if (reloading && !m_wasReloading) {  // a reload starts (or is seen again after aiming again)
            if (m_inReload) Log::Info("player FOV: reload seen again while aiming (%llu ms in)", now - m_startMs);
            m_inReload = true;
            m_left = false;
            m_closest = 1e9f;
            m_startMs = m_nearSinceMs = now;
        }
        m_wasReloading = reloading;
        if (reloading) m_lastReloadMs = now;
        if (havePose) {
            if (!m_haveLast || Distance(pose, m_last) > kStillMetres) {  // m_last: where it was still since
                std::memcpy(m_last, pose, sizeof m_last);
                m_stillSinceMs = now;
            }
            m_haveLast = true;
        }
        if (m_inReload) EndReloadIfBack(havePose, pose, reloading, now);
        bool atSight = sight && !m_inReload;
        if (havePose) {
            if (atSight && now - m_stillSinceMs >= kStillMs) {
                std::memcpy(m_ref, pose, sizeof m_ref);
                m_haveRef = true;
            }
        }
        ReleaseSRWLockExclusive(&m_lock);
        return atSight;
    }

private:
    void EndReloadIfBack(bool havePose, const float* pose, bool reloading, ULONGLONG now) {
        ULONGLONG sinceStart = now - m_startMs, sinceState = now - m_lastReloadMs;
        const char* how = nullptr;
        bool still = havePose && now - m_stillSinceMs >= kStillMs;
        if (!havePose) {
            if (!reloading && sinceState > kAfterReloadMs) how = "after a fixed delay (weapon position unknown)";
        } else if (!m_haveRef) {
            // Sight place not learnt yet (reloaded right after drawing or while moving the view).
            if (!reloading && still) how = "weapon settled (sight place unknown)";
        } else {
            float distance = Distance(pose, m_ref);
            if (distance < m_closest) m_closest = distance;
            if (distance > kLeftSightMetres) m_left = true;
            if (distance >= kBackAtSightMetres) m_nearSinceMs = now;
            // Only after the state ends: some weapons pass by the sight mid-reload (measured: a
            // pistol comes within 1 cm at 0.5 s, then moves away again until 3.4 s).
            if (m_left && !reloading && distance < kBackAtSightMetres) how = "weapon back at the sight";
            // Done, wherever it is (e.g. aiming from another stance after a dodge roll).
            else if (!reloading && still) how = "weapon settled";
            else if (!m_left && sinceStart > kLeaveSightMs) how = "weapon never left the sight";
            // A reload interrupted (dodge roll) can leave the state set with the weapon at the sight.
            else if (reloading && now - m_nearSinceMs >= kLeaveSightMs) how = "stale reload state, weapon at the sight";
        }
        if (!how && !reloading && sinceState > kMaxReturnMs) how = "time limit (weapon not back)";
        if (!how) return;
        m_inReload = false;
        // One line per reload, to diagnose the rare cases.
        Log::Info("player FOV: sight FOV back %llu ms after the reload started: %s (closest %.1f cm)", sinceStart, how,
                  m_closest < 1e8f ? m_closest * 100.0f : -1.0f);
    }

    static float Distance(const float* a, const float* b) {
        return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
    }
    SRWLOCK m_lock = SRWLOCK_INIT;
    const void* m_weapon = nullptr;
    bool m_haveRef = false, m_haveLast = false, m_wasReloading = false, m_inReload = false, m_left = false;
    float m_ref[3] = {}, m_last[3] = {};
    float m_closest = 1e9f;  // nearest the weapon came to its sight place in this reload
    ULONGLONG m_startMs = 0, m_lastReloadMs = 0, m_stillSinceMs = 0, m_nearSinceMs = 0;
};
WeaponAtSight g_weaponAtSight;

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

// First signature with exactly one match whose site validates; isValid(site, signature).
template <typename Sig, size_t N, typename Validate>
const std::uint8_t* FindSite(const Sig (&signatures)[N], Validate isValid) {
    for (const auto& sig : signatures) {
        auto hits = Pattern::FindAll(GameImage::Code(), sig.bytes);
        Log::Info("scan %s: %zu match(es)", sig.name, hits.size());
        if (hits.size() != 1) continue;
        const auto* site = hits[0] + sig.offset;
        if (isValid(site, sig)) return site;
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

bool IsMovssStoreRcx(const std::uint8_t* p, std::uint8_t modrm) {
    return p[0] == 0xF3 && p[1] == 0x0F && p[2] == 0x11 && p[3] == modrm;
}

float RipFloatAt(const std::uint8_t* instruction, int dispAt, int length) {
    float v;
    std::memcpy(&v, Pattern::ResolveRip(instruction, dispAt, length), 4);
    return v;
}

// Every field offset comes from the function's own stores; the constants confirm what it computes.
bool FindLegacyPerspective() {
    const auto* start = FindSite(kLegacyPerspectiveSignatures, [](const std::uint8_t* p, const Signature&) {
        const auto* flag = p + kLegacyPerspectiveFlag;
        Projection found{Disp32(p + kLegacyAspectStore + 4), Disp32(p + kLegacyFovStore + 4), 0, nullptr,
                         reinterpret_cast<void (*)(std::uint8_t*, float, float, float, float)>(
                             const_cast<std::uint8_t*>(p)),
                         Disp32(p + kLegacyNearStore + 4), Disp32(p + kLegacyFarStore + 4), Disp32(flag + 2)};
        bool shape = FunctionStart(p) == p && IsMovssStoreRcx(p + kLegacyFovStore, 0x89) &&
                     IsMovssStoreRcx(p + kLegacyNearStore, 0x99) && IsMovssStoreRcx(p + kLegacyAspectStore, 0xB1) &&
                     IsMovssStoreRcx(p + kLegacyFarStore, 0x81) &&
                     Disp32(p + kLegacyFovReload + 4) == found.fov &&
                     RipFloatAt(p + kLegacyHalfLoad, 4, 8) == 0.5f &&
                     std::fabs(RipFloatAt(p + kLegacyDegreeLoad, 4, 8) - kDegreeInRadians) < 1e-7f &&
                     flag[0] == 0xC6 && flag[1] == 0x83 && flag[6] == 1;
        if (!shape || !IsViewportField(found.fov) || !IsViewportField(found.aspect) ||
            !IsViewportField(found.nearClip) || !IsViewportField(found.farClip) ||
            !IsViewportField(found.perspectiveFlag))
            return false;
        g_projection = found;
        return true;
    });
    if (start)
        Log::Info("player FOV: perspective at exe+0x%zx (fov +0x%x, aspect +0x%x, near +0x%x, far +0x%x, flag +0x%x)",
                  ExeOffset(start), g_projection.fov, g_projection.aspect, g_projection.nearClip,
                  g_projection.farClip, g_projection.perspectiveFlag);
    return start != nullptr;
}

bool FindProjectionBuilder() {
    if (GameImage::IsLegacy()) return FindLegacyPerspective();
    const auto* site = FindSite(kProjectionBuilderSignatures, [](const std::uint8_t* p, const Signature&) {
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
    if (g_projection.perspective && !viewport[g_projection.perspectiveFlag]) return;  // orthographic
    ULONGLONG now = GetTickCount64();
    bool body = PrivateDraw(kind) == PrivateDraw::Body;
    if (body)
        g_lastBodyMs.store(now, std::memory_order_relaxed);
    else if (now - g_lastBodyMs.load(std::memory_order_relaxed) > kBodyRecentMs)
        return;  // a camera-relative object without the first-person body: not the player's weapon
    float fov = ReadFloat(viewport, g_projection.fov);
    float secondFov = g_projection.rebuild ? ReadFloat(viewport, g_projection.secondFov) : fov;
    if (!IsCameraFov(fov) || !IsCameraFov(secondFov)) return;
    // Both checks run every time so each heartbeat's last change stays current.
    bool inVehicle = g_vehicleCamera.Active(now), sight = g_sightCamera.Active(now);
    bool reloading = g_sightReloading.Active(now);
    bool throughSight = g_weaponAtSight.Check(sight, reloading, now);
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
    // While the world's FOV is itself zooming (the sight with UniversalFov=0), the smoothed offset
    // can pass the target and make the weapon bigger than it should be before it settles: never
    // narrower than the player's FOV. Not when leaving the narrow state, which must ease out.
    if (narrow && playerFov < fov) narrowed = std::fmax(narrowed, playerFov);
    if (!IsCameraFov(narrowed)) return;
    if (g_projection.perspective) {
        g_projection.perspective(viewport, narrowed, ReadFloat(viewport, g_projection.aspect),
                                 ReadFloat(viewport, g_projection.nearClip), ReadFloat(viewport, g_projection.farClip));
    } else {
        WriteFloat(viewport, g_projection.fov, narrowed);
        WriteFloat(viewport, g_projection.secondFov, secondFov * (narrowed / fov));
        g_projection.rebuild(viewport);
    }
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
    const std::uint8_t *bodySetCurrent = nullptr, *weaponSetCurrent = nullptr;
    auto privateCall = [](const std::uint8_t*& setCurrent) {
        return [&setCurrent](const std::uint8_t* p, const PrivateCallSignature& sig) {
            auto* target = CallTarget(p);
            setCurrent = CallTarget(p + sig.setCurrentCall);
            return target && FunctionStart(target) == target && setCurrent && FunctionStart(setCurrent) == setCurrent;
        };
    };
    bool legacy = GameImage::IsLegacy();
    auto* body = const_cast<std::uint8_t*>(legacy ? FindSite(kLegacyBodySignatures, privateCall(bodySetCurrent))
                                                  : FindSite(kBodySignatures, privateCall(bodySetCurrent)));
    auto* weapon = const_cast<std::uint8_t*>(legacy ? FindSite(kLegacyWeaponSignatures, privateCall(weaponSetCurrent))
                                                    : FindSite(kWeaponSignatures, privateCall(weaponSetCurrent)));
    // Same SetCameraMtx and same SetCurrent at both sites: they are the two private viewports.
    if (!body || !weapon || CallTarget(body) != CallTarget(weapon) || bodySetCurrent != weaponSetCurrent) {
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
