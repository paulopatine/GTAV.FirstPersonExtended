#pragma once
#include <cstdint>
#include <span>

namespace MemoryPatch {
// Overwrites code/data only if the current bytes equal `expected`; never patches blindly.
// Returns false (and leaves memory untouched) on mismatch or protection failure.
bool Replace(void* address, std::span<const std::uint8_t> expected,
             std::span<const std::uint8_t> replacement);
}  // namespace MemoryPatch
