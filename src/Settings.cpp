#include "Settings.h"

#include <windows.h>

#include "GameImage.h"
#include "Log.h"

namespace {

constexpr int kMinFov = 60;
constexpr int kMaxFov = 140;
constexpr int kMissing = -1;  // GetPrivateProfileInt returns this when the key is absent

// Reads an integer; returns `fallback` (and logs) if the key is present but not usable.
template <typename IsValid>
int ReadInt(const wchar_t* path, const wchar_t* key, int fallback, IsValid isValid, const char* rule) {
    int value = int(GetPrivateProfileIntW(L"FirstPersonExtended", key, kMissing, path));
    if (value == kMissing) return fallback;
    if (isValid(value)) return value;
    Log::Info("ini: %ls=%d is not valid (%s); using %d", key, value, rule, fallback);
    return fallback;
}

}  // namespace

Settings::Values Settings::Load() {
    Values values;
    const auto path = GameImage::SiblingPath(L"GTAV.FirstPersonExtended.ini");
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log::Info("ini: GTAV.FirstPersonExtended.ini not found; using defaults");
    } else {
        values.maxHorizontalFovDegrees = ReadInt(
            path.c_str(), L"MaxFov", values.maxHorizontalFovDegrees,
            [](int v) { return v >= kMinFov && v <= kMaxFov; }, "whole degrees from 60 to 140");
        values.diagonalSprint = ReadInt(
            path.c_str(), L"DiagonalSprint", values.diagonalSprint,
            [](int v) { return v == 0 || v == 1; }, "0 or 1") != 0;
    }
    Log::Info("ini: MaxFov=%d DiagonalSprint=%d", values.maxHorizontalFovDegrees,
              values.diagonalSprint ? 1 : 0);
    return values;
}
