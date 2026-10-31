#include "Log.h"

#include <windows.h>

#include <mutex>

#include "GameImage.h"

namespace {
FILE* g_file = nullptr;
std::mutex g_mutex;
}  // namespace

void Log::Open() {
    std::lock_guard lock(g_mutex);
    if (!g_file) g_file = _wfopen(GameImage::SiblingPath(L"GTAV.FirstPersonExtended.log").c_str(), L"w");
}

void Log::Write(const char* text) {
    std::lock_guard lock(g_mutex);
    if (!g_file) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(g_file, "[%02d:%02d:%02d.%03d] %s\n", t.wHour, t.wMinute, t.wSecond,
                 t.wMilliseconds, text);
    std::fflush(g_file);
}
