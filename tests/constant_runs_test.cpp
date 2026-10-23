#include "constant_runs.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
int main() {
    std::mt19937 rng(901738);
    std::array<uint8_t, 4096 + 32> a{}, b{};
    for (unsigned t = 0; t < 40000; ++t) {
        const unsigned bytes = (1 + rng() % 256) * 16, alignment = t % 16;
        auto* shadow = a.data() + alignment;
        auto* now = b.data() + alignment;
        const bool valid = t % 7;
        for (unsigned j = 0; j < bytes; ++j) shadow[j] = now[j] = uint8_t(rng());
        for (unsigned j = 0; j < t % 193; ++j) now[rng() % bytes] ^= uint8_t(rng());
        std::vector<unsigned> expected;
        for (unsigned j = 0; j < bytes; j += 16)
            if (!valid || std::memcmp(shadow + j, now + j, 16)) expected.push_back(j);
        std::vector<unsigned> emitted;
        gpu::dk::ChangedConstantRuns(shadow, now, bytes, valid, [&](uint32_t off, uint32_t n) {
            if (off % 16 || n % 16 || !n || n > 1024 || off + n > bytes) std::abort();
            for (unsigned j = off; j < off + n; j += 16) emitted.push_back(j);
            std::memcpy(shadow + off, now + off, n);
        });
        if (expected != emitted || std::memcmp(shadow, now, bytes)) std::abort();
        gpu::dk::ChangedConstantRuns(shadow, now, bytes, true, [](auto, auto) { std::abort(); });
    }
    std::puts("PASS 40000 constant uploads: exact changed registers, unaligned data, invalid shadows and 1 KiB limits");
}
