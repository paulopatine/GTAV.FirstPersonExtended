#pragma once
#include <cstdio>

// Plain-text log next to the game EXE (GTAV.FirstPersonExtended.log). Overwritten each launch.
namespace Log {
void Open();
void Write(const char* text);

template <typename... Args>
void Info(const char* fmt, Args... args) {
    char line[1024];
    std::snprintf(line, sizeof line, fmt, args...);
    Write(line);
}
}  // namespace Log
