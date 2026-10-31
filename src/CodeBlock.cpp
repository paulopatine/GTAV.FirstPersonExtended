#include "CodeBlock.h"

#include <windows.h>

#include <cstring>
#include <vector>

#include "MemoryPatch.h"

namespace {
constexpr std::size_t kJmpLength = 5;
constexpr std::uintptr_t kRipReach = 0x7FFF0000;
}  // namespace

bool CodeBlock::Allocate(const std::uint8_t* nearCode, std::size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const auto granularity = std::uintptr_t(si.dwAllocationGranularity);
    const auto origin = std::uintptr_t(nearCode) & ~(granularity - 1);
    for (std::uintptr_t delta = granularity; delta < kRipReach - size; delta += granularity) {
        for (auto candidate : {origin - delta, origin + delta}) {
            void* p = VirtualAlloc(reinterpret_cast<void*>(candidate), size,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (!p) continue;
            m_base = static_cast<std::uint8_t*>(p);
            m_cursor = m_base + kDataSize;
            std::memset(m_base, 0, kDataSize);
            std::memset(m_cursor, 0xCC, size - kDataSize);  // int3 past the stubs
            return true;
        }
    }
    return false;
}

void CodeBlock::Emit(std::initializer_list<std::uint8_t> bytes) {
    for (auto b : bytes) *m_cursor++ = b;
}

void CodeBlock::Emit(std::span<const std::uint8_t> bytes) {
    std::memcpy(m_cursor, bytes.data(), bytes.size());
    m_cursor += bytes.size();
}

void CodeBlock::EmitRip32(const void* target) {
    auto disp = std::int32_t(static_cast<const std::uint8_t*>(target) - (m_cursor + 4));
    std::memcpy(m_cursor, &disp, 4);
    m_cursor += 4;
}

void* CodeBlock::Reserve(std::size_t size) {
    void* p = m_cursor;
    std::memset(p, 0, size);
    m_cursor += size;
    return p;
}

void CodeBlock::EmitJmp(const void* target) {
    Emit({0xE9});
    EmitRip32(target);
}

void CodeBlock::EmitCall(const void* target) {
    Emit({0xE8});
    EmitRip32(target);
}

std::uint8_t* CodeBlock::EmitBranch(std::initializer_list<std::uint8_t> opcode) {
    Emit(opcode);
    std::uint8_t* rel32 = m_cursor;
    m_cursor += 4;
    return rel32;
}

void CodeBlock::Bind(std::uint8_t* rel32) {
    auto disp = std::int32_t(m_cursor - (rel32 + 4));
    std::memcpy(rel32, &disp, 4);
}

bool CodePatch::JumpToStub(std::uint8_t* at, std::span<const std::uint8_t> expected,
                           const std::uint8_t* stub) {
    if (expected.size() < kJmpLength) return false;
    std::vector<std::uint8_t> jump(expected.size(), 0x90);
    jump[0] = 0xE9;
    auto disp = std::int32_t(stub - (at + kJmpLength));
    std::memcpy(&jump[1], &disp, 4);
    return MemoryPatch::Replace(at, expected, jump);
}
