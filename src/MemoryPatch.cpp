#include "MemoryPatch.h"

#include <windows.h>

#include <cstring>

bool MemoryPatch::Replace(void* address, std::span<const std::uint8_t> expected,
                          std::span<const std::uint8_t> replacement) {
    if (expected.size() != replacement.size() || expected.empty()) return false;
    if (std::memcmp(address, expected.data(), expected.size()) != 0) return false;
    DWORD old = 0;
    if (!VirtualProtect(address, replacement.size(), PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(address, replacement.data(), replacement.size());
    VirtualProtect(address, replacement.size(), old, &old);
    FlushInstructionCache(GetCurrentProcess(), address, replacement.size());
    return true;
}
