#pragma once
#include <cstdint>
#include <initializer_list>
#include <span>

// One executable page placed within RIP reach (+-2 GB) of the game code, holding the
// mod's shared values followed by small hand-assembled stubs that patched code jumps to.
class CodeBlock {
public:
    // Returns false if no memory could be reserved near `nearCode`.
    bool Allocate(const std::uint8_t* nearCode, std::size_t size = 4096);

    // Fixed-size area at the start of the page for values the stubs read or write.
    void* Data() const { return m_base; }

    const std::uint8_t* Cursor() const { return m_cursor; }
    void Emit(std::initializer_list<std::uint8_t> bytes);
    void Emit(std::span<const std::uint8_t> bytes);
    // disp32 for an instruction whose RIP-relative operand is its last 4 bytes.
    void EmitRip32(const void* target);
    void EmitJmp(const void* target);   // E9 rel32
    void EmitCall(const void* target);  // E8 rel32
    // Forward branch: emits `opcode` + a rel32 to fill in later; Bind() points it at the cursor.
    std::uint8_t* EmitBranch(std::initializer_list<std::uint8_t> opcode);
    void Bind(std::uint8_t* rel32);
    // Zero-filled scratch space carved from the block (e.g. counters).
    void* Reserve(std::size_t size);

    static constexpr std::size_t kDataSize = 64;

private:
    std::uint8_t* m_base = nullptr;
    std::uint8_t* m_cursor = nullptr;
};

namespace CodePatch {
// Overwrites `length` bytes at `at` (which must equal `expected`) with `jmp stub` + NOPs.
bool JumpToStub(std::uint8_t* at, std::span<const std::uint8_t> expected, const std::uint8_t* stub);
}  // namespace CodePatch
