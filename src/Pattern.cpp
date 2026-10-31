#include "Pattern.h"

#include <cstring>

namespace {

struct Token {
    std::uint8_t value;
    bool wildcard;
};

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Returns empty on malformed input so a typo never turns into a wrong match.
std::vector<Token> Parse(std::string_view s) {
    std::vector<Token> out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == ' ') { ++i; continue; }
        if (s[i] == '?') {
            out.push_back({0, true});
            i += (i + 1 < s.size() && s[i + 1] == '?') ? 2 : 1;
            continue;
        }
        int hi = HexDigit(s[i]), lo = i + 1 < s.size() ? HexDigit(s[i + 1]) : -1;
        if (hi < 0 || lo < 0) return {};
        out.push_back({std::uint8_t(hi << 4 | lo), false});
        i += 2;
    }
    return out;
}

}  // namespace

std::vector<const std::uint8_t*> Pattern::FindAll(std::span<const std::uint8_t> haystack,
                                                  std::string_view signature) {
    auto tokens = Parse(signature);
    std::vector<const std::uint8_t*> hits;
    if (tokens.empty() || tokens[0].wildcard || haystack.size() < tokens.size()) return hits;
    const auto* end = haystack.data() + haystack.size() - tokens.size() + 1;
    for (const auto* p = haystack.data(); p < end; ++p) {
        p = static_cast<const std::uint8_t*>(std::memchr(p, tokens[0].value, end - p));
        if (!p) break;
        size_t i = 1;
        while (i < tokens.size() && (tokens[i].wildcard || p[i] == tokens[i].value)) ++i;
        if (i == tokens.size()) hits.push_back(p);
    }
    return hits;
}

std::optional<const std::uint8_t*> Pattern::FindUnique(std::span<const std::uint8_t> haystack,
                                                       std::string_view signature) {
    auto hits = FindAll(haystack, signature);
    if (hits.size() != 1) return std::nullopt;
    return hits[0];
}

const std::uint8_t* Pattern::ResolveRip(const std::uint8_t* instruction, int operandOffset,
                                        int instructionLength) {
    std::int32_t displacement;
    std::memcpy(&displacement, instruction + operandOffset, sizeof displacement);
    return instruction + instructionLength + displacement;
}
