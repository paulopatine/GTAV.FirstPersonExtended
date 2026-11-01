#pragma once

// Extends the native first-person FOV slider (Settings > Camera) up to a horizontal maximum.
namespace FieldOfView {
// Patches the game's slider-to-FOV formula so the slider tops out at `maxHorizontalDegrees`.
// With `universal`, the aim, phone and vehicle cameras use the slider's FOV instead of scaling
// their own. Returns false (game untouched) on any mismatch.
bool Apply(float maxHorizontalDegrees, bool universal);

// Keeps the new maximum in sync with the window aspect ratio and logs slider changes.
// Call only after Apply() succeeded. Never returns.
[[noreturn]] void Track();
// Offset of the FOV field in a camera's frame (found by Apply), or -1. Used by the probe.
int FrameFovOffset();
}  // namespace FieldOfView
