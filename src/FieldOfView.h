#pragma once
#include <cstdint>

// Extends the native first-person FOV slider (Settings > Camera) up to a horizontal maximum.
namespace FieldOfView {
// Patches the game's slider-to-FOV formula so the slider tops out at `maxHorizontalDegrees`.
// With `universal`, the aim, phone and vehicle cameras use the slider's FOV instead of scaling
// their own. Returns false (game untouched) on any mismatch.
bool Apply(float maxHorizontalDegrees, bool universal);

// Keeps the new maximum in sync with the window aspect ratio and logs slider changes.
// Call only after Apply() succeeded. Never returns.
[[noreturn]] void Track();
// How much the slider widens the first-person view at its current position (extended FOV /
// original FOV, both vertical degrees); 1 until known. Safe to read from any thread.
float SliderWidening();
// Cameras whose patched stub counts its own updates, so other features can tell when that camera
// is active (the count keeps changing). VehicleCamera: first person in a vehicle. SightCamera: aim
// down sights while looking through the weapon's sight. SightReloading: the same camera while the
// player's weapon reloads (counted instead of SightCamera).
enum class Heartbeat { VehicleCamera = 0, SightCamera = 1, SightReloading = 2 };
bool Tracked(Heartbeat beat);       // false when that camera's site was not patched
std::uint32_t Updates(Heartbeat beat);
// The FOV (vertical degrees) the game itself set for the sight last time it was looked through,
// before the mod changed it; 0 until then.
float SightGameFov();
// Where the weapon held at the sight is relative to the aim camera, in metres along the camera's
// right, forward and up; false when unknown (patterns not found, nothing held). Any thread.
bool SightWeaponOffset(float out[3]);
// The weapon object held at the sight (changes with the weapon); null when none. Only compared.
const void* SightWeapon();
// Offset of the FOV field in a camera's frame (found by Apply), or -1. Used by the probe.
int FrameFovOffset();
}  // namespace FieldOfView
