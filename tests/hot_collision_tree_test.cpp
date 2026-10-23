#include "cpu/x86_ops.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
X86_EXTERN_FUNC(sub_000CFE40);
X86_EXTERN_FUNC(baseline_collision_walk);
X86_EXTERN_FUNC(__imp__sub_000CFE40);
X86Func *const *x86_icall_table = nullptr;
uint32_t x86_icall_lo = 0, x86_icall_count = 0;
static std::vector<std::array<uint32_t, 3>> calls;
static unsigned stop;
void x86_icall(X86Context &ctx, uint8_t *base, uint32_t target) {
    ctx.last_icall = target;
    if (target != 0x123456)
        std::abort();
    calls.push_back({X86_MEM32(ctx.esp + 4), X86_MEM32(ctx.esp + 8), X86_MEM32(ctx.esp + 12)});
    // A cdecl callback can clobber all caller-saved state. Preserve it exactly.
    ctx.eax = 0xabcd0000u | (calls.size() != stop);
    ctx.ecx = 0x1234;
    ctx.edx = 0x5678;
    for (auto &v : ctx.xmm)
        for (auto &u : v.u)
            u ^= 0xa5a5a5a5;
    ctx.esp += 4;
}
int main(int argc, char **) {
    constexpr uint32_t e = 0xA00200, object = 0x800000, segment = 0x800400, nodes = 0x801000;
    std::vector<uint8_t> a(1 << 24), b(a.size());
    std::mt19937 rng(7788);
    uint64_t callbacks = 0;
    for (unsigned t = 0; t < 12000; ++t) {
        uint8_t *base = a.data();
        for (auto [p, n] : {std::pair{e - 0x2000, 0x2100u}, std::pair{object, 0x5000u}})
            for (unsigned j = 0; j < n; j += 4)
                X86_MEM32(p + j) = rng();
        const unsigned count = 1 + t % 40;
        X86_MEM32(object + 0x4c) = segment;
        X86_MEM32(object + 0x58) = 0x123456;
        X86_MEM32(object + 0x64) = rng();
        X86_MEMF32(0x295C74) = 1.0f;
        for (unsigned i = 0; i < 8; ++i)
            X86_MEMF32(segment + i * 4) = i == 0 ? -5.0f : i == 4 ? 5.0f : 0.0f;
        for (unsigned i = 0; i < count; ++i) {
            const uint32_t p = nodes + i * 0x80;
            for (unsigned j = 0; j < 3; ++j)
                X86_MEMF32(p + j * 4) = t % 4 ? float(int(rng() % 32) - 16) : 0.0f;
            X86_MEMF32(p + 12) = t % 4 ? float(rng() % 12) : 100.0f;
            unsigned first = i * 3 + 1, last = std::min(first + 3, count);
            X86_MEM32(p + 0x24) = first < count;
            X86_MEM32(p + 0x28) = first < count ? nodes + first * 0x80 + 0x14 : p + 0x28;
            for (unsigned j = first; j < last; ++j)
                X86_MEM32(nodes + j * 0x80 + 0x14) = j + 1 < last ? nodes + (j + 1) * 0x80 + 0x14 : p + 0x28;
        }
        X86_MEM32(e + 4) = nodes;
        stop = t % 17;
        for (auto [p, n] : {std::pair{e - 0x2000, 0x2100u}, std::pair{object, 0x5000u}, std::pair{0x295C74u, 4u}})
            std::memcpy(b.data() + p, a.data() + p, n);
        X86Context x{};
        x.eax = rng();
        x.ecx = object;
        x.edx = rng();
        x.ebx = rng();
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
        calls.clear();
        __imp__sub_000CFE40(x, a.data());
        auto expected = calls;
        calls.clear();
        sub_000CFE40(y, b.data());
        callbacks += calls.size();
        bool same = expected == calls && !std::memcmp(&x, &y, sizeof x);
        for (auto [p, n] : {std::pair{e - 0x2000, 0x2100u}, std::pair{object, 0x5000u}})
            same &= !std::memcmp(a.data() + p, b.data() + p, n);
        if (!same) {
            std::printf("FAIL collision tree trial %u callbacks %zu/%zu\n", t, expected.size(), calls.size());
            for (unsigned i = 0; i < sizeof x / 4; ++i)
                if (((uint32_t *)&x)[i] != ((uint32_t *)&y)[i])
                    std::printf("ctx[%u] %08x/%08x\n", i, ((uint32_t *)&x)[i], ((uint32_t *)&y)[i]);
            for (unsigned p = e - 0x2000; p < e + 0x100; p += 4)
                if (std::memcmp(a.data() + p, b.data() + p, 4))
                    std::printf("stack %x %08x/%08x\n", p, *(uint32_t *)(a.data() + p), *(uint32_t *)(b.data() + p));
            return 1;
        }
    }
    if (callbacks < 10000)
        std::abort();
    std::printf("PASS 12000 collision trees, %llu callbacks: traversal/exit order, registers and guest stack\n",
                (unsigned long long)callbacks);
    if (argc > 1) {
        uint8_t *base = a.data();
        X86_MEM32(e + 4) = nodes;
        X86_MEMF32(nodes) = 0;
        X86_MEMF32(nodes + 4) = 0;
        X86_MEMF32(nodes + 8) = 0;
        X86_MEMF32(nodes + 12) = 100;
        stop = 0;
        for (unsigned i = 0; i < 40; ++i) {
            X86_MEMF32(nodes + i * 0x80) = 0;
            X86_MEMF32(nodes + i * 0x80 + 4) = 0;
            X86_MEMF32(nodes + i * 0x80 + 8) = 0;
            X86_MEMF32(nodes + i * 0x80 + 12) = 100;
        }
        double ns[2][5];
        for (unsigned r = 0; r < 5; ++r)
            for (unsigned step = 0; step < 2; ++step) {
                unsigned which = step ^ (r & 1);
                X86Context c{};
                c.mxcsr = 0x1f80;
                auto start = std::chrono::steady_clock::now();
                for (unsigned i = 0; i < 10000; ++i) {
                    c.esp = e;
                    c.ecx = object;
                    calls.clear();
                    (which ? sub_000CFE40 : baseline_collision_walk)(c, base);
                }
                ns[which][r] =
                    std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / 10000;
            }
        std::sort(ns[0], ns[0] + 5);
        std::sort(ns[1], ns[1] + 5);
        std::printf("BENCH collision tree previous-production %.2f native %.2f ns (host/qemu only)\n", ns[0][2],
                    ns[1][2]);
    }
}
