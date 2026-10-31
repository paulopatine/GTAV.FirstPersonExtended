#pragma once

// Options read from GTAV.FirstPersonExtended.ini next to the game EXE. A missing file, a missing key or
// an invalid value gives the default (and an invalid value is logged).
namespace Settings {
struct Values {
    int maxHorizontalFovDegrees = 120;  // top of the native first-person FOV slider
    bool diagonalSprint = true;         // keep sprinting on W+A / W+D
};

Values Load();
}  // namespace Settings
