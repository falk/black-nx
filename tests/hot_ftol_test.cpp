#include "cpu/x86_ops.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>
X86_EXTERN_FUNC(sub_000A34D4);
X86_EXTERN_FUNC(__imp__sub_000A34D4);
int main(int argc, char**) {
    std::mt19937_64 rng(0xA34D4);
    std::vector<uint8_t> a(8192), b(a.size());
    const double edges[] = {0., -0., .5, -.5, 1., -1., 1.5, -1.5, 1.e-40, -1.e-40,
        4294967296., -4294967296., 9223372036854775808., -9223372036854775808.};
    for (unsigned t = 0; t < 150000; ++t) {
        for (unsigned i = 0; i < 256; i += 8) { const auto bits = rng(); std::memcpy(a.data() + 4096 + i, &bits, 8); }
        const uint32_t e = 4224 + (t % 4) * 4;
        uint64_t bits = rng(); double value; std::memcpy(&value, &bits, 8);
        if (t % 3) value = double(int64_t(rng())) / 65536;
        if (t < sizeof(edges) / sizeof(*edges)) value = edges[t];
        X86Context x{};
        x.eax = rng(); x.ecx = rng(); x.edx = rng(); x.ebp = rng(); x.esp = e;
        x.top = rng() & 7; x.fcw = uint16_t((t % 3 == 0 ? 0x7f : t % 3 == 1 ? 0x27f : 0x37f) | ((t % 4) << 10));
        x.fsw = rng(); x.mxcsr = 0x1f80;
        for (auto& v : x.st) v = double(int64_t(rng()));
        x.st[x.top] = value;
        X86Context y = x; b = a;
        __imp__sub_000A34D4(x, a.data()); sub_000A34D4(y, b.data());
        if (std::memcmp(&x, &y, sizeof x) || a != b) {
            std::fprintf(stderr, "ftol mismatch trial %u value %.17g fcw %x\n", t, value, x.fcw);
            const auto* xp = reinterpret_cast<uint8_t*>(&x); const auto* yp = reinterpret_cast<uint8_t*>(&y);
            for (unsigned i = 0; i < sizeof x; ++i) if (xp[i] != yp[i])
                std::fprintf(stderr, "ctx[%u] %02x %02x\n", i, xp[i], yp[i]);
            for (unsigned i = 4096; i < 4352; ++i) if (a[i] != b[i])
                std::fprintf(stderr, "mem[%u] %02x %02x\n", i, a[i], b[i]);
            return 1;
        }
    }
    std::puts("PASS 150000 x87 integer conversions: full context/scratch, precision/rounding modes, boundaries and raw bits");
    if (argc > 1) {
        double ns[2][5];
        for (unsigned r = 0; r < 5; ++r) for (unsigned step = 0; step < 2; ++step) {
            unsigned which = step ^ (r & 1);
            X86Context ctx{}; ctx.fcw = 0x37f;
            auto start = std::chrono::steady_clock::now();
            for (unsigned i = 0; i < 500000; ++i) {
                ctx.top = 0; ctx.st[0] = double(i % 97) + .25; ctx.esp = 4224;
                (which ? sub_000A34D4 : __imp__sub_000A34D4)(ctx, a.data());
            }
            ns[which][r] = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / 500000;
        }
        std::sort(ns[0], ns[0] + 5); std::sort(ns[1], ns[1] + 5);
        std::printf("BENCH ftol original %.2f native %.2f ns/call (host/qemu only)\n", ns[0][2], ns[1][2]);
    }
}
