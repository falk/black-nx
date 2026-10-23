#include "cpu/x86_ops.h"
#include <cstdio>
#include <random>
#include <vector>
X86_EXTERN_FUNC(sub_00140960);
X86_EXTERN_FUNC(__imp__sub_00140960);
int main() {
    std::mt19937 rng(140960);
    std::vector<uint8_t> a(1 << 24), b(a.size());
    constexpr uint32_t e = 0xA00200;
    for (unsigned t = 0; t < 12000; ++t) {
        uint8_t* base = a.data();
        for (auto [lo, n] : {std::pair{e - 0x200, 0x300u}, std::pair{0x800000u, 0x300u},
                            std::pair{0x289F00u, 0x100u}, std::pair{0x295D00u, 0x100u}})
            for (unsigned j = 0; j < n; j += 4) X86_MEM32(lo + j) = rng();
        // Finite predictor/step tables, including zero, negative and unstable
        // predictors that exercise both saturation limits and invalid products.
        for (unsigned j = 0x44; j < 0xC8; j += 4)
            X86_MEMF32(0x289F00 + j) = float(int(rng() % 65) - 32) / 32;
        for (unsigned j = 0; j < 16; ++j)
            X86_MEMF32(0x289F64 + j * 4) = float(1u << (rng() % 16)) / 268435456;
        X86_MEMF32(0x295DA0) = 1.f / 32768;
        X86_MEMF32(0x295DD4) = t % 11 ? 32767 : 1024;
        X86_MEMF32(0x295DD0) = t % 13 ? -32768 : -1024;
        uint32_t input = 0x800000, output = 0x800100;
        if (t % 17 == 0) input = e - 0x20;
        if (t % 19 == 0) output = e - 0x60;
        if (t % 23 == 0) output = input + 4;
        for (auto [lo, n] : {std::pair{e - 0x200, 0x300u}, std::pair{0x800000u, 0x300u},
                            std::pair{0x289F00u, 0x100u}, std::pair{0x295D00u, 0x100u}})
            std::memcpy(b.data() + lo, a.data() + lo, n);
        X86Context x{};
        x.eax = rng(); x.ebx = rng(); x.ecx = input; x.edx = rng(); x.esi = rng(); x.edi = output; x.esp = e;
        x.fcw = 0x37f; x.mxcsr = 0x1f80;
        for (auto& v : x.xmm) for (auto& u : v.u) u = rng();
        X86Context y = x;
        __imp__sub_00140960(x, a.data()); sub_00140960(y, b.data());
        bool same = std::memcmp(&x, &y, sizeof x) == 0;
        for (auto [lo, n] : {std::pair{e - 0x200, 0x300u}, std::pair{0x800000u, 0x300u}})
            same &= std::memcmp(a.data() + lo, b.data() + lo, n) == 0;
        if (!same) {
            std::fprintf(stderr, "ADPCM mismatch trial %u\n", t);
            const auto* xp = reinterpret_cast<uint8_t*>(&x); const auto* yp = reinterpret_cast<uint8_t*>(&y);
            for (unsigned i = 0; i < sizeof x; ++i) if (xp[i] != yp[i])
                std::fprintf(stderr, "ctx[%u] %02x %02x\n", i, xp[i], yp[i]);
            for (unsigned i = e - 0x200; i < e; ++i) if (a[i] != b[i])
                std::fprintf(stderr, "frame[%x] %02x %02x\n", i, a[i], b[i]);
            return 1;
        }
    }
    std::puts("PASS 12000 ADPCM blocks: samples, full context and scratch; saturation, custom limits and aliases");
}
