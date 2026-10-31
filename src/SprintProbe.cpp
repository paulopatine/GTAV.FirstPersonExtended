// Diagnostic build only (make sprintprobe). Not part of the released mod.
// Locating the code that drops a first-person sprint to a run on W+A / W+D: every SSE
// instruction that loads the move-blend-ratio constants 2.0 (run) or 3.0 (sprint) from
// read-only data is redirected to a stub that bumps a per-site counter. A sampling thread
// attributes the counts to whichever W/A/S/D/Shift combination is held, so offline analysis
// can tell which code runs while sprinting straight but not while sprinting diagonally.
#ifdef FPFIX_SPRINT_PROBE
#include "SprintProbe.h"

#include <windows.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

#include "CodeBlock.h"
#include "GameImage.h"
#include "Log.h"
#include "Pattern.h"

namespace {

struct Site {
    std::uint8_t* at;
    int length;
    float value;
};

constexpr int kComboCount = 32;  // W, A, S, D, Shift bits
constexpr DWORD kSampleMs = 100;
constexpr DWORD kDumpEverySamples = 50;

CodeBlock g_block;
std::vector<Site> g_sites;
volatile std::uint32_t* g_counters = nullptr;

bool IsRex(std::uint8_t b) { return (b & 0xF0) == 0x40; }

// Length of a scalar SSE op reading [rip+disp32] as its last 4 bytes, else 0.
//   F3 [REX] 0F {10,58,59,5C,5D,5E,5F} modrm(rip)   movss/addss/mulss/subss/minss/divss/maxss
//   0F {2E,2F} modrm(rip)                            ucomiss/comiss (no REX or 66 prefix: a
//                                                    REX byte can't be told apart from the
//                                                    previous instruction's last byte, and
//                                                    66 0F 2E is ucomisd, a double compare)
int ScalarRipLoadLength(const std::uint8_t* p) {
    auto isRipModrm = [](std::uint8_t m) { return (m & 0xC7) == 0x05; };
    if (p[0] == 0xF3) {
        int rex = IsRex(p[1]) ? 1 : 0;
        const auto* q = p + 1 + rex;
        if (q[0] != 0x0F || !isRipModrm(q[2])) return 0;
        switch (q[1]) {
            case 0x10: case 0x58: case 0x59: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
                return 8 + rex;
        }
        return 0;
    }
    if (p[0] == 0x0F && (p[1] == 0x2E || p[1] == 0x2F) && isRipModrm(p[2]) && !IsRex(p[-1]) &&
        p[-1] != 0x66 && p[-1] != 0xF2 && p[-1] != 0xF3)
        return 7;
    return 0;
}

// Constant data: inside the main module but outside its code section. (Page protection
// can't be used: the protected EXE leaves its data sections writable in memory.)
bool IsModuleData(const std::uint8_t* p) {
    const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto code = GameImage::Code();
    bool inModule = p >= base && p < base + nt->OptionalHeader.SizeOfImage;
    bool inCode = p >= code.data() && p < code.data() + code.size();
    return inModule && !inCode;
}

int HeldCombo() {
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    return (down('W') << 0) | (down('A') << 1) | (down('S') << 2) | (down('D') << 3) |
           (down(VK_SHIFT) << 4);
}

bool DumpCsv(const std::vector<std::uint64_t>& counts, const std::uint64_t* samples) {
    // One file per launch: overwriting the previous run's file is denied (EACCES) once it
    // has been touched from outside the game.
    wchar_t name[64];
    std::swprintf(name, 64, L"GTAV.FirstPersonExtended_sprint_%lu.csv", GetCurrentProcessId());
    auto path = GameImage::SiblingPath(name);
    FILE* f = _wfopen(path.c_str(), L"w");
    if (!f) return false;
    const auto* exe = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    std::fprintf(f, "samples");
    for (int c = 0; c < kComboCount; ++c) std::fprintf(f, ",%llu", (unsigned long long)samples[c]);
    std::fprintf(f, "\n");
    for (size_t i = 0; i < g_sites.size(); ++i) {
        std::fprintf(f, "%zx,%g", size_t(g_sites[i].at - exe), g_sites[i].value);
        for (int c = 0; c < kComboCount; ++c)
            std::fprintf(f, ",%llu", (unsigned long long)counts[i * kComboCount + c]);
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    return true;
}

DWORD WINAPI Sample(LPVOID) {
    std::vector<std::uint32_t> last(g_sites.size());
    std::vector<std::uint64_t> counts(g_sites.size() * kComboCount);
    std::uint64_t samples[kComboCount] = {};
    bool reported = false;
    for (DWORD n = 1;; ++n) {
        Sleep(kSampleMs);
        int combo = HeldCombo();
        ++samples[combo];
        for (size_t i = 0; i < g_sites.size(); ++i) {
            std::uint32_t now = g_counters[i];
            counts[i * kComboCount + combo] += now - last[i];
            last[i] = now;
        }
        if (n % kDumpEverySamples == 0) {
            bool ok = DumpCsv(counts, samples);
            bool failureLogged = reported;
            reported = reported || !ok;
            if (!ok ? !failureLogged : n == kDumpEverySamples) Log::Info("sprint probe: csv dump %s (errno %d)", ok ? "ok" : "FAILED", errno);
        }
    }
}

}  // namespace

void SprintProbe::Install() {
    auto code = GameImage::Code();
    for (size_t i = 1; i + 16 < code.size(); ++i) {
        auto* p = const_cast<std::uint8_t*>(code.data() + i);
        int len = ScalarRipLoadLength(p);
        if (!len) continue;
        const auto* target = Pattern::ResolveRip(p, len - 4, len);
        if (!IsModuleData(target)) continue;
        float value;
        std::memcpy(&value, target, sizeof value);
        if (value != 2.0f && value != 3.0f) continue;
        g_sites.push_back({p, len, value});
        i += len - 1;
    }
    constexpr size_t kBlockSize = 256 * 1024;
    if (!g_block.Allocate(code.data(), kBlockSize)) { Log::Info("sprint probe: no memory"); return; }
    g_counters = static_cast<volatile std::uint32_t*>(g_block.Reserve(g_sites.size() * 4));
    int patched = 0;
    for (size_t i = 0; i < g_sites.size(); ++i) {
        auto [at, len, value] = g_sites[i];
        const auto* target = Pattern::ResolveRip(at, len - 4, len);
        const auto* stub = g_block.Cursor();
        // pushfq; lock inc dword [rip+counter]; popfq -- keeps the game's flags intact.
        g_block.Emit({0x9C, 0xF0, 0xFF, 0x05});
        g_block.EmitRip32(const_cast<std::uint32_t*>(&g_counters[i]));
        g_block.Emit({0x9D});
        g_block.Emit(std::span<const std::uint8_t>(at, len - 4));
        g_block.EmitRip32(target);
        g_block.EmitJmp(at + len);
        if (CodePatch::JumpToStub(at, std::span<const std::uint8_t>(at, len), stub)) ++patched;
    }
    Log::Info("sprint probe: %d of %zu sites instrumented", patched, g_sites.size());
    if (HANDLE t = CreateThread(nullptr, 0, Sample, nullptr, 0, nullptr)) CloseHandle(t);
    else Log::Info("sprint probe: sampler thread failed (error %lu)", GetLastError());
}
#endif
