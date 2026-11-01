#pragma once

// Draws the first-person player (body and the weapon in the hands) with the FOV the same camera
// had in the original game, while the world keeps the wider slider FOV.
namespace PlayerFov {
// Redirects the game's two private first-person viewports (body, weapon) together. With
// `everyCamera` false the original FOV applies only while looking through the weapon's sight.
// Never in vehicles. Returns false (game untouched) when a site is missing or does not validate.
bool Apply(bool everyCamera);
}  // namespace PlayerFov
