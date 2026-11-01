#pragma once
#include <cstdint>
#include <span>
#include <string>

// Facts about the running game's main module (GTA5_Enhanced.exe or, for Legacy, GTA5.exe).
namespace GameImage {
// Path of a file in the game directory (next to the EXE).
std::wstring SiblingPath(const wchar_t* fileName);

// FileVersion resource of the EXE, e.g. "1.0.1158.16".
std::string Version();

// True for GTA V Legacy (GTA5.exe): its code is compiled differently, so every feature looks
// for the Legacy shape of its sites.
bool IsLegacy();

// "Enhanced" or "Legacy", for the log.
const char* EditionName();

// First executable section of the main module (where game code lives).
std::span<const std::uint8_t> Code();

// The EXE's code is encrypted on disk and decrypted in memory during startup.
// Blocks until the code section looks decrypted or the timeout expires.
bool WaitForDecryptedCode(unsigned timeoutSeconds);
}  // namespace GameImage
