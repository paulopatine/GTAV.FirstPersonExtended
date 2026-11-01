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
#include <string>
#include <vector>

#include "CodeBlock.h"
#include "GameImage.h"
#include "Log.h"
#include "MemoryPatch.h"
#include "Pattern.h"

namespace {

struct Site {
    std::uint8_t* at;
    int length;
    float value;  // 0 for a raw site
    bool raw;     // instructions copied as they are (no RIP operand), only counted
    // "bit" sites count only the executions where `test dword [reg+disp], mask` is non-zero.
    int watchReg = -1;
    std::int32_t watchDisp = 0;
    std::uint32_t watchMask = 0;
    // "fgt" sites count only the executions where xmm<watchXmm> (a positive float) is above the
    // threshold given as its bit pattern.
    int watchXmm = -1;
    std::uint32_t thresholdBits = 0;
    // "save" sites store a general register into g_saved[saveSlot] on every execution.
    int saveReg = -1;
    int saveSlot = 0;
};

// "chain <slot> <size> <offset>...": every sample, follow g_saved[slot] through the pointer
// offsets and record `size` bytes there (read with ReadProcessMemory, so a stale pointer only
// gives an empty row). Used to watch game objects (e.g. the player's weapon) over time.
struct Chain {
    int slot;
    std::size_t size;
    std::vector<std::uint32_t> offsets;
};
constexpr int kSavedSlots = 8;
std::vector<Chain> g_chains;
volatile std::uint64_t* g_saved = nullptr;

constexpr int kComboCount = 32;  // W, A, S, D, Shift bits
DWORD g_sampleMs = 100;  // "period <ms>" in the site list changes it
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

// One row per sample: time, keys (bit0 R, bit1 right mouse, bit2 left mouse, bit3 W, bit4 Shift),
// then every chain's bytes as dwords (empty when a pointer in the chain could not be read).
std::string SnapshotRow(DWORD now) {
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    int keys = down('R') | down(VK_RBUTTON) << 1 | down(VK_LBUTTON) << 2 | down('W') << 3 | down(VK_SHIFT) << 4;
    std::string row = std::to_string(now) + "," + std::to_string(keys);
    for (const auto& chain : g_chains) {
        std::uint64_t p = g_saved[chain.slot];
        bool ok = p != 0;
        for (std::uint32_t off : chain.offsets) {
            if (!ok) break;
            SIZE_T got = 0;
            ok = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(p + off), &p, 8, &got) && got == 8 && p;
        }
        std::vector<std::uint32_t> bytes(chain.size / 4);
        SIZE_T got = 0;
        ok = ok && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(p), bytes.data(), bytes.size() * 4, &got) &&
             got == bytes.size() * 4;
        row += ",|";
        if (ok)
            for (std::uint32_t v : bytes) {
                char hex[12];
                std::snprintf(hex, sizeof hex, " %x", v);
                row += hex;
            }
    }
    return row;
}

// Appends the rows not written yet. The file stays open for the whole run: reopening it to
// rewrite fails (EACCES) once something outside the game has the file open, e.g. a scanner.
bool DumpSnapshots(const std::vector<std::string>& rows, size_t& written) {
    static FILE* f = [] {
        wchar_t name[64];
        std::swprintf(name, 64, L"GTAV.FirstPersonExtended_snap_%lu.csv", GetCurrentProcessId());
        return _wfopen(GameImage::SiblingPath(name).c_str(), L"w");
    }();
    if (!f) return false;
    for (; written < rows.size(); ++written) std::fprintf(f, "%s\n", rows[written].c_str());
    return std::fflush(f) == 0;
}

DWORD WINAPI Sample(LPVOID) {
    std::vector<std::string> snapshots;
    size_t snapshotsWritten = 0;
    const DWORD start = GetTickCount();
    std::vector<std::uint32_t> last(g_sites.size());
    std::vector<std::uint64_t> counts(g_sites.size() * kComboCount);
    std::uint64_t samples[kComboCount] = {};
    bool reported = false;
    for (DWORD n = 1;; ++n) {
        Sleep(g_sampleMs);
        int combo = HeldCombo();
        ++samples[combo];
        for (size_t i = 0; i < g_sites.size(); ++i) {
            std::uint32_t now = g_counters[i];
            counts[i * kComboCount + combo] += now - last[i];
            last[i] = now;
        }
        if (!g_chains.empty() && snapshots.size() < 8000) snapshots.push_back(SnapshotRow(GetTickCount() - start));
        if (n % kDumpEverySamples == 0) {
            if (!g_chains.empty() && !DumpSnapshots(snapshots, snapshotsWritten))
                Log::Info("sprint probe: snapshot write FAILED (errno %d)", errno);
            bool ok = DumpCsv(counts, samples);
            bool failureLogged = reported;
            reported = reported || !ok;
            if (!ok ? !failureLogged : n == kDumpEverySamples) Log::Info("sprint probe: csv dump %s (errno %d)", ok ? "ok" : "FAILED", errno);
        }
    }
}

// A load of 2.0 or 3.0 from module data at `p`, `len` bytes long: recorded as a site.
bool AddSite(std::uint8_t* p, int len) {
    if (!len || ScalarRipLoadLength(p) != len) return false;
    const auto* target = Pattern::ResolveRip(p, len - 4, len);
    if (!IsModuleData(target)) return false;
    float value;
    std::memcpy(&value, target, sizeof value);
    if (value != 2.0f && value != 3.0f) return false;
    g_sites.push_back({p, len, value, false});
    return true;
}

// Optional site list (dev only): "rva length" per line, made offline with a disassembler for this
// exact EXE, so only whole instructions in the chosen functions are instrumented. Each entry still
// has to pass AddSite. Without the file every match in the code section is used.
bool LoadSiteList() {
    FILE* f = _wfopen(GameImage::SiblingPath(L"GTAV.FirstPersonExtended_probe_sites.txt").c_str(), L"r");
    if (!f) return false;
    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    auto code = GameImage::Code();
    char line[128];
    int rejected = 0;
    while (std::fgets(line, sizeof line, f)) {
        unsigned long rva = 0;
        int len = 0;
        char kind[8] = {};
        unsigned long period = 0;
        if (std::sscanf(line, "period %lu", &period) == 1) {
            if (period >= 10 && period <= 1000) g_sampleMs = DWORD(period);
            continue;
        }
        if (std::strncmp(line, "chain", 5) == 0) {
            Chain chain{};
            unsigned long size = 0;
            int used = 0;
            if (std::sscanf(line, "chain %d %lx%n", &chain.slot, &size, &used) == 2 && chain.slot >= 0 &&
                chain.slot < kSavedSlots && size > 0 && size <= 0x1000) {
                chain.size = size;
                unsigned long off = 0;
                int n = 0;
                for (const char* q = line + used; std::sscanf(q, "%lx%n", &off, &n) == 1; q += n)
                    chain.offsets.push_back(std::uint32_t(off));
                g_chains.push_back(chain);
            } else {
                ++rejected;
            }
            continue;
        }
        int fields = std::sscanf(line, "%lx %d %7s", &rva, &len, kind);
        if (fields < 2) continue;
        auto* p = base + rva;
        bool inCode = p >= code.data() && p + len <= code.data() + code.size();
        // "raw": whole non-relative instructions chosen offline, counted where they are.
        // "bit <reg> <disp> <mask>": raw, counted only while that bit of [reg+disp] is set.
        int reg = -1;
        std::int32_t disp = 0;
        std::uint32_t mask = 0;
        unsigned long d = 0, m = 0;
        // "nop": experiment only, overwrites the whole-instruction range with NOPs.
        if (inCode && fields == 3 && std::strcmp(kind, "nop") == 0 && len >= 1 && len <= 15) {
            std::uint8_t nops[15];
            std::memset(nops, 0x90, sizeof nops);
            std::vector<std::uint8_t> original(p, p + len);
            bool ok = MemoryPatch::Replace(p, original, {nops, size_t(len)});
            Log::Info("sprint probe: nop %lx len %d %s", rva, len, ok ? "applied" : "FAILED");
            continue;
        }
        int saveReg = -1, saveSlot = 0;
        if (inCode && std::sscanf(line, "%*x %*d save %d %d", &saveReg, &saveSlot) == 2 && saveReg >= 0 &&
            saveReg < 16 && saveReg != 4 && saveSlot >= 0 && saveSlot < kSavedSlots && len >= 5 && len <= 15) {
            Site site{p, len, 0.0f, true};
            site.saveReg = saveReg;
            site.saveSlot = saveSlot;
            g_sites.push_back(site);
            continue;
        }
        unsigned long xmm = 0, bits = 0;
        if (inCode && std::sscanf(line, "%*x %*d fgt %lu %lx", &xmm, &bits) == 2 && xmm < 16 && len >= 5 && len <= 15) {
            Site site{p, len, 0.0f, true};
            site.watchXmm = int(xmm);
            site.thresholdBits = std::uint32_t(bits);
            g_sites.push_back(site);
            continue;
        }
        if (inCode && std::sscanf(line, "%*x %*d bit %d %lx %lx", &reg, &d, &m) == 3 && reg >= 0 && reg <= 7 &&
            reg != 4 && len >= 5 && len <= 15 && (disp = std::int32_t(d), mask = std::uint32_t(m), true))
            g_sites.push_back({p, len, 0.0f, true, reg, disp, mask});
        else if (inCode && fields == 3 && std::strcmp(kind, "raw") == 0 && len >= 5 && len <= 15)
            g_sites.push_back({p, len, 0.0f, true});
        else if (!inCode || !AddSite(p, len))
            ++rejected;
    }
    std::fclose(f);
    Log::Info("sprint probe: site list loaded, %zu accepted, %d rejected", g_sites.size(), rejected);
    return true;
}

}  // namespace

void SprintProbe::Install() {
    auto code = GameImage::Code();
    if (!LoadSiteList())
        for (size_t i = 1; i + 16 < code.size(); ++i) {
            auto* p = const_cast<std::uint8_t*>(code.data() + i);
            int len = ScalarRipLoadLength(p);
            if (AddSite(p, len)) i += len - 1;
        }
    constexpr size_t kBlockSize = 256 * 1024;
    if (!g_block.Allocate(code.data(), kBlockSize)) { Log::Info("sprint probe: no memory"); return; }
    g_counters = static_cast<volatile std::uint32_t*>(g_block.Reserve(g_sites.size() * 4));
    g_saved = static_cast<volatile std::uint64_t*>(g_block.Reserve(kSavedSlots * 8));
    int patched = 0;
    for (size_t i = 0; i < g_sites.size(); ++i) {
        auto [at, len, value, raw, watchReg, watchDisp, watchMask, watchXmm, thresholdBits, saveReg, saveSlot] = g_sites[i];
        const auto* stub = g_block.Cursor();
        // pushfq; lock inc dword [rip+counter]; popfq -- keeps the game's flags intact.
        g_block.Emit({0x9C});
        if (saveReg >= 0) {  // mov [rip+slot],reg
            g_block.Emit({std::uint8_t(0x48 | (saveReg >= 8 ? 4 : 0)), 0x89, std::uint8_t(0x05 | ((saveReg & 7) << 3))});
            g_block.EmitRip32(const_cast<std::uint64_t*>(&g_saved[saveSlot]));
        }
        std::uint8_t* skip = nullptr;
        if (watchReg >= 0) {  // test dword [reg+disp32], imm32 ; jz skip
            g_block.Emit({0xF7, std::uint8_t(0x80 | watchReg)});
            g_block.Emit(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&watchDisp), 4));
            g_block.Emit(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&watchMask), 4));
            skip = g_block.EmitBranch({0x0F, 0x84});
        }
        if (watchXmm >= 0) {  // movss [rsp-8]... done with a real push so no red zone is assumed
            g_block.Emit({0x48, 0x83, 0xEC, 0x08});  // sub rsp,8
            if (watchXmm >= 8) g_block.Emit({0xF3, 0x44, 0x0F, 0x11, std::uint8_t(0x04 | ((watchXmm - 8) << 3)), 0x24});
            else g_block.Emit({0xF3, 0x0F, 0x11, std::uint8_t(0x04 | (watchXmm << 3)), 0x24});
            g_block.Emit({0x81, 0x3C, 0x24});  // cmp dword [rsp],imm32
            g_block.Emit(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&thresholdBits), 4));
            g_block.Emit({0x48, 0x8D, 0x64, 0x24, 0x08});  // lea rsp,[rsp+8] (keeps the flags)
            skip = g_block.EmitBranch({0x0F, 0x86});       // jbe skip
        }
        g_block.Emit({0xF0, 0xFF, 0x05});
        g_block.EmitRip32(const_cast<std::uint32_t*>(&g_counters[i]));
        if (skip) g_block.Bind(skip);
        g_block.Emit({0x9D});
        if (raw) {
            g_block.Emit(std::span<const std::uint8_t>(at, len));
        } else {
            g_block.Emit(std::span<const std::uint8_t>(at, len - 4));
            g_block.EmitRip32(Pattern::ResolveRip(at, len - 4, len));
        }
        g_block.EmitJmp(at + len);
        if (CodePatch::JumpToStub(at, std::span<const std::uint8_t>(at, len), stub)) ++patched;
    }
    Log::Info("sprint probe: %d of %zu sites instrumented", patched, g_sites.size());
    if (HANDLE t = CreateThread(nullptr, 0, Sample, nullptr, 0, nullptr)) CloseHandle(t);
    else Log::Info("sprint probe: sampler thread failed (error %lu)", GetLastError());
}
#endif
