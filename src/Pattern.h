#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Byte-signature search with wildcards: "48 8B 05 ?? ?? ?? ?? F3 0F 10".
namespace Pattern {
std::vector<const std::uint8_t*> FindAll(std::span<const std::uint8_t> haystack,
                                         std::string_view signature);

// Address of the only match, or nullopt when there are zero or several matches.
std::optional<const std::uint8_t*> FindUnique(std::span<const std::uint8_t> haystack,
                                              std::string_view signature);

// Target of a RIP-relative operand: the int32 displacement at instruction+operandOffset,
// relative to the end of the instruction (instruction+instructionLength).
const std::uint8_t* ResolveRip(const std::uint8_t* instruction, int operandOffset,
                               int instructionLength);
}  // namespace Pattern
