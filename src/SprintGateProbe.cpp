// Diagnostic build only. The first-person sprint gate ends in `ucomiss xmm6, xmm7; jae deny`
// (xmm6 = value read from the player's controls, xmm7 = threshold). This replaces that pair
// with a stub that records both values and counts executions and denials, then behaves
// exactly like the original.
#ifdef FPFIX_SPRINT_GATE_PROBE
#include "SprintGateProbe.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "CodeBlock.h"
#include "GameImage.h"
#include "Log.h"
#include "Pattern.h"

namespace {

struct Counters {
    volatile std::uint32_t executed;
    volatile std::uint32_t denied;
    volatile float lastValue;
    volatile float lastThreshold;
};

// Last value seen per move-blend-ratio request type (the game asks for several each frame).
constexpr int kTypes = 4;
struct Ratios {
    volatile float raw[kTypes];    // result of the input -> ratio computation
    volatile float final[kTypes];  // value the caller receives
};

// What the first-person movement task holds right before it stores its desired heading.
struct Steering {
    volatile float heading;        // xmm7: heading being stored
    volatile float lateral;        // xmm8: stick left/right
    volatile float forward;        // xmm9: stick forward/back (positive = forward)
    volatile float cameraHeading;  // xmm10
    volatile std::uint32_t sprintOverride;  // sil
};

CodeBlock g_block;
Counters* g_counters = nullptr;
Ratios* g_ratios = nullptr;
Steering* g_steering = nullptr;

const char* ComboName(int combo) {
    static char name[16];
    std::snprintf(name, sizeof name, "%s%s%s%s%s", (combo & 1) ? "W" : "", (combo & 2) ? "A" : "",
                  (combo & 4) ? "S" : "", (combo & 8) ? "D" : "", (combo & 16) ? "+Shift" : "");
    return name;
}

int HeldCombo() {
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    return (down('W') << 0) | (down('A') << 1) | (down('S') << 2) | (down('D') << 3) |
           (down(VK_SHIFT) << 4);
}

DWORD WINAPI Report(LPVOID) {
    std::uint32_t lastExecuted = 0, lastDenied = 0;
    for (;;) {
        Sleep(500);
        std::uint32_t executed = g_counters->executed, denied = g_counters->denied;
        if (executed == lastExecuted) continue;
        Log::Info("gate: [%s] executed=%u denied=%u value=%.3f threshold=%.3f",
                  ComboName(HeldCombo()), executed - lastExecuted, denied - lastDenied,
                  g_counters->lastValue, g_counters->lastThreshold);
        lastExecuted = executed;
        lastDenied = denied;
        Log::Info("steer: [%s] sil=%u lateral=%.2f forward=%.2f camera=%.2f heading=%.2f", ComboName(HeldCombo()),
                  g_steering->sprintOverride & 0xFF, g_steering->lateral, g_steering->forward,
                  g_steering->cameraHeading, g_steering->heading);
        Log::Info("ratio: [%s] raw %.2f %.2f %.2f %.2f final %.2f %.2f %.2f %.2f", ComboName(HeldCombo()),
                  g_ratios->raw[0], g_ratios->raw[1], g_ratios->raw[2], g_ratios->raw[3],
                  g_ratios->final[0], g_ratios->final[1], g_ratios->final[2], g_ratios->final[3]);
    }
}

// Redirects `at` (length bytes, position independent) to a stub that stores xmmN into
// slots[request type & 3] -- the type is in r12d at both sites -- then runs the original bytes.
bool InstallRatioTap(std::uint8_t* at, int length, volatile float* slots, std::uint8_t xmmModrm) {
    const auto* stub = g_block.Cursor();
    g_block.Emit({0x9C, 0x50, 0x51});              // pushfq; push rax; push rcx
    g_block.Emit({0x44, 0x89, 0xE0});              // mov eax, r12d
    g_block.Emit({0x83, 0xE0, 0x03});              // and eax, 3
    g_block.Emit({0x48, 0x8D, 0x0D});              // lea rcx, [rip+slots]
    g_block.EmitRip32(const_cast<float*>(slots));
    g_block.Emit({0xF3, 0x0F, 0x11, xmmModrm, 0x81});  // movss [rcx+rax*4], xmmN
    g_block.Emit({0x59, 0x58, 0x9D});              // pop rcx; pop rax; popfq
    g_block.Emit(std::span<const std::uint8_t>(at, length));
    g_block.EmitJmp(at + length);
    return CodePatch::JumpToStub(at, std::span<const std::uint8_t>(at, length), stub);
}

}  // namespace

void SprintGateProbe::Install() {
    auto code = GameImage::Code();
    auto hit = Pattern::FindUnique(code, "0F 2E F7 73 ?? F6 86 ?? ?? 00 00 80 75 ??");
    if (!hit) { Log::Info("gate probe: signature not found"); return; }
    auto* at = const_cast<std::uint8_t*>(*hit);
    const std::uint8_t* deny = at + 5 + static_cast<std::int8_t>(at[4]);
    if (!g_block.Allocate(code.data())) { Log::Info("gate probe: no memory"); return; }
    g_counters = static_cast<Counters*>(g_block.Reserve(sizeof(Counters)));

    const auto* stub = g_block.Cursor();
    g_block.Emit({0xF0, 0xFF, 0x05});  // lock inc dword [rip+executed]
    g_block.EmitRip32(const_cast<std::uint32_t*>(&g_counters->executed));
    g_block.Emit({0xF3, 0x0F, 0x11, 0x35});  // movss [rip+lastValue], xmm6
    g_block.EmitRip32(const_cast<float*>(&g_counters->lastValue));
    g_block.Emit({0xF3, 0x0F, 0x11, 0x3D});  // movss [rip+lastThreshold], xmm7
    g_block.EmitRip32(const_cast<float*>(&g_counters->lastThreshold));
    g_block.Emit({0x0F, 0x2E, 0xF7});        // ucomiss xmm6, xmm7 (original)
    g_block.Emit({0x73, 0x05});              // jae denied (skips the 5-byte jmp below)
    g_block.EmitJmp(at + 5);                 // not denied: continue after the original pair
    g_block.Emit({0xF0, 0xFF, 0x05});        // denied: lock inc dword [rip+denied]
    g_block.EmitRip32(const_cast<std::uint32_t*>(&g_counters->denied));
    g_block.EmitJmp(deny);

    bool ok = CodePatch::JumpToStub(at, std::span<const std::uint8_t>(at, 5), stub);
    Log::Info("gate probe: %s", ok ? "installed" : "patch refused");

    g_ratios = static_cast<Ratios*>(g_block.Reserve(sizeof(Ratios)));
    // After the input -> ratio call returns: `movaps xmm6, xmm0; mov esi, r12d`.
    auto raw = Pattern::FindUnique(code, "E8 ?? ?? ?? ?? 0F 28 F0 44 89 E6 41 83 FC 01 0F 85");
    // Function epilogue returning the ratio: `movaps xmm0, xmm6; ...; ret`.
    auto fin = Pattern::FindUnique(code, "0F 28 C6 0F 28 74 24 40 0F 28 7C 24 50 44 0F 28 44 24 60 "
                                         "48 83 C4 78 5B 5D 5F 5E 41 5C 41 5D 41 5E 41 5F C3");
    bool rawOk = raw && InstallRatioTap(const_cast<std::uint8_t*>(*raw) + 5, 6, g_ratios->raw, 0x04);
    bool finOk = fin && InstallRatioTap(const_cast<std::uint8_t*>(*fin), 8, g_ratios->final, 0x34);
    Log::Info("ratio probe: raw tap %s, final tap %s", rawOk ? "ok" : "FAILED", finOk ? "ok" : "FAILED");

    // Just before the movement task stores its desired heading: `movss [r14+disp32],xmm7`
    // followed by `test sil,sil`.
    g_steering = static_cast<Steering*>(g_block.Reserve(sizeof(Steering)));
    auto steer = Pattern::FindUnique(code, "F3 41 0F 11 BE ?? ?? 00 00 40 84 F6");
    if (steer) {
        auto* sat = const_cast<std::uint8_t*>(*steer);
        const auto* st = g_block.Cursor();
        g_block.Emit({0x40, 0x88, 0x35});              // mov [rip+sprintOverride], sil
        g_block.EmitRip32(const_cast<std::uint32_t*>(&g_steering->sprintOverride));
        g_block.Emit({0xF3, 0x44, 0x0F, 0x11, 0x05});  // movss [rip+lateral], xmm8
        g_block.EmitRip32(const_cast<float*>(&g_steering->lateral));
        g_block.Emit({0xF3, 0x44, 0x0F, 0x11, 0x0D});  // movss [rip+forward], xmm9
        g_block.EmitRip32(const_cast<float*>(&g_steering->forward));
        g_block.Emit({0xF3, 0x44, 0x0F, 0x11, 0x15});  // movss [rip+cameraHeading], xmm10
        g_block.EmitRip32(const_cast<float*>(&g_steering->cameraHeading));
        g_block.Emit({0xF3, 0x0F, 0x11, 0x3D});        // movss [rip+heading], xmm7
        g_block.EmitRip32(const_cast<float*>(&g_steering->heading));
        g_block.Emit(std::span<const std::uint8_t>(sat, 9));
        g_block.EmitJmp(sat + 9);
        bool steerOk = CodePatch::JumpToStub(sat, std::span<const std::uint8_t>(sat, 9), st);
        Log::Info("steer probe: %s", steerOk ? "ok" : "FAILED");
    } else {
        Log::Info("steer probe: signature not found");
    }
    if (ok)
        if (HANDLE t = CreateThread(nullptr, 0, Report, nullptr, 0, nullptr)) CloseHandle(t);
}
#endif
