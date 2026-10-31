// Host-side check: runs src/Pattern.cpp over an in-memory image dump (argv[1]).
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#include "../src/Pattern.h"
int main(int argc, char** argv) {
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<std::uint8_t> img((std::istreambuf_iterator<char>(f)), {});
    std::span<const std::uint8_t> text(img.data() + 0x1000, 0x2479400);  // .text of the dump
    for (int i = 2; i < argc; ++i) {
        auto hits = Pattern::FindAll(text, argv[i]);
        std::printf("%zu", hits.size());
        for (auto h : hits) std::printf(" 0x%zx", size_t(h - img.data()));
        std::printf("\n");
    }
}
