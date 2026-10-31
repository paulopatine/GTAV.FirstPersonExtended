// First-person diagonal sprint, as found in 1.0.1158.16 (details: docs/patterns.md).
//
// In first person the game drops a sprint to a run in three places when A or D is held:
//
// 1. Sprint ramp. The move-blend ratio only ramps up to sprint while the "forward" flag is
//    set, and that flag is cleared when a lateral key (MOVE_LEFT_ONLY / MOVE_RIGHT_ONLY) is
//    active:   noLateralInput = !(leftActive | rightActive);   // or r12b,sil ; xor r12b,1
//               forward       = anyMoveActive && noLateralInput;
//    Despite its name `forward` is true for any movement, so the lateral veto is also what
//    keeps A, D and S from sprinting. We lift the veto only while MOVE_UP_ONLY (W) is held:
//               noLateralInput |= upActive;
//
// 2. Sprint input. While sprinting, the movement task replaces the stick input with straight
//    forward (lateral 0, forward 1) unless a game tunable (a bool on a global settings object)
//    is set:      if (!tunable) { lateral = 0; forward = 1; }  // cmp byte [..],0 ; je forceForward
//    The tunable is the game's own "keep the input" switch. We behave as if it were on, but only
//    while the stick points forward (forward > 0): backwards sprinting stays as in the game.
//
// 3. Sprint heading. Even with the input kept, a sprinting character is then turned to the
//    camera's forward heading instead of the stick direction:
//               if (sprinting) heading = cameraHeading;       // test sil,sil ; je skip
//    We skip that override too under the same condition (forward > 0), so the heading follows
//    the stick; every other case keeps the game's behaviour.
//
// All three are needed (with fewer, the character sprints straight ahead or not at all), so
// they are applied together or not at all.
//
// 4. Footsteps (separate feature, applied only if found). The game detects a step when a foot is
//    low and its speed in the world drops below a per-gait limit (~4 m/s for sprinting): a planted
//    foot is at rest. Sprinting diagonally keeps the straight-ahead animation while the body
//    travels along the diagonal, so planted feet slide at about 2*v*sin(angle/2) (~5.4 m/s at
//    45 degrees) and never pass: no step sound and no noise circle on the minimap. While the
//    player sprints with a lateral key (flag set by patch 3) the limit is raised to
//    kSlideSpeedLimit. Other peds, other gaits and straight sprinting keep the game's limit.
#include "DiagonalSprint.h"

#include <windows.h>

#include <array>
#include <cstring>

#include "CodeBlock.h"
#include "GameImage.h"
#include "Log.h"
#include "MemoryPatch.h"
#include "Pattern.h"

namespace {

constexpr int kMoveUpOnlyControl = 0x20;  // INPUT_MOVE_UP_ONLY (W)
// Planted-foot speed limit while sprinting diagonally, in m/s. Covers a slide of 2*7*sin(a/2)
// for stick angles up to ~75 degrees (the sprint speed is about 7 m/s).
constexpr float kSlideSpeedLimit = 9.0f;

// Values shared by the stubs, at the start of the code block (zero-filled by Allocate).
struct SharedData {
    float zero;                        // compared against by ucomiss
    std::uint8_t diagonalSprinting;    // 1 while the player sprints forward + lateral
    std::uint8_t padding[3];
    float slideSpeedLimit;
};
static_assert(sizeof(SharedData) <= CodeBlock::kDataSize);

struct Signature {
    const char* name;
    const char* bytes;
    int patchOffset;
};

// Primary signatures include what follows the patched bytes; fallbacks use what precedes them
// (or a sibling), so they survive the compiler reshuffling one side.
constexpr Signature kLateralVetoSignatures[] = {
    {"diagonal_ramp_full",
     "41 08 F4 41 80 F4 01 49 8D 8D ?? ?? 00 00 4C 8D 05 ?? ?? ?? ?? 0F 28 CE E8 ?? ?? ?? ?? "
     "84 C0 0F 84",
     0},
    {"diagonal_ramp_tail", "E8 ?? ?? ?? ?? 89 C6 41 08 F4 41 80 F4 01", 7},
};
constexpr std::array<std::uint8_t, 7> kLateralVetoBytes = {0x41, 0x08, 0xF4, 0x41, 0x80, 0xF4, 0x01};

constexpr Signature kForcedForwardSignatures[] = {
    {"diagonal_input_full",
     "80 B8 ?? ?? 00 00 00 74 ?? F3 0F 59 F6 41 0F 28 C0 F3 41 0F 59 C0 F3 0F 58 C6 0F 57 C9 "
     "0F 2E C1 0F 85 ?? ?? 00 00 0F 8A ?? ?? 00 00 40 B6 01",
     0},
    {"diagonal_input_tail",
     "74 ?? F3 0F 59 F6 41 0F 28 C0 F3 41 0F 59 C0 F3 0F 58 C6 0F 57 C9 0F 2E C1 0F 85 ?? ?? 00 "
     "00 0F 8A ?? ?? 00 00 40 B6 01 F3 44 0F 10 0D",
     -7},
};

#ifdef FPFIX_SIMULATE_SCAN_FAILURE
// Test build (make failtest): proves a missing site leaves the whole diagonal sprint unapplied.
constexpr Signature kHeadingOverrideSignatures[] = {
    {"diagonal_heading_full (corrupted on purpose)", "DE AD BE EF DE AD BE EF DE AD BE EF", 0},
    {"diagonal_heading_tail (corrupted on purpose)", "DE AD BE EF DE AD BE EF DE AD BE EF", 0},
};
#else
constexpr Signature kHeadingOverrideSignatures[] = {
    {"diagonal_heading_full",
     "40 84 F6 74 ?? 48 89 F9 41 0F 28 CA E8 ?? ?? ?? ?? F3 45 0F 11 96 ?? ?? 00 00 F6 83 "
     "?? ?? 00 00 01",
     0},
    {"diagonal_heading_tail", "F3 41 0F 11 BE ?? ?? 00 00 40 84 F6 74 ?? 48 89 F9 41 0F 28 CA", 9},
};
#endif

// The game's own "is this control down" sequence, as it checks MOVE_LEFT_ONLY / RIGHT_ONLY
// (the second of the three tests it ORs together; the first one is an edge-triggered check):
//   mov rcx,r13 ; mov edx,<control> ; call getControl ; lea rbx,[table] ; mov rcx,rax ;
//   movaps xmm1,xmm6 ; mov r8,rbx ; call isDown
// We call the same functions for MOVE_UP_ONLY.
constexpr Signature kControlCheckSignatures[] = {
    {"control_check_left",
     "4C 89 E9 BA 22 00 00 00 E8 ?? ?? ?? ?? 48 8D 1D ?? ?? ?? ?? 48 89 C1 0F 28 CE 49 89 D8 "
     "E8 ?? ?? ?? ?? 41 B4 01 84 C0",
     0},
    {"control_check_right",
     "4C 89 E9 BA 23 00 00 00 E8 ?? ?? ?? ?? 48 8D 1D ?? ?? ?? ?? 48 89 C1 0F 28 CE 49 89 D8 "
     "E8 ?? ?? ?? ?? 84 C0",
     0},
};

// The footstep detector's planted-foot test, and (same function) its player test.
constexpr Signature kFootstepGateSignatures[] = {
    {"diagonal_steps_full",
     "41 0F 2E 36 0F 86 ?? ?? ?? ?? 44 09 E8 89 84 AE ?? ?? ?? ?? 80 BE ?? ?? ?? ?? ?? 0F 84", 0},
    {"diagonal_steps_tail", "44 09 E8 89 84 AE ?? ?? ?? ?? 80 BE ?? ?? ?? ?? ?? 0F 84", -10},
};
constexpr Signature kPlayerTestSignatures[] = {
    {"diagonal_steps_player",
     "48 8B 8E ?? ?? ?? ?? 41 80 FF 07 75 ?? 83 FF 01 75 ?? 0F B6 81 ?? ?? ?? ?? 24 03 3C 02", 0},
};

struct ControlCheck {
    const std::uint8_t* getControl;
    const std::uint8_t* table;
    const std::uint8_t* isDown;
};

template <size_t N>
std::uint8_t* Find(const Signature (&signatures)[N]) {
    for (const auto& sig : signatures) {
        auto hits = Pattern::FindAll(GameImage::Code(), sig.bytes);
        Log::Info("scan %s: %zu match(es)", sig.name, hits.size());
        if (hits.size() == 1) return const_cast<std::uint8_t*>(hits[0] + sig.patchOffset);
    }
    return nullptr;
}

bool InCode(const std::uint8_t* p) {
    auto code = GameImage::Code();
    return p >= code.data() && p < code.data() + code.size();
}

bool ReadControlCheck(const std::uint8_t* at, ControlCheck& out) {
    auto callTarget = [](const std::uint8_t* call) {
        std::int32_t rel;
        std::memcpy(&rel, call + 1, 4);
        return call + 5 + rel;
    };
    out = {callTarget(at + 8), Pattern::ResolveRip(at + 13, 3, 7), callTarget(at + 29)};
    return InCode(out.getControl) && InCode(out.isDown) && !InCode(out.table);
}

// Replaces `or r12b,sil ; xor r12b,1` with a jump to: the same two instructions, then
// `if (!noLateralInput) noLateralInput = isDown(getControl(MOVE_UP_ONLY))`.
// Registers clobbered by the calls are all reloaded by the code that follows the site.
bool PatchLateralVeto(std::uint8_t* at, const ControlCheck& check, CodeBlock& block) {
    const auto* stub = block.Cursor();
    block.Emit({0x41, 0x08, 0xF4, 0x41, 0x80, 0xF4, 0x01});  // or r12b,sil ; xor r12b,1
    block.Emit({0x45, 0x84, 0xE4});                          // test r12b,r12b
    block.Emit({0x75, 0x22});                                // jnz done (skips the 34 bytes below)
    block.Emit({0x4C, 0x89, 0xE9});                          // mov rcx,r13
    block.Emit({0xBA, kMoveUpOnlyControl, 0, 0, 0});         // mov edx,MOVE_UP_ONLY
    block.EmitCall(check.getControl);
    block.Emit({0x4C, 0x8D, 0x05});                          // lea r8,[table]
    block.EmitRip32(check.table);
    block.Emit({0x48, 0x89, 0xC1});                          // mov rcx,rax
    block.Emit({0x0F, 0x28, 0xCE});                          // movaps xmm1,xmm6
    block.EmitCall(check.isDown);
    block.Emit({0x41, 0x08, 0xC4});                          // or r12b,al
    block.EmitJmp(at + kLateralVetoBytes.size());            // done:
    return CodePatch::JumpToStub(at, kLateralVetoBytes, stub);
}

// The jump distance of the short `je` at `je`, as an absolute target.
const std::uint8_t* JumpTarget(const std::uint8_t* je) { return je + 2 + static_cast<std::int8_t>(je[1]); }

// `cmp byte [reg+disp32],0 ; je forceForward` (9 bytes) becomes: the same compare, then keep the
// input if the tunable is set or the stick points forward, else `jmp forceForward`.
bool PatchForcedForward(std::uint8_t* at, CodeBlock& block) {
    constexpr int kLength = 9;
    const std::uint8_t* keep = at + kLength;
    const auto* stub = block.Cursor();
    block.Emit(std::span<const std::uint8_t>(at, 7));  // cmp byte [reg+disp32],0
    block.Emit({0x0F, 0x85});                          // jne keep
    block.EmitRip32(keep);
    block.Emit({0x44, 0x0F, 0x2E, 0x0D});              // ucomiss xmm9,[zero]  (forward > 0 ?)
    block.EmitRip32(block.Data());
    block.Emit({0x0F, 0x87});                          // ja keep
    block.EmitRip32(keep);
    block.EmitJmp(JumpTarget(at + 7));                 // forceForward
    return CodePatch::JumpToStub(at, std::span<const std::uint8_t>(at, kLength), stub);
}

// `test sil,sil ; je skip` (5 bytes) becomes: skip the override if not sprinting or the stick
// points forward, else run it. Also keeps `diagonalSprinting` up to date: set only when the
// override is skipped because of a forward stick with a lateral component.
bool PatchHeadingOverride(std::uint8_t* at, CodeBlock& block, SharedData& data) {
    constexpr int kLength = 5;
    const std::uint8_t* skip = JumpTarget(at + 3);
    auto clearFlag = [&] {
        block.Emit({0xC6, 0x05});                                  // mov byte [rip+flag],0
        block.EmitRip32(&data.diagonalSprinting - 1);              // imm8 follows the disp32
        block.Emit({0x00});
    };
    const auto* stub = block.Cursor();
    block.Emit({0x40, 0x84, 0xF6});                                // test sil,sil
    auto* notSprinting = block.EmitBranch({0x0F, 0x84});           // jz notSprinting
    block.Emit({0x44, 0x0F, 0x2E, 0x0D});                          // ucomiss xmm9,[zero]
    block.EmitRip32(&data.zero);
    auto* notForward = block.EmitBranch({0x0F, 0x86});             // jbe notForward
    block.Emit({0x44, 0x0F, 0x2E, 0x05});                          // ucomiss xmm8,[zero]
    block.EmitRip32(&data.zero);
    block.Emit({0x0F, 0x95, 0x05});                                // setne byte [rip+flag]
    block.EmitRip32(&data.diagonalSprinting);
    block.EmitJmp(skip);
    block.Bind(notSprinting);
    clearFlag();
    block.EmitJmp(skip);
    block.Bind(notForward);
    clearFlag();
    block.EmitJmp(at + kLength);                                   // run the override
    return CodePatch::JumpToStub(at, std::span<const std::uint8_t>(at, kLength), stub);
}

// Offsets read from the footstep detector's own "is this ped the player" test:
//   mov rcx,[helper+pedOffset] ... movzx eax,byte [rcx+flagsOffset] ; and al,3 ; cmp al,2
struct PlayerTest {
    std::int32_t pedOffset;
    std::int32_t flagsOffset;
};

bool ReadPlayerTest(const std::uint8_t* at, PlayerTest& out) {
    std::memcpy(&out.pedOffset, at + 3, 4);
    std::memcpy(&out.flagsOffset, at + 21, 4);
    return out.pedOffset > 0 && out.flagsOffset > 0;
}

// `ucomiss xmm6,[r14] ; jbe notAStep` (10 bytes, the detector's planted-foot test) becomes the
// same test, then, only for the player while diagonalSprinting, a second one against
// slideSpeedLimit. Clobbers rcx and xmm0, both dead here.
bool PatchFootsteps(std::uint8_t* at, const PlayerTest& player, CodeBlock& block, SharedData& data) {
    constexpr int kLength = 10;
    std::int32_t rel;
    std::memcpy(&rel, at + 6, 4);
    const std::uint8_t* notAStep = at + kLength + rel;
    const auto* stub = block.Cursor();
    block.Emit({0x41, 0x0F, 0x2E, 0x36});                          // ucomiss xmm6,[r14]
    auto* isAStep = block.EmitBranch({0x0F, 0x87});                // ja isAStep
    block.Emit({0x80, 0x3D});                                      // cmp byte [rip+flag],0
    block.EmitRip32(&data.diagonalSprinting - 1);
    block.Emit({0x00});
    block.Emit({0x0F, 0x84});                                      // je notAStep
    block.EmitRip32(notAStep);
    block.Emit({0xF3, 0x0F, 0x10, 0x05});                          // movss xmm0,[rip+limit]
    block.EmitRip32(&data.slideSpeedLimit);
    block.Emit({0x41, 0x0F, 0x2E, 0x06});                          // ucomiss xmm0,[r14]
    block.Emit({0x0F, 0x86});                                      // jbe notAStep
    block.EmitRip32(notAStep);
    block.Emit({0x48, 0x8B, 0x8E});                                // mov rcx,[rsi+pedOffset]
    block.Emit(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&player.pedOffset), 4));
    block.Emit({0x0F, 0xB6, 0x89});                                // movzx ecx,byte [rcx+flagsOffset]
    block.Emit(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&player.flagsOffset), 4));
    block.Emit({0x83, 0xE1, 0x03, 0x83, 0xF9, 0x02});              // and ecx,3 ; cmp ecx,2
    block.Emit({0x0F, 0x85});                                      // jne notAStep
    block.EmitRip32(notAStep);
    block.Bind(isAStep);
    block.EmitJmp(at + kLength);
    return CodePatch::JumpToStub(at, std::span<const std::uint8_t>(at, kLength), stub);
}

size_t ExeOffset(const std::uint8_t* p) {
    return size_t(p - reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr)));
}

// Footsteps while sprinting diagonally; independent of the sprint itself, so a failure here
// only leaves that sprint silent, as in the game.
void ApplyFootsteps(CodeBlock& block, SharedData& data) {
    auto* gate = Find(kFootstepGateSignatures);
    auto* playerTest = Find(kPlayerTestSignatures);
    PlayerTest player{};
    if (!gate || !playerTest || !ReadPlayerTest(playerTest, player) || gate[0] != 0x41 ||
        gate[1] != 0x0F || gate[2] != 0x2E || gate[4] != 0x0F || gate[5] != 0x86) {
        Log::Info("diagonal footsteps: site not found or not validated; sprint stays silent");
        return;
    }
    bool ok = PatchFootsteps(gate, player, block, data);
    Log::Info("diagonal footsteps: %s at exe+0x%zx", ok ? "patched" : "NOT patched", ExeOffset(gate));
}

}  // namespace

bool DiagonalSprint::Apply() {
    auto* veto = Find(kLateralVetoSignatures);
    auto* input = Find(kForcedForwardSignatures);
    auto* heading = Find(kHeadingOverrideSignatures);
    auto* controlCheck = Find(kControlCheckSignatures);
    ControlCheck check{};
    bool valid = veto && input && heading && controlCheck && ReadControlCheck(controlCheck, check) &&
                 std::memcmp(veto, kLateralVetoBytes.data(), kLateralVetoBytes.size()) == 0 &&
                 input[0] == 0x80 && (input[1] & 0xF8) == 0xB8 && input[7] == 0x74 &&  // cmp byte [reg+disp32],imm8 ; je
                 heading[0] == 0x40 && heading[1] == 0x84 && heading[2] == 0xF6 && heading[3] == 0x74;
    if (!valid) {
        Log::Info("diagonal sprint: a site was not found or did not validate; stays original");
        return false;
    }
    static CodeBlock block;  // the stubs live as long as the game
    if (!block.Allocate(GameImage::Code().data())) {
        Log::Info("diagonal sprint: no memory near the game code; stays original");
        return false;
    }
    auto& data = *static_cast<SharedData*>(block.Data());
    data.slideSpeedLimit = kSlideSpeedLimit;
    bool rampOk = PatchLateralVeto(veto, check, block);
    bool inputOk = PatchForcedForward(input, block);
    bool headingOk = PatchHeadingOverride(heading, block, data);
    Log::Info("diagonal sprint: ramp %s at exe+0x%zx, input %s at exe+0x%zx, heading %s at exe+0x%zx",
              rampOk ? "patched" : "NOT patched", ExeOffset(veto), inputOk ? "patched" : "NOT patched",
              ExeOffset(input), headingOk ? "patched" : "NOT patched", ExeOffset(heading));
    if (rampOk && inputOk && headingOk) ApplyFootsteps(block, data);
    return rampOk && inputOk && headingOk;
}
