// Diagnostic build only (make probe). Not part of the released mod.
// Every camera clamps its FOV to [1, 130] right before writing it, so counting how often each
// instruction that loads the 130.0 constant runs tells which camera code is live in a given
// moment (on foot, ADS, vehicle...). Each such instruction is redirected to a stub that bumps
// a counter and then executes the original instruction re-encoded for the stub's address.
#ifdef FPFIX_CAMERA_PROBE
#include "CameraProbe.h"

#include <windows.h>

#include <cstring>
#include <vector>

#include "CodeBlock.h"
#include "FieldOfView.h"
#include "GameImage.h"
#include "Log.h"
#include "Pattern.h"

namespace {

struct Site {
    std::uint8_t* at;
    int length;
};

CodeBlock g_probe;
std::vector<Site> g_sites;
volatile std::uint32_t* g_counters = nullptr;

// Second probe: every `movss [reg+nearClip], xmm` store, with the last value written.
std::vector<Site> g_nearSites;
volatile std::uint32_t* g_nearCounters = nullptr;
volatile float* g_nearValues = nullptr;

// Length of `movss [base+disp32], xmmN` (optional REX) storing at `offset`, else 0.
int NearClipStoreLength(const std::uint8_t* p, std::int32_t offset) {
    int rex = (p[0] == 0xF3 && (p[1] & 0xF0) == 0x40) ? 1 : 0;
    const auto* q = p + rex;
    if (p[0] != 0xF3 || q[1] != 0x0F || q[2] != 0x11 || (q[3] & 0xC0) != 0x80 || (q[3] & 7) == 4)
        return 0;
    std::int32_t disp;
    std::memcpy(&disp, q + 4, 4);
    return disp == offset ? 8 + rex : 0;
}

// Instruction length if `p` is `movss xmm,[rip+d]` (opt. REX) or `ucomiss xmm,[rip+d]`.
int RipLoadLength(const std::uint8_t* p) {
    if (p[0] == 0xF3 && p[1] == 0x0F && p[2] == 0x10 && (p[3] & 0xC7) == 0x05) return 8;
    if (p[0] == 0xF3 && (p[1] & 0xF0) == 0x40 && p[2] == 0x0F && p[3] == 0x10 &&
        (p[4] & 0xC7) == 0x05)
        return 9;
    if (p[0] == 0x0F && p[1] == 0x2E && (p[2] & 0xC7) == 0x05) return 7;
    return 0;
}

// Several cameras share this exact clamp sequence; all must agree on one 130.0f constant.
const std::uint8_t* ClampConstant() {
    auto hits = Pattern::FindAll(GameImage::Code(),
                                 "0F 2E 35 ?? ?? ?? ?? 0F 82 ?? ?? ?? ?? F3 0F 10 05 ?? ?? ?? ?? "
                                 "0F 2E C6 0F 28 C6 0F 82");
    const std::uint8_t* clamp = nullptr;
    for (const auto* h : hits) {
        const auto* c = Pattern::ResolveRip(h + 13, 4, 8);
        float value;
        std::memcpy(&value, c, sizeof value);
        if (value != 130.0f || (clamp && c != clamp)) return nullptr;
        clamp = c;
    }
    return clamp;
}

DWORD WINAPI Report(LPVOID) {
    std::vector<std::uint32_t> last(g_sites.size()), lastNear(g_nearSites.size());
    const auto* exe = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    for (;;) {
        Sleep(1000);
        char line[1024];
        int n = std::snprintf(line, sizeof line, "probe:");
        for (size_t i = 0; i < g_sites.size(); ++i) {
            std::uint32_t now = g_counters[i], delta = now - last[i];
            last[i] = now;
            if (delta && n < int(sizeof line) - 32)
                n += std::snprintf(line + n, sizeof line - n, " %zx=%u", size_t(g_sites[i].at - exe), delta);
        }
        Log::Write(line);
        n = std::snprintf(line, sizeof line, "near:");
        for (size_t i = 0; i < g_nearSites.size(); ++i) {
            std::uint32_t now = g_nearCounters[i], delta = now - lastNear[i];
            lastNear[i] = now;
            if (delta && n < int(sizeof line) - 40)
                n += std::snprintf(line + n, sizeof line - n, " %zx=%u/%.3f",
                                   size_t(g_nearSites[i].at - exe), delta, g_nearValues[i]);
        }
        Log::Write(line);
    }
}

}  // namespace

void InstrumentNearClipStores(std::int32_t nearClipOffset) {
    auto code = GameImage::Code();
    for (size_t i = 0; i + 16 < code.size(); ++i) {
        auto* p = const_cast<std::uint8_t*>(code.data() + i);
        if (int len = NearClipStoreLength(p, nearClipOffset)) g_nearSites.push_back({p, len});
    }
    g_nearCounters = static_cast<volatile std::uint32_t*>(g_probe.Reserve(g_nearSites.size() * 4));
    g_nearValues = static_cast<volatile float*>(g_probe.Reserve(g_nearSites.size() * 4));
    int patched = 0;
    for (size_t i = 0; i < g_nearSites.size(); ++i) {
        auto [at, len] = g_nearSites[i];
        bool rex = len == 9;
        std::uint8_t modrm = at[rex ? 4 : 3];
        const auto* stub = g_probe.Cursor();
        g_probe.Emit({0x9C, 0xF0, 0xFF, 0x05});  // pushfq; lock inc [rip+counter]
        g_probe.EmitRip32(const_cast<std::uint32_t*>(&g_nearCounters[i]));
        g_probe.Emit({0x9D, 0xF3});  // popfq; movss [rip+value], xmmN (keeps REX.R only)
        if (rex && (at[1] & 0x04)) g_probe.Emit({0x44});
        g_probe.Emit({0x0F, 0x11, std::uint8_t((modrm & 0x38) | 5)});
        g_probe.EmitRip32(const_cast<float*>(&g_nearValues[i]));
        g_probe.Emit(std::span<const std::uint8_t>(at, len));
        g_probe.EmitJmp(at + len);
        if (CodePatch::JumpToStub(at, std::span<const std::uint8_t>(at, len), stub)) ++patched;
    }
    Log::Info("probe: %d of %zu near-clip stores instrumented", patched, g_nearSites.size());
}

void CameraProbe::Install() {
    const auto* clamp = ClampConstant();
    if (!clamp) { Log::Info("probe: clamp constant not found"); return; }
    auto code = GameImage::Code();
    for (size_t i = 0; i + 16 < code.size(); ++i) {
        auto* p = const_cast<std::uint8_t*>(code.data() + i);
        int len = RipLoadLength(p);
        if (len && Pattern::ResolveRip(p, len - 4, len) == clamp) g_sites.push_back({p, len});
    }
    constexpr size_t kBlockSize = 64 * 1024;
    if (!g_probe.Allocate(code.data(), kBlockSize)) { Log::Info("probe: no memory"); return; }
    g_counters = static_cast<volatile std::uint32_t*>(g_probe.Reserve(g_sites.size() * 4));
    int patched = 0;
    for (size_t i = 0; i < g_sites.size(); ++i) {
        auto [at, len] = g_sites[i];
        const auto* stub = g_probe.Cursor();
        // pushfq; lock inc dword [rip+counter]; popfq -- keeps the game's flags intact.
        g_probe.Emit({0x9C, 0xF0, 0xFF, 0x05});
        g_probe.EmitRip32(const_cast<std::uint32_t*>(&g_counters[i]));
        g_probe.Emit({0x9D});
        g_probe.Emit(std::span<const std::uint8_t>(at, len - 4));
        g_probe.EmitRip32(clamp);
        g_probe.EmitJmp(at + len);
        if (CodePatch::JumpToStub(at, std::span<const std::uint8_t>(at, len), stub)) ++patched;
    }
    Log::Info("probe: %d of %zu clamp sites instrumented", patched, g_sites.size());
    if (int fov = FieldOfView::FrameFovOffset(); fov >= 0) InstrumentNearClipStores(fov + 4);
    if (HANDLE t = CreateThread(nullptr, 0, Report, nullptr, 0, nullptr)) CloseHandle(t);
}
#endif
