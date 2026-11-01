// Diagnostic build only (make vmprobe). Not part of the released mod. Reads only, except for
// swapping entries of the game's draw-list command table (heap data) to counting stubs.
// Question it answers: which draw-list command draws the player's weapon in the main camera
// viewports, and where its matrix lives in the command, so the weapon can later join the
// first-person ped draw (which the game already draws through a private viewport).
// Earlier rounds (upload detour) are in the session logs; this round watches commands instead.
#ifdef FPFIX_VIEWMODEL_PROBE
#include "ViewmodelProbe.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>

#include "CodeBlock.h"
#include "GameImage.h"
#include "Log.h"
#include "Pattern.h"

namespace {

// grcViewport and render-context fields of build 1.0.1158.16 (probe only).
constexpr int kCameraPosition = 0xf0, kFov = 0x410, kAspect = 0x43c;
constexpr int kCurrentViewport = 0x15c48;  // TLS render context -> current grcViewport
constexpr int kFpvCommand = 0x84;          // draw-list command that draws a ped
constexpr int kFpvByte = 0xcd;             // its bit 7: first-person player ped

constexpr float kNearDistance = 2.5f, kMinDistance = 0.05f, kCell = 0.25f;
constexpr float kHeldDistance = 1.2f;  // a held weapon sits well within this of the eye
constexpr int kObjectCommand = 0x82;   // draw-list command that draws an object (weapon seen here)
// Object executor: the test that picks the private camera-relative viewport for this draw.
constexpr const char* kObjectViewportTest = "41 F6 87 ?? ?? ?? ?? 40 0F 94 C0 40 08 F0 4C 89 64 24 ?? 0F 85";
constexpr int kObjectTestLength = 8;  // test byte [r15+disp32],0x40
constexpr int kScanBytes = 0xc0;  // command bytes searched for an entity matrix

// Draw-list command registration: lea r8,[executor]; mov rcx,rsi; mov edx,id; xor r9d,r9d; call.
constexpr const char* kRegistration = "4C 8D 05 ?? ?? ?? ?? 48 89 F1 BA ?? 00 00 00 45 31 C9 E8";
// Command executor: copies the command's first-person-ped bit to the TLS render context.
constexpr const char* kExecutor = "48 83 EC 28 44 0F B6 89 ?? ?? ?? ?? 44 89 C8 C0 E8 07 44 8B 05 ?? ?? ?? ?? "
                                  "65 48 8B 14 25 58 00 00 00 4A 8B 14 C2 88 82 ?? ?? ?? ??";

const std::uint32_t* g_tlsIndex = nullptr;
const void* g_executors[256]{};

struct ViewportStat {
    std::atomic<std::uint32_t> key{0};  // FOV/aspect class (0 = free)
    float fov = 0, aspect = 0;
    std::atomic<std::uint32_t> commands[256]{};
};

struct NearMatrix {
    std::atomic<std::uint32_t> key{0};
    int viewport = 0, id = 0, offset = 0, fpv = 0, relative = 0, cx = 0, cy = 0, cz = 0;
    float dx = 0, dy = 0, dz = 0;
    std::atomic<std::uint32_t> count{0};
};

constexpr int kViewports = 64, kNear = 4096;
ViewportStat g_viewports[kViewports];
NearMatrix g_near[kNear];
CodeBlock g_block;

// The executor runs on the same thread right after the command stub, so a thread-local mark
// links a command to the viewport test inside its executor.
thread_local bool t_heldCommand = false;

// At the object executor's viewport test: [held][flag 0x40][sil] -> count, and the object's flag
// words for held objects versus other main-view objects (to find what marks the player's weapon).
std::atomic<std::uint32_t> g_test[2][2][2]{};
constexpr int kFlagWords = 64;
struct FlagWords {
    std::atomic<std::uint64_t> key{0};  // (+0x108) << 32 | (+0x10c), 0 = free
    std::uint32_t word110 = 0;
    std::atomic<std::uint32_t> count{0};
};
FlagWords g_flags[2][kFlagWords];

const std::uint8_t* RenderContext() {
    std::uintptr_t teb58;  // TEB ThreadLocalStoragePointer, as the game reads it
    asm volatile("mov %%gs:0x58, %0" : "=r"(teb58));
    return reinterpret_cast<const std::uint8_t* const*>(teb58)[*g_tlsIndex];
}

float F(const std::uint8_t* p, int off);

// The game builds fresh viewport objects every frame, so viewports are grouped by what they
// show (FOV and aspect), not by address.
int ViewportIndex(const std::uint8_t* vp) {
    std::uint32_t key = ((std::uint32_t(std::lround(F(vp, kFov) * 100)) << 12) ^
                         std::uint32_t(std::lround(F(vp, kAspect) * 1000))) | 0x80000000u;
    for (int i = 0; i < kViewports; ++i) {
        std::uint32_t cur = g_viewports[i].key.load(std::memory_order_relaxed);
        if (cur == key) return i;
        if (!cur) {
            std::uint32_t expected = 0;
            if (g_viewports[i].key.compare_exchange_strong(expected, key) || expected == key) return i;
        }
    }
    return -1;
}

float F(const std::uint8_t* p, int off) {
    float v;
    std::memcpy(&v, p + off, 4);
    return v;
}

// Commands sit in large draw-list buffers; only the page boundary needs checking.
bool Readable(const std::uint8_t* p, size_t n) {
    thread_local std::uintptr_t okPage = 0;
    auto last = (std::uintptr_t(p) + n - 1) & ~std::uintptr_t(0xfff);
    if (last == okPage || last == (std::uintptr_t(p) & ~std::uintptr_t(0xfff))) return true;
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery(reinterpret_cast<void*>(last), &m, sizeof m) || m.State != MEM_COMMIT ||
        (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    okPage = last;
    return true;
}

// Three unit, mutually orthogonal rows followed by a translation: an entity matrix.
bool IsMatrixAt(const std::uint8_t* p) {
    float r[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i][j] = F(p, i * 16 + j * 4);
    for (int i = 0; i < 3; ++i) {
        float len = r[i][0] * r[i][0] + r[i][1] * r[i][1] + r[i][2] * r[i][2];
        if (!(len > 0.8f && len < 1.2f)) return false;
    }
    for (int i = 0; i < 3; ++i) {
        const float* a = r[i];
        const float* b = r[(i + 1) % 3];
        if (std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) > 0.1f) return false;
    }
    return true;
}

void Record(int vp, int id, int offset, int fpv, int relative, const float* d) {
    int cx = int(std::floor(d[0] / kCell)), cy = int(std::floor(d[1] / kCell)), cz = int(std::floor(d[2] / kCell));
    std::uint32_t key = 2166136261u;
    for (int v : {vp, id, offset, fpv, relative, cx, cy, cz}) key = (key ^ std::uint32_t(v)) * 16777619u;
    key |= 1;
    for (int i = 0, slot = key % kNear; i < kNear; ++i, slot = (slot + 1) % kNear) {
        auto& e = g_near[slot];
        std::uint32_t cur = e.key.load(std::memory_order_acquire);
        if (cur == 0) {
            std::uint32_t expected = 0;
            if (e.key.compare_exchange_strong(expected, 0xFFFFFFFFu)) {
                e.viewport = vp, e.id = id, e.offset = offset, e.fpv = fpv, e.relative = relative;
                e.cx = cx, e.cy = cy, e.cz = cz, e.dx = d[0], e.dy = d[1], e.dz = d[2];
                e.key.store(key, std::memory_order_release);
            }
            cur = e.key.load(std::memory_order_acquire);
        }
        if (cur == key) {
            e.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

}  // namespace

extern "C" void ViewmodelProbeOnCommand(const std::uint8_t* cmd, int id) {
    if (!g_tlsIndex) return;
    const std::uint8_t* ctx = RenderContext();
    if (!ctx) return;
    const std::uint8_t* vp = *reinterpret_cast<const std::uint8_t* const*>(ctx + kCurrentViewport);
    if (!vp) return;
    int v = ViewportIndex(vp);
    if (v < 0) return;
    auto& s = g_viewports[v];
    s.fov = F(vp, kFov), s.aspect = F(vp, kAspect);
    s.commands[id].fetch_add(1, std::memory_order_relaxed);
    if (!Readable(cmd, kScanBytes + 64)) return;
    int fpv = id == kFpvCommand && (cmd[kFpvByte] & 0x80) ? 1 : 0;
    t_heldCommand = false;
    for (int off = 4; off <= kScanBytes; off += 4) {
        if (!IsMatrixAt(cmd + off)) continue;
        const float t[3] = {F(cmd + off, 48), F(cmd + off, 52), F(cmd + off, 56)};
        bool found = false;
        for (int relative = 0; relative < 2; ++relative) {
            float d[3];
            for (int i = 0; i < 3; ++i) d[i] = relative ? t[i] : t[i] - F(vp, kCameraPosition + 4 * i);
            float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            // Unit vectors and leftovers near zero look like translations; entities near the eye do not.
            bool unitLike = relative && len > 0.95f && len < 1.05f;
            if (len < kNearDistance && len > kMinDistance && !unitLike) {
                Record(v, id, off, fpv, relative, d);
                found = true;
                if (id == kObjectCommand && !relative && len < kHeldDistance && s.aspect > 1.7f)
                    t_heldCommand = true;
            }
        }
        if (found) off += 60;  // a matrix found: skip past it
    }
}

extern "C" void ViewmodelProbeOnObjectTest(const std::uint8_t* data, std::uint64_t rsi, int flagOffset) {
    if (!g_tlsIndex) return;
    const std::uint8_t* ctx = RenderContext();
    if (!ctx) return;
    const std::uint8_t* vp = *reinterpret_cast<const std::uint8_t* const*>(ctx + kCurrentViewport);
    if (!vp || F(vp, kAspect) < 1.7f) return;  // main camera views only
    int held = t_heldCommand ? 1 : 0, flag = data[flagOffset] & 0x40 ? 1 : 0, sil = (rsi & 0xff) ? 1 : 0;
    g_test[held][flag][sil].fetch_add(1, std::memory_order_relaxed);
    std::uint32_t w108, w10c, w110;
    std::memcpy(&w108, data + flagOffset, 4);
    std::memcpy(&w10c, data + flagOffset + 4, 4);
    std::memcpy(&w110, data + flagOffset + 8, 4);
    std::uint64_t key = (std::uint64_t(w108) << 32 | w10c) | 1ull << 63;  // never 0
    for (auto& e : g_flags[held]) {
        std::uint64_t cur = e.key.load(std::memory_order_relaxed);
        if (cur == key) { e.count.fetch_add(1, std::memory_order_relaxed); return; }
        if (!cur) {
            std::uint64_t expected = 0;
            if (e.key.compare_exchange_strong(expected, key)) { e.word110 = w110; e.count.fetch_add(1); return; }
        }
    }
}

namespace {

DWORD WINAPI Report(LPVOID) {
    static std::uint32_t last[kViewports][256], lastNear[kNear];
    for (;;) {
        Sleep(1000);
        char line[2048];
        for (int i = 0; i < kViewports && g_viewports[i].key; ++i) {
            auto& s = g_viewports[i];
            int n = std::snprintf(line, sizeof line, "vm cmds #%d fov=%.2f asp=%.3f:", i, s.fov, s.aspect);
            bool any = false;
            for (int id = 0; id < 256; ++id) {
                std::uint32_t c = s.commands[id], d = c - last[i][id];
                last[i][id] = c;
                if (d && n < int(sizeof line) - 16) n += std::snprintf(line + n, sizeof line - n, " %02x=%u", id, d), any = true;
            }
            if (any) Log::Write(line);
        }
        {
            static std::uint32_t lastTest[2][2][2], lastFlags[2][kFlagWords];
            for (int h = 0; h < 2; ++h)
                for (int f = 0; f < 2; ++f)
                    for (int b = 0; b < 2; ++b) {
                        std::uint32_t c = g_test[h][f][b], d = c - lastTest[h][f][b];
                        lastTest[h][f][b] = c;
                        if (d) Log::Info("vm test held=%d flag40=%d sil=%d n=%u", h, f, b, d);
                    }
            for (int h = 0; h < 2; ++h)
                for (int i = 0; i < kFlagWords; ++i) {
                    auto& e = g_flags[h][i];
                    std::uint32_t c = e.count, d = c - lastFlags[h][i];
                    lastFlags[h][i] = c;
                    std::uint64_t k = e.key;
                    if (k && d)
                        Log::Info("vm flags held=%d w108=%08x w10c=%08x w110=%08x n=%u", h,
                                  unsigned(k >> 32 & 0x7fffffff), unsigned(k & 0xffffffff), e.word110, d);
                }
        }
        for (int i = 0; i < kNear; ++i) {
            auto& e = g_near[i];
            std::uint32_t key = e.key, c = e.count, d = c - lastNear[i];
            if (!key || key == 0xFFFFFFFFu || !d) continue;
            lastNear[i] = c;
            Log::Info("vm mtx #%d id=%02x off=0x%x fpv=%d %s cell=(%d %d %d) n=%u d=(%.2f %.2f %.2f)", e.viewport,
                      e.id, e.offset, e.fpv, e.relative ? "rel" : "world", e.cx, e.cy, e.cz, d, e.dx, e.dy, e.dz);
        }
    }
}

// pushes argument registers and xmm0-3, calls the recorder, restores, jumps to the executor.
const void* MakeStub(int id, const void* executor) {
    const auto* stub = g_block.Cursor();
    g_block.Emit({0x51, 0x52, 0x41, 0x50, 0x41, 0x51});        // push rcx, rdx, r8, r9
    g_block.Emit({0x48, 0x83, 0xEC, 0x68});                    // sub rsp,0x68 (aligned)
    for (int x = 0; x < 4; ++x)                                // movdqu [rsp+0x28+16x], xmmX
        g_block.Emit({0xF3, 0x0F, 0x7F, std::uint8_t(0x44 | (x << 3)), 0x24, std::uint8_t(0x28 + 16 * x)});
    g_block.Emit({0xBA});                                      // mov edx, id
    std::int32_t i32 = id;
    g_block.Emit(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&i32), 4));
    g_block.Emit({0x48, 0xB8});
    auto handler = reinterpret_cast<std::uint64_t>(&ViewmodelProbeOnCommand);
    g_block.Emit(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&handler), 8));
    g_block.Emit({0xFF, 0xD0});                                // call rax
    for (int x = 0; x < 4; ++x)                                // movdqu xmmX, [rsp+0x28+16x]
        g_block.Emit({0xF3, 0x0F, 0x6F, std::uint8_t(0x44 | (x << 3)), 0x24, std::uint8_t(0x28 + 16 * x)});
    g_block.Emit({0x48, 0x83, 0xC4, 0x68});
    g_block.Emit({0x41, 0x59, 0x41, 0x58, 0x5A, 0x59});        // pop r9, r8, rdx, rcx
    g_block.Emit({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});        // jmp [rip+0]
    auto target = reinterpret_cast<std::uint64_t>(executor);
    g_block.Emit(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&target), 8));
    return stub;
}

// Mid-function detour: every volatile register is live here, so all are saved. The body of the
// object executor keeps rsp 16-byte aligned (8 pushes + sub 0x708), hence 7 pushes + sub 0x88.
bool DetourObjectTest(std::uint8_t* at) {
    std::int32_t flagOffset;
    std::memcpy(&flagOffset, at + 3, 4);
    const auto* stub = g_block.Cursor();
    g_block.Emit({0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});  // rax rcx rdx r8-r11
    g_block.Emit({0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00});                           // sub rsp,0x88
    for (int x = 0; x < 6; ++x)
        g_block.Emit({0xF3, 0x0F, 0x7F, std::uint8_t(0x44 | (x << 3)), 0x24, std::uint8_t(0x20 + 16 * x)});
    g_block.Emit({0x4C, 0x89, 0xF9});              // mov rcx, r15
    g_block.Emit({0x48, 0x89, 0xF2});              // mov rdx, rsi
    g_block.Emit({0x41, 0xB8});                    // mov r8d, flagOffset
    g_block.Emit(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&flagOffset), 4));
    g_block.Emit({0x48, 0xB8});
    auto handler = reinterpret_cast<std::uint64_t>(&ViewmodelProbeOnObjectTest);
    g_block.Emit(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&handler), 8));
    g_block.Emit({0xFF, 0xD0});
    for (int x = 0; x < 6; ++x)
        g_block.Emit({0xF3, 0x0F, 0x6F, std::uint8_t(0x44 | (x << 3)), 0x24, std::uint8_t(0x20 + 16 * x)});
    g_block.Emit({0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00});
    g_block.Emit({0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58});
    g_block.Emit(std::span<const std::uint8_t>(at, kObjectTestLength));
    g_block.EmitJmp(at + kObjectTestLength);
    return CodePatch::JumpToStub(at, std::span<const std::uint8_t>(at, kObjectTestLength), stub);
}

// The draw-list manager is a heap object whose pointer the game keeps in a global (stored by
// its constructor right after the vtable); its executor table starts at +0x10.
constexpr const char* kManagerCtor = "48 8D 05 ?? ?? ?? ?? 48 89 06 48 89 35 ?? ?? ?? ?? 4C 89 F1";
void* const* g_manager = nullptr;

void** FindTable() {
    std::uint8_t* const* slot = reinterpret_cast<std::uint8_t* const*>(g_manager);
    std::uint8_t* manager = slot ? *slot : nullptr;
    if (!manager) return nullptr;
    auto** table = reinterpret_cast<void**>(manager + 0x10);
    int matching = 0;
    for (int id = 0; id < 256; ++id) matching += g_executors[id] && table[id] == g_executors[id];
    for (int id = 0x80; id <= 0x93; ++id)
        if (g_executors[id] && table[id] != g_executors[id]) return nullptr;
    return matching >= 20 ? table : nullptr;
}

DWORD WINAPI InstallTableHooks(LPVOID) {
    void** table = nullptr;
    for (int tries = 0; tries < 120 && !table; ++tries) {
        Sleep(1000);
        table = FindTable();
    }
    if (!table) {
        auto* manager = g_manager ? static_cast<std::uint8_t*>(*g_manager) : nullptr;
        Log::Info("vm probe: command table not found (manager %p)", static_cast<void*>(manager));
        if (manager) {
            auto** t = reinterpret_cast<void**>(manager + 0x10);
            auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            for (int id = 0; id < 256; ++id)
                if (g_executors[id] && t[id] != g_executors[id])
                    Log::Info("vm probe: id %02x expected exe+0x%zx found %p", id,
                              size_t(std::uintptr_t(g_executors[id]) - exe), t[id]);
        }
        return 0;
    }
    int matching = 0, hooked = 0;
    for (int id = 0; id < 256; ++id)
        if (g_executors[id] && table[id] == g_executors[id]) ++matching;
    Log::Info("vm probe: command table at %p (%d registered executors match)", static_cast<void*>(table), matching);
    for (int id = 0; id < 256; ++id) {
        if (!g_executors[id] || table[id] != g_executors[id]) continue;
        InterlockedExchangePointer(&table[id], const_cast<void*>(MakeStub(id, g_executors[id])));
        ++hooked;
    }
    Log::Info("vm probe: %d command executors instrumented", hooked);
    if (HANDLE t = CreateThread(nullptr, 0, Report, nullptr, 0, nullptr)) CloseHandle(t);
    return 0;
}

}  // namespace

void ViewmodelProbe::Install() {
    auto code = GameImage::Code();
    auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto exec = Pattern::FindUnique(code, kExecutor);
    if (!exec) { Log::Info("vm probe: executor not found; not installed"); return; }
    g_tlsIndex = reinterpret_cast<const std::uint32_t*>(Pattern::ResolveRip(*exec + 18, 3, 7));
    // Other subsystems register into tables of their own (same byte shape, another registrar,
    // overlapping ids): keep only the registrar most calls go to.
    auto hits = Pattern::FindAll(code, kRegistration);
    const std::uint8_t* registrar = nullptr;
    size_t best = 0;
    for (const auto* r : hits) {
        const auto* target = Pattern::ResolveRip(r + 18, 1, 5);
        size_t n = 0;
        for (const auto* q : hits) n += Pattern::ResolveRip(q + 18, 1, 5) == target;
        if (n > best) best = n, registrar = target;
    }
    int registrations = 0;
    for (const auto* r : hits) {
        if (Pattern::ResolveRip(r + 18, 1, 5) != registrar) continue;
        g_executors[r[11]] = Pattern::ResolveRip(r, 3, 7);
        ++registrations;
    }
    Log::Info("vm probe: executor at exe+0x%zx, %d command registrations parsed", size_t(std::uintptr_t(*exec) - exe),
              registrations);
    if (auto ctor = Pattern::FindUnique(code, kManagerCtor))
        g_manager = reinterpret_cast<void* const*>(Pattern::ResolveRip(*ctor + 10, 3, 7));
    Log::Info("vm probe: draw-list manager global %s", g_manager ? "found" : "NOT found");
    if (!g_manager) return;
    if (!g_block.Allocate(code.data(), 64 * 1024)) { Log::Info("vm probe: no memory"); return; }
    if (auto test = Pattern::FindUnique(code, kObjectViewportTest))
        Log::Info("vm probe: object viewport test at exe+0x%zx %s", size_t(std::uintptr_t(*test) - exe),
                  DetourObjectTest(const_cast<std::uint8_t*>(*test)) ? "instrumented" : "NOT instrumented");
    else
        Log::Info("vm probe: object viewport test not found");
    if (HANDLE t = CreateThread(nullptr, 0, InstallTableHooks, nullptr, 0, nullptr)) CloseHandle(t);
}
#endif
