// Diagnostic build only (make footstepprobe). Not part of the released mod.
// Counts how often each function of the footstep-audio call chain is entered, per held key
// combination. Comparing W+Shift (footsteps heard) with W+A+Shift (silent) shows where the
// chain stops. The table comes from tools/re/sigmake.py: signature of a function start and the
// length (whole, position-independent instructions) the jump to the counting stub replaces.
#ifdef FPFIX_FOOTSTEP_PROBE
#include "FootstepProbe.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <tuple>
#include <vector>

#include "CodeBlock.h"
#include "GameImage.h"
#include "Log.h"
#include "Pattern.h"

namespace {

struct Function {
    const char* name;
    const char* signature;
    int patchLength;
};

constexpr Function kFunctions[] = {
    {"fn_9824f0", "41 57 41 56 56 57 55 53 48 81 EC ?? ?? ?? ?? 0F 29 B4 24 ?? ?? ?? ?? 44 89 C3 89 D7", 5},
    {"fn_d55340", "41 57 41 56 41 55 41 54 56 57 55 53 48 83 EC ?? 0F 29 74 24 ?? 83 3D ?? ?? ?? ?? ?? 0F 84 ?? ?? ?? ??", 6},
    {"fn_d54af0", "48 8B 81 ?? ?? ?? ?? 48 8B 88 ?? ?? ?? ?? 48 85 C9 74 ?? 41 89 D3 41 D1 CB", 7},
    {"fn_98ab80", "56 57 48 83 EC ?? 80 B9 ?? ?? ?? ?? ?? 74 ?? 48 89 CF 48 63 81 ?? ?? ?? ?? 48 83 F8 ?? 74 ?? 89 D6 8D 48 ?? 89 8F ?? ?? ?? ?? 89 94 87 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 48 8B BF ?? ?? ?? ?? 48 8D 4C 24 ?? 89 F2 E8 ?? ?? ?? ?? 48 85 FF 74 ?? 0F B6 47 ?? 83 F8 ?? 77 ?? B9 ?? ?? ?? ?? 0F A3 C1 73 ?? 48 89 7C 24 ?? 48 C7 44 24 ?? ?? ?? ?? ?? 48 8D 4C 24 ?? 48 8D 54 24 ?? 45 31 C0 45 31 C9 E8 ?? ?? ?? ?? 90 48 83 C4 ?? 5F 5E C3 56", 6},
    {"fn_d563b0", "56 48 81 EC ?? ?? ?? ?? 44 0F 29 8C 24 ?? ?? ?? ?? 44 0F 29 84 24 ?? ?? ?? ?? 0F 29 BC 24 ?? ?? ?? ?? 0F 29 B4 24 ?? ?? ?? ?? 48 89 CE 0F 57 C0", 8},
    {"fn_97c730", "56 48 83 EC ?? 48 89 CE 44 8B 0D ?? ?? ?? ?? 48 8B 81 ?? ?? ?? ?? 44 8B 80 ?? ?? ?? ??", 5},
    {"fn_97d390", "56 57 53 48 83 EC ?? 0F 29 74 24 ?? 48 89 CE 48 8B 89 ?? ?? ?? ?? 48 8B 01", 7},
    {"fn_981e30", "41 56 56 57 53 48 83 EC ?? 0F 29 7C 24 ?? 0F 29 74 24 ?? 83 B9 ?? ?? ?? ?? ?? 0F 8E ?? ?? ?? ?? 48 89 CE", 5},
    {"fn_981ff0", "41 57 41 56 56 57 55 53 48 81 EC ?? ?? ?? ?? 44 0F 29 84 24 ?? ?? ?? ?? 0F 29 7C 24 ?? 0F 29 74 24 ?? 48 89 CE 48 8B 89 ?? ?? ?? ??", 5},
    {"fn_9864b0", "41 57 41 56 41 54 56 57 55 53 48 81 EC ?? ?? ?? ?? 44 0F 29 9C 24 ?? ?? ?? ?? 44 0F 29 94 24 ?? ?? ?? ?? 44 0F 29 8C 24 ?? ?? ?? ?? 44 0F 29 84 24 ?? ?? ?? ?? 0F 29 BC 24 ?? ?? ?? ?? 0F 29 B4 24 ?? ?? ?? ?? 45 89 CE 4D 89 C7", 6},
    {"fn_988390", "41 57 41 56 56 57 55 53 48 83 EC ?? 0F 29 74 24 ?? 4D 89 CE 4D 89 C7 89 D5", 5},
    {"fn_984e20", "41 56 56 57 53 48 81 EC ?? ?? ?? ?? 83 3D ?? ?? ?? ?? ?? 0F 84 ?? ?? ?? ??", 5},
    {"fn_987910", "41 57 41 56 56 57 55 53 48 81 EC ?? ?? ?? ?? 44 0F 29 BC 24 ?? ?? ?? ?? 44 0F 29 B4 24 ?? ?? ?? ?? 44 0F 29 AC 24 ?? ?? ?? ?? 44 0F 29 A4 24 ?? ?? ?? ?? 44 0F 29 9C 24 ?? ?? ?? ?? 44 0F 29 94 24 ?? ?? ?? ?? 44 0F 29 8C 24 ?? ?? ?? ?? 44 0F 29 84 24 ?? ?? ?? ?? 0F 29 BC 24 ?? ?? ?? ?? 0F 29 B4 24 ?? ?? ?? ?? 45 89 CE", 5},
    {"fn_9889b0", "41 57 41 56 56 57 53 48 83 EC ?? 48 8B 99 ?? ?? ?? ?? 48 85 DB 0F 84 ?? ?? ?? ?? 48 89 CE", 5},
    {"fn_96cde0", "56 48 83 EC ?? 48 89 D6 48 8D 8A ?? ?? ?? ?? BA ?? ?? ?? ?? 45 31 C0 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 48 8D 4C 24 ?? B2 ?? E8 ?? ?? ?? ?? 48 85 F6 74 ?? 0F B6 46 ?? 83 F8 ?? 77 ?? B9 ?? ?? ?? ?? 0F A3 C1 73 ?? 48 89 74 24 ?? 48 C7 44 24 ?? ?? ?? ?? ?? 48 8D 4C 24 ?? 48 8D 54 24 ?? 45 31 C0 45 31 C9 E8 ?? ?? ?? ?? 90 48 83 C4 ?? 5E C3 C9", 5},
};
constexpr size_t kCount = sizeof kFunctions / sizeof kFunctions[0];

// Footstep update of one ped: the movement type it passes down (r8d).
struct StepUpdate {
    volatile std::uint32_t calls;
    volatile std::uint32_t type;
};

// How often the footstep queue sees the ped in the state that routes footsteps to the
// alternative handler (ped flags & 0x1fc000 == 0x70000).
struct RouteCount {
    volatile std::uint32_t total;
    volatile std::uint32_t alternative;
};

// The player's footstep classification: the speed handed to the classifier and the footstep
// type it returns (0x14/0x15 select the timed sprint footsteps), plus the "timed path" flag.
struct Classification {
    volatile float speed;
    volatile std::uint32_t type;
    volatile std::uint32_t timedPath;
};

// The two comparisons that decide whether the foot detector reports a step. Each execution
// appends (left operand, right operand, foot index) to a ring buffer.
struct Comparison {
    float left, right;
    std::uint32_t foot;
};
constexpr int kRing = 32;
struct Ring {
    volatile std::uint32_t next;
    Comparison entries[kRing];
};

CodeBlock g_block;
Ring* g_valueGate = nullptr;  // xmm0 (limit) vs xmm6 (foot value): passes when limit > value
Ring* g_speedGate = nullptr;  // xmm6 (ped speed + margin) vs [r14] (foot speed): passes when greater
RouteCount* g_route = nullptr;
Classification* g_class = nullptr;
volatile std::uint32_t* g_counters = nullptr;
StepUpdate* g_update = nullptr;

int HeldCombo() {
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    return (down('W') << 0) | (down('A') << 1) | (down('S') << 2) | (down('D') << 3) |
           (down(VK_SHIFT) << 4);
}

const char* ComboName(int combo) {
    static char name[16];
    std::snprintf(name, sizeof name, "%s%s%s%s%s", (combo & 1) ? "W" : "", (combo & 2) ? "A" : "",
                  (combo & 4) ? "S" : "", (combo & 8) ? "D" : "", (combo & 16) ? "+Shift" : "");
    return name;
}

DWORD WINAPI Report(LPVOID) {
    std::uint32_t last[kCount] = {}, lastCalls = 0;
    RouteCount lastRoute = {};
    std::uint32_t lastValueNext = 0, lastSpeedNext = 0;
    for (;;) {
        Sleep(500);
        char line[512];
        int n = std::snprintf(line, sizeof line, "fn: [%s]", ComboName(HeldCombo()));
        bool any = false;
        for (size_t i = 0; i < kCount; ++i) {
            std::uint32_t now = g_counters[i], delta = now - last[i];
            last[i] = now;
            if (delta) {
                any = true;
                n += std::snprintf(line + n, sizeof line - n, " %s=%u", kFunctions[i].name, delta);
            }
        }
        if (any) Log::Write(line);
        Log::Info("step update: [%s] calls=%u lastType=%u", ComboName(HeldCombo()), g_update->calls - lastCalls, g_update->type);
        lastCalls = g_update->calls;
        Log::Info("route: [%s] queue=%u alternative=%u", ComboName(HeldCombo()), g_route->total - lastRoute.total, g_route->alternative - lastRoute.alternative);
        lastRoute = {g_route->total, g_route->alternative};
        for (auto [name, ring, lastNext] : {std::tuple{"value", g_valueGate, &lastValueNext}, std::tuple{"speed", g_speedGate, &lastSpeedNext}}) {
            std::uint32_t now = ring->next;
            std::uint32_t first = now - *lastNext > kRing ? now - kRing : *lastNext;
            n = std::snprintf(line, sizeof line, "%sgate: [%s]", name, ComboName(HeldCombo()));
            for (std::uint32_t i = first; i < now && i < first + 10; ++i) {
                const auto& e = ring->entries[i % kRing];
                n += std::snprintf(line + n, sizeof line - n, " f%u(%.2f|%.2f)", e.foot & 1, e.left, e.right);
            }
            if (now != first) Log::Write(line);
            *lastNext = now;
        }
        Log::Info("class: [%s] speed=%.3f type=0x%x timedPath=%u", ComboName(HeldCombo()), g_class->speed, g_class->type, g_class->timedPath & 0xFF);
    }
}

}  // namespace

void FootstepProbe::Install() {
    auto code = GameImage::Code();
    if (!g_block.Allocate(code.data(), 16 * 1024)) { Log::Info("footstep probe: no memory"); return; }
    g_counters = static_cast<volatile std::uint32_t*>(g_block.Reserve(kCount * 4));
    int patched = 0;
    for (size_t i = 0; i < kCount; ++i) {
        auto hit = Pattern::FindUnique(code, kFunctions[i].signature);
        if (!hit) { Log::Info("footstep probe: %s not found", kFunctions[i].name); continue; }
        auto* at = const_cast<std::uint8_t*>(*hit);
        int length = kFunctions[i].patchLength;
        const auto* stub = g_block.Cursor();
        g_block.Emit({0xF0, 0xFF, 0x05});  // lock inc dword [rip+counter]
        g_block.EmitRip32(const_cast<std::uint32_t*>(&g_counters[i]));
        g_block.Emit(std::span<const std::uint8_t>(at, length));
        g_block.EmitJmp(at + length);
        if (CodePatch::JumpToStub(at, std::span<const std::uint8_t>(at, length), stub)) ++patched;
    }
    Log::Info("footstep probe: %d of %zu functions instrumented", patched, kCount);

    // `cmp ecx,0x70000 ; sete al ; ret`: the test that picks the footstep handler.
    g_route = static_cast<RouteCount*>(g_block.Reserve(sizeof(RouteCount)));
    auto route = Pattern::FindUnique(code, "81 F9 00 00 07 00 0F 94 C0 C3");
    if (route) {
        auto* at = const_cast<std::uint8_t*>(*route);
        const auto* stub = g_block.Cursor();
        g_block.Emit({0xF0, 0xFF, 0x05});  // lock inc dword [rip+total]
        g_block.EmitRip32(const_cast<std::uint32_t*>(&g_route->total));
        g_block.Emit(std::span<const std::uint8_t>(at, 6));  // cmp ecx,0x70000
        g_block.Emit({0x75, 0x0D});                          // jne back (skips the 13 bytes below)
        g_block.Emit({0xF0, 0xFF, 0x05});                    // lock inc dword [rip+alternative]
        g_block.EmitRip32(const_cast<std::uint32_t*>(&g_route->alternative));
        g_block.Emit(std::span<const std::uint8_t>(at, 6));  // restore the flags the sete reads
        g_block.EmitJmp(at + 6);
        bool ok = CodePatch::JumpToStub(at, std::span<const std::uint8_t>(at, 6), stub);
        Log::Info("footstep probe: route test %s", ok ? "instrumented" : "FAILED");
    } else {
        Log::Info("footstep probe: route test not found");
    }

    // Append (left, right, foot) to `ring`; `leftModrm`/`rightLoad` select the compared registers.
    auto installGate = [&](const char* signature, int pairLength, Ring* ring, bool speedGate) {
        auto hit = Pattern::FindUnique(code, signature);
        if (!hit) return false;
        auto* at = const_cast<std::uint8_t*>(*hit);
        const std::uint8_t* exit = at + pairLength + *reinterpret_cast<const std::int32_t*>(at + pairLength - 4);
        const auto* stub = g_block.Cursor();
        g_block.Emit({0x50, 0x51});                              // push rax; push rcx
        g_block.Emit({0xB8, 0x01, 0x00, 0x00, 0x00});            // mov eax,1
        g_block.Emit({0xF0, 0x0F, 0xC1, 0x05});                  // lock xadd [rip+next],eax
        g_block.EmitRip32(const_cast<std::uint32_t*>(&ring->next));
        g_block.Emit({0x83, 0xE0, kRing - 1});                   // and eax,31
        g_block.Emit({0x48, 0x8D, 0x0D});                        // lea rcx,[rip+entries]
        g_block.EmitRip32(ring->entries);
        g_block.Emit({0x48, 0x8D, 0x04, 0x40});                  // lea rax,[rax+rax*2]
        g_block.Emit({0x48, 0x8D, 0x0C, 0x81});                  // lea rcx,[rcx+rax*4]
        if (speedGate) {
            g_block.Emit({0xF3, 0x0F, 0x11, 0x31});              // movss [rcx],xmm6
            g_block.Emit({0xF3, 0x41, 0x0F, 0x10, 0x06});        // movss xmm0,[r14]
            g_block.Emit({0xF3, 0x0F, 0x11, 0x41, 0x04});        // movss [rcx+4],xmm0
        } else {
            g_block.Emit({0xF3, 0x0F, 0x11, 0x01});              // movss [rcx],xmm0
            g_block.Emit({0xF3, 0x0F, 0x11, 0x71, 0x04});        // movss [rcx+4],xmm6
        }
        g_block.Emit({0x89, 0x79, 0x08});                        // mov [rcx+8],edi
        g_block.Emit({0x59, 0x58});                              // pop rcx; pop rax
        g_block.Emit(std::span<const std::uint8_t>(at, pairLength - 6));  // the compare
        g_block.Emit({0x0F, 0x86});                              // jbe exit
        g_block.EmitRip32(exit);
        g_block.EmitJmp(at + pairLength);
        return CodePatch::JumpToStub(at, std::span<const std::uint8_t>(at, pairLength), stub);
    };
    g_valueGate = static_cast<Ring*>(g_block.Reserve(sizeof(Ring)));
    g_speedGate = static_cast<Ring*>(g_block.Reserve(sizeof(Ring)));
    bool valueOk = installGate("0F 2E C6 0F 86 ?? ?? ?? ?? 80 BE ?? ?? ?? ?? ?? 0F 57 F6 74", 9, g_valueGate, false);
    bool speedOk = installGate("41 0F 2E 36 0F 86 ?? ?? ?? ?? 44 09 E8 89 84 AE ?? ?? ?? ?? 80 BE ?? ?? ?? ?? ?? 0F 84", 10, g_speedGate, true);
    Log::Info("footstep probe: value gate %s, speed gate %s", valueOk ? "ok" : "FAILED", speedOk ? "ok" : "FAILED");

    g_class = static_cast<Classification*>(g_block.Reserve(sizeof(Classification)));
    // Classifier entry: `push rsi; push rdi; push rbx; sub rsp,N` (7 bytes). Records xmm1 (speed)
    // when the helper's ped is the player (ped flags & 3 == 2).
    auto classifier = Pattern::FindUnique(code, "56 57 53 48 83 EC ?? 0F 29 74 24 ?? 4C 89 CF 4C 89 C6 48 89 CB");
    if (classifier) {
        auto* at = const_cast<std::uint8_t*>(*classifier);
        const auto* stub = g_block.Cursor();
        g_block.Emit({0x48, 0x8B, 0x81, 0xE0, 0x01, 0x00, 0x00});  // mov rax,[rcx+0x1e0]
        g_block.Emit({0x0F, 0xB6, 0x80, 0x90, 0x10, 0x00, 0x00});  // movzx eax,byte [rax+0x1090]
        g_block.Emit({0x83, 0xE0, 0x03, 0x83, 0xF8, 0x02});        // and eax,3 ; cmp eax,2
        g_block.Emit({0x75, 0x08});                                // jne skip
        g_block.Emit({0xF3, 0x0F, 0x11, 0x0D});                    // movss [rip+speed],xmm1
        g_block.EmitRip32(const_cast<float*>(&g_class->speed));
        g_block.Emit(std::span<const std::uint8_t>(at, 7));
        g_block.EmitJmp(at + 7);
        bool ok = CodePatch::JumpToStub(at, std::span<const std::uint8_t>(at, 7), stub);
        Log::Info("footstep probe: classifier %s", ok ? "instrumented" : "FAILED");
    }
    // After the classifier returns: `mov edx,[rsp+N] ; test al,al`. Records type and al.
    auto result = Pattern::FindUnique(code, "8B 54 24 ?? 84 C0 74 ?? 89 D0 83 E0 ?? 83 F8 ?? 75 ?? 48 89 F1");
    if (result) {
        auto* at = const_cast<std::uint8_t*>(*result);
        const auto* stub = g_block.Cursor();
        g_block.Emit({0x48, 0x8B, 0x8E, 0xE0, 0x01, 0x00, 0x00});  // mov rcx,[rsi+0x1e0]
        g_block.Emit({0x0F, 0xB6, 0x89, 0x90, 0x10, 0x00, 0x00});  // movzx ecx,byte [rcx+0x1090]
        g_block.Emit({0x83, 0xE1, 0x03, 0x83, 0xF9, 0x02});        // and ecx,3 ; cmp ecx,2
        g_block.Emit({0x75, 0x10});                                // jne skip
        g_block.Emit({0x8B, 0x4C, 0x24, at[3]});                   // mov ecx,[rsp+N]
        g_block.Emit({0x89, 0x0D});                                // mov [rip+type],ecx
        g_block.EmitRip32(const_cast<std::uint32_t*>(&g_class->type));
        g_block.Emit({0x88, 0x05});                                // mov [rip+timedPath],al
        g_block.EmitRip32(const_cast<std::uint32_t*>(&g_class->timedPath));
        g_block.Emit(std::span<const std::uint8_t>(at, 6));
        g_block.EmitJmp(at + 6);
        bool ok = CodePatch::JumpToStub(at, std::span<const std::uint8_t>(at, 6), stub);
        Log::Info("footstep probe: classification result %s", ok ? "instrumented" : "FAILED");
    }

    // Entry of the per-ped footstep update: counts calls and records r8d (the movement type).
    g_update = static_cast<StepUpdate*>(g_block.Reserve(sizeof(StepUpdate)));
    auto update = Pattern::FindUnique(code, "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC ?? ?? ?? ?? 44 0F 29 94 24 ?? ?? ?? ?? 44 0F 29 8C 24 ?? ?? ?? ?? 44 0F 29 84 24 ?? ?? ?? ?? 0F 29 BC 24 ?? ?? ?? ?? 0F 29 B4 24 ?? ?? ?? ?? 4C 89 4C 24 ?? 44 89 C6");
    if (update) {
        auto* at = const_cast<std::uint8_t*>(*update);
        const auto* stub = g_block.Cursor();
        g_block.Emit({0xF0, 0xFF, 0x05});        // lock inc dword [rip+calls]
        g_block.EmitRip32(const_cast<std::uint32_t*>(&g_update->calls));
        g_block.Emit({0x44, 0x89, 0x05});        // mov [rip+type], r8d
        g_block.EmitRip32(const_cast<std::uint32_t*>(&g_update->type));
        g_block.Emit(std::span<const std::uint8_t>(at, 6));
        g_block.EmitJmp(at + 6);
        bool ok = CodePatch::JumpToStub(at, std::span<const std::uint8_t>(at, 6), stub);
        Log::Info("footstep probe: step update %s", ok ? "instrumented" : "FAILED");
    } else {
        Log::Info("footstep probe: step update not found");
    }
    if (HANDLE t = CreateThread(nullptr, 0, Report, nullptr, 0, nullptr)) CloseHandle(t);
}
#endif
