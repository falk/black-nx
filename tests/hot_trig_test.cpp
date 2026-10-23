#include "game/hot_trig.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>
int main(int argc, char **) {
    std::mt19937 rng(778899);
    const uint32_t edges[] = {0,          0x80000000, 1,          0x80000001, 0x3f490fdb,
                              0x3f490fda, 0x3fc90fdb, 0x40490fdb, 0x40c90fdb, 0x7f7fffff,
                              0xff7fffff, 0x7f800000, 0xff800000, 0x7fc12345, 0x7f812345};
    for (unsigned t = 0; t < 300000; ++t) {
        uint32_t bits = t < std::size(edges) ? edges[t] : rng();
        float f;
        std::memcpy(&f, &bits, 4);
        double angle = f;
        const auto actual = hot::ParticleSinCos(angle);
        const double expected[] = {std::sin(angle), std::cos(angle)};
        if (std::memcmp(&actual.sine, expected, 8) || std::memcmp(&actual.cosine, expected + 1, 8)) {
            std::printf("FAIL sin/cos angle %08x\n", bits);
            return 1;
        }
    }
    std::puts("PASS 300000 particle angles against Switch libm: double sin/cos bits, quadrants, huge inputs and NaNs");
    if (argc > 1) {
        std::vector<double> angles(1024);
        for (auto &v : angles)
            v = double(int(rng() % 100000) - 50000) / 100.0;
        double ns[2][5];
        volatile double sink = 0;
        for (unsigned r = 0; r < 5; ++r)
            for (unsigned step = 0; step < 2; ++step) {
                unsigned which = step ^ (r & 1);
                auto start = std::chrono::steady_clock::now();
                for (unsigned i = 0; i < 100000; ++i) {
                    const double angle = angles[i % angles.size()];
                    const auto v = which ? hot::ParticleSinCos(angle) : hot::SinCos{std::sin(angle), std::cos(angle)};
                    sink = v.sine + v.cosine;
                }
                ns[which][r] =
                    std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / 100000;
            }
        std::sort(ns[0], ns[0] + 5);
        std::sort(ns[1], ns[1] + 5);
        std::printf("BENCH Switch libm particle trig original %.2f shared-reduction %.2f ns (qemu only)\n", ns[0][2],
                    ns[1][2]);
    }
}
