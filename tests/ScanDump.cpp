// Host-side check: runs src/Pattern.cpp over an in-memory image dump (argv[1]).
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#include "../src/Pattern.h"
int main(int argc, char** argv) {
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<std::uint8_t> img((std::istreambuf_iterator<char>(f)), {});
    // .text from the dump's own section table (Enhanced and Legacy differ).
    auto u32 = [&](size_t at) { return std::uint32_t(img[at] | img[at + 1] << 8 | img[at + 2] << 16 | img[at + 3] << 24); };
    size_t nt = u32(0x3C), sections = nt + 24 + (img[nt + 20] | img[nt + 21] << 8);
    std::span<const std::uint8_t> text(img.data() + u32(sections + 12), u32(sections + 8));
    for (int i = 2; i < argc; ++i) {
        auto hits = Pattern::FindAll(text, argv[i]);
        std::printf("%zu", hits.size());
        for (auto h : hits) std::printf(" 0x%zx", size_t(h - img.data()));
        std::printf("\n");
    }
}
