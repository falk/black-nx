#include "cpu/x86_ops.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
X86_EXTERN_FUNC(sub_000A2080);
X86_EXTERN_FUNC(__imp__sub_000A2080);
X86_EXTERN_FUNC(sub_000A20F0);
X86_EXTERN_FUNC(__imp__sub_000A20F0);
X86_EXTERN_FUNC(sub_00156D80);
X86Func *const *x86_icall_table = nullptr;
uint32_t x86_icall_lo = 0, x86_icall_count = 0;
void x86_trap(X86Context &, uint32_t, const char *) { std::abort(); }
void x86_icall(X86Context &ctx, uint8_t *base, uint32_t target) {
    ctx.last_icall = target;
    if (target != 0x156D80 && target != 0x156D81)
        std::abort();
    sub_00156D80(ctx, base);
}
int main(int argc, char **) {
    std::vector<uint8_t> a(1 << 24), b(a.size());
    std::mt19937 rng(872104);
    constexpr uint32_t e = 0xA00200, keys = 0x803000;
    for (unsigned full = 0; full < 2; ++full)
        for (unsigned t = 0; t < 20000; ++t) {
            uint8_t *base = a.data();
            for (auto [p, n] : {std::pair{e - 0x400, 0x500u}, std::pair{keys, 0x4000u}})
                for (unsigned j = 0; j < n; j += 4)
                    X86_MEM32(p + j) = rng();
            unsigned count = full && t % 11 == 0 ? t % 1024 : t % 65;
            const unsigned size = t % 13 ? 8 : 16;
            const unsigned comparator = t % 17 ? 0x156D80 : 0x156D81;
            uint32_t data = !full && t % 19 == 0 ? e - 32 : keys;
            if (data != keys)
                count = 2;
            X86_MEM32(e + 4) = data;
            X86_MEM32(e + 8) = full ? count : size;
            X86_MEM32(e + 12) = full ? size : comparator;
            X86_MEM32(e + 16) = comparator;
            // Identical keys, sorted/reverse order and signed priority wraparound.
            for (unsigned i = 0; i < 1024; ++i) {
                X86_MEM16(keys + i * size) = t % 4 == 0 ? i : t % 4 == 1 ? 1023 - i : t % 4 == 2 ? rng() % 7 : rng();
                X86_MEM8(keys + i * size + 6) = t % 5 ? rng() % 4 : rng();
            }
            for (auto [p, n] : {std::pair{e - 0x400, 0x500u}, std::pair{keys, 0x4000u}})
                std::memcpy(b.data() + p, a.data() + p, n);
            X86Context x{};
            x.eax = data + (count ? count - 1 : 0) * size;
            x.ebx = rng();
            x.ecx = rng();
            x.edx = rng();
            x.esi = rng();
            x.edi = rng();
            x.ebp = rng();
            x.esp = e;
            x.fcw = 0x37f;
            x.mxcsr = 0x1f80;
            for (auto &v : x.xmm)
                for (auto &u : v.u)
                    u = rng();
            auto y = x;
            (full ? __imp__sub_000A20F0 : __imp__sub_000A2080)(x, a.data());
            (full ? sub_000A20F0 : sub_000A2080)(y, b.data());
            bool same = !std::memcmp(&x, &y, sizeof x);
            for (auto [p, n] : {std::pair{e - 0x400, 0x500u}, std::pair{keys, 0x4000u}})
                same &= !std::memcmp(a.data() + p, b.data() + p, n);
            if (!same) {
                std::printf("FAIL render full %u trial %u\n", full, t);
                for (unsigned i = 0; i < sizeof x / 4; ++i)
                    if (((uint32_t *)&x)[i] != ((uint32_t *)&y)[i])
                        std::printf("ctx[%u] %08x/%08x\n", i, ((uint32_t *)&x)[i], ((uint32_t *)&y)[i]);
                for (unsigned p = e - 0x400; p < e + 0x100; p += 4)
                    if (std::memcmp(a.data() + p, b.data() + p, 4))
                        std::printf("stack %x %08x/%08x\n", p, *(uint32_t *)(a.data() + p),
                                    *(uint32_t *)(b.data() + p));
                return 1;
            }
        }
    std::puts(
        "PASS 40000 render sorts: duplicate/reversed/wrapped keys, full context/scratch and generic/alias fallbacks");
    if (argc > 1)
        for (unsigned kind = 0; kind < 3; ++kind) {
            double ns[2][5];
            const unsigned count = kind == 0 ? 8 : kind == 1 ? 64 : 512, iterations = kind == 2 ? 1000 : 10000;
            uint8_t *base = a.data();
            X86_MEM32(e + 4) = keys;
            X86_MEM32(e + 8) = kind ? count : 8;
            X86_MEM32(e + 12) = kind ? 8 : 0x156D80;
            X86_MEM32(e + 16) = 0x156D80;
            std::vector<uint8_t> fixture(count * 8);
            for (unsigned i = 0; i < count; ++i) {
                X86_MEM16(keys + i * 8) = rng() % 32;
                X86_MEM8(keys + i * 8 + 6) = rng() % 4;
            }
            std::memcpy(fixture.data(), base + keys, fixture.size());
            for (unsigned r = 0; r < 5; ++r)
                for (unsigned step = 0; step < 2; ++step) {
                    unsigned which = step ^ (r & 1);
                    X86Context c{};
                    c.mxcsr = 0x1f80;
                    auto start = std::chrono::steady_clock::now();
                    for (unsigned i = 0; i < iterations; ++i) {
                        c.esp = e;
                        c.eax = keys + (count - 1) * 8;
                        std::memcpy(base + keys, fixture.data(), fixture.size());
                        (kind ? (which ? sub_000A20F0 : __imp__sub_000A20F0)
                              : (which ? sub_000A2080 : __imp__sub_000A2080))(c, base);
                    }
                    ns[which][r] =
                        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() /
                        iterations;
                }
            std::sort(ns[0], ns[0] + 5);
            std::sort(ns[1], ns[1] + 5);
            std::printf("BENCH render sort %u keys original %.2f native %.2f ns including identical input reset "
                        "(host/qemu only)\n",
                        count, ns[0][2], ns[1][2]);
        }
}
