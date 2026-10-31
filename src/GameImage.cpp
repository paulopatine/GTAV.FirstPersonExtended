#include "GameImage.h"

#include <windows.h>

#include <array>
#include <cmath>
#include <vector>

namespace {

const std::uint8_t* ModuleBase() {
    return reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
}

bool IsReadable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT ||
        (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    return static_cast<const std::uint8_t*>(m.BaseAddress) + m.RegionSize >=
           static_cast<const std::uint8_t*>(p) + n;
}

double Entropy(const std::uint8_t* p, size_t n) {
    std::array<size_t, 256> count{};
    for (size_t i = 0; i < n; ++i) ++count[p[i]];
    double e = 0;
    for (size_t c : count)
        if (c) e -= double(c) / n * std::log2(double(c) / n);
    return e;
}

// Encrypted data has ~8 bits/byte; x64 machine code sits around 6.0-6.5.
constexpr double kDecryptedCodeMaxEntropy = 7.2;

bool CodeLooksDecrypted() {
    auto code = GameImage::Code();
    constexpr size_t kSample = 64 * 1024, kSamples = 16;
    if (code.size() < kSample) return false;
    for (size_t i = 0; i < kSamples; ++i) {
        const auto* p = code.data() + (code.size() - kSample) * i / (kSamples - 1);
        if (!IsReadable(p, kSample) || Entropy(p, kSample) > kDecryptedCodeMaxEntropy)
            return false;
    }
    return true;
}

}  // namespace

std::wstring GameImage::SiblingPath(const wchar_t* fileName) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    dir.resize(dir.find_last_of(L"\\/") + 1);
    return dir + fileName;
}

std::string GameImage::Version() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    DWORD size = GetFileVersionInfoSizeW(path, nullptr);
    std::vector<std::uint8_t> data(size);
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!size || !GetFileVersionInfoW(path, 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info)
        return "unknown";
    char out[64];
    std::snprintf(out, sizeof out, "%u.%u.%u.%u", unsigned(HIWORD(info->dwFileVersionMS)),
                  unsigned(LOWORD(info->dwFileVersionMS)), unsigned(HIWORD(info->dwFileVersionLS)),
                  unsigned(LOWORD(info->dwFileVersionLS)));
    return out;
}

std::span<const std::uint8_t> GameImage::Code() {
    const auto* base = ModuleBase();
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
            return {base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize};
    return {};
}

bool GameImage::WaitForDecryptedCode(unsigned timeoutSeconds) {
    for (unsigned i = 0; i < timeoutSeconds * 4; ++i) {
        if (CodeLooksDecrypted()) return true;
        Sleep(250);
    }
    return false;
}
