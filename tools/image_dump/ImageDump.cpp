// Development-only tool (not shipped with the mod).
// GTA5_Enhanced.exe keeps its .text encrypted on disk; the protection decrypts it
// in memory at startup. This ASI waits until the code looks decrypted (entropy drop),
// then writes the main module image, as laid out in memory, next to the game EXE
// so it can be analysed offline. It only reads the game's own process memory.
#include <windows.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::wstring GameDirectoryFile(const wchar_t* name) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    dir.resize(dir.find_last_of(L"\\/") + 1);
    return dir + name;
}

FILE* g_log = nullptr;

void Log(const char* fmt, auto... args) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(g_log, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    std::fprintf(g_log, fmt, args...);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

bool IsReadable(const MEMORY_BASIC_INFORMATION& m) {
    if (m.State != MEM_COMMIT) return false;
    if (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return true;
}

// Entropy of a readable range; returns -1 if any page in it is unreadable.
double Entropy(const unsigned char* p, size_t n) {
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery(p, &m, sizeof m) || !IsReadable(m) ||
        static_cast<const unsigned char*>(m.BaseAddress) + m.RegionSize < p + n)
        return -1;
    std::array<size_t, 256> count{};
    for (size_t i = 0; i < n; ++i) ++count[p[i]];
    double e = 0;
    for (size_t c : count)
        if (c) e -= double(c) / n * std::log2(double(c) / n);
    return e;
}

struct Section {
    char name[9]{};
    unsigned char* start;
    size_t size;
};

std::vector<Section> Sections(unsigned char* base) {
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    std::vector<Section> out;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        Section s{{}, base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize};
        std::memcpy(s.name, sec[i].Name, 8);
        out.push_back(s);
    }
    return out;
}

// Average entropy over samples spread across the first code section.
double CodeEntropy(const Section& code) {
    constexpr size_t kSample = 64 * 1024, kSamples = 16;
    double sum = 0;
    int ok = 0;
    for (size_t i = 0; i < kSamples; ++i) {
        double e = Entropy(code.start + (code.size - kSample) * i / (kSamples - 1), kSample);
        if (e >= 0) sum += e, ++ok;
    }
    return ok ? sum / ok : -1;
}

void WriteImage(unsigned char* base, size_t imageSize, const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) { Log("cannot create dump file"); return; }
    std::vector<unsigned char> zeros(0x10000);
    size_t copied = 0, zeroed = 0;
    for (unsigned char* p = base; p < base + imageSize;) {
        MEMORY_BASIC_INFORMATION m{};
        VirtualQuery(p, &m, sizeof m);
        size_t n = static_cast<unsigned char*>(m.BaseAddress) + m.RegionSize - p;
        if (p + n > base + imageSize) n = base + imageSize - p;
        if (IsReadable(m)) {
            std::fwrite(p, 1, n, f);
            copied += n;
        } else {
            for (size_t left = n; left;) {
                size_t k = left < zeros.size() ? left : zeros.size();
                std::fwrite(zeros.data(), 1, k, f);
                left -= k;
            }
            zeroed += n;
        }
        p += n;
    }
    std::fclose(f);
    Log("dump written: %zu bytes copied, %zu bytes unreadable (zero-filled)", copied, zeroed);
}

DWORD WINAPI Run(LPVOID) {
    g_log = _wfopen(GameDirectoryFile(L"GTAV.FirstPersonExtended_dump.log").c_str(), L"w");
    auto base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(
        base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    size_t imageSize = nt->OptionalHeader.SizeOfImage;
    auto sections = Sections(base);
    Log("base=%p imageSize=0x%zx timestamp=0x%08lx", base, imageSize, nt->FileHeader.TimeDateStamp);

    // Poll up to 10 minutes; protected code usually decrypts within seconds of startup.
    double e = -1;
    for (int i = 0; i < 600; ++i) {
        e = CodeEntropy(sections[0]);
        if (i % 10 == 0) Log("code entropy %.3f", e);
        if (e >= 0 && e < 7.2) break;
        Sleep(1000);
    }
    Log("code entropy at decision %.3f", e);
    // Extra settle time so late-initialised globals in .data are populated too.
    Sleep(20000);
    for (auto& s : sections) {
        double se = s.size >= 65536 ? Entropy(s.start, 65536) : -2;
        Log("section %-8s rva=0x%08zx size=0x%08zx entropy(first 64K)=%.3f", s.name,
            size_t(s.start - base), s.size, se);
    }
    WriteImage(base, imageSize, GameDirectoryFile(L"GTAV.FirstPersonExtended_image.bin"));
    Log("done");
    std::fclose(g_log);
    g_log = nullptr;
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (HANDLE t = CreateThread(nullptr, 0, Run, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
