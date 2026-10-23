#include "cpu/x86_ops.h"
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <random>
#include <vector>
X86_EXTERN_FUNC(sub_000205A0);
X86_EXTERN_FUNC(__imp__sub_000205A0);
int main(int argc, char**) {
    std::mt19937 rng(205);
    std::vector<uint8_t> a(1 << 24), b(a.size());
    for (unsigned t = 0; t < 100000; ++t) {
        uint8_t* base = a.data();
        const uint32_t e = 0xA00200 + (t % 4) * 4, s = ((e - 4) & ~15u) - 0x20;
        uint32_t mesh = 0x800000, data = 0x800100, output = 0x800200;
        for (auto [lo, n] : {std::pair{e - 0x100, 0x200u}, std::pair{0x800000u, 0x400u}})
            for (unsigned j = 0; j < n; j += 4) X86_MEM32(lo + j) = rng();
        if (t % 11 == 0) mesh = s;
        if (t % 13 == 0) data = s + 16;
        if (t % 17 == 0) output = s + 8;
        if (t % 19 == 0) output = mesh;
        X86_MEM32(mesh + 4) = data;
        const uint32_t edges[] = {0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x00800000,
                                 0x807fffff, 0x80800000, 0x7f800000, 0xff800000, 0x7fc12345, 0x7f812345};
        X86_MEM32(s + 28) = t < 120 ? edges[t % 12] : t % 3 ? rng() & 0x807fffff : rng();
        X86_MEMF32(0x295D80) = 65536; X86_MEMF32(0x295D84) = 500; X86_MEMF32(0x295D8C) = 1000;
        for (auto [lo, n] : {std::pair{e - 0x100, 0x200u}, std::pair{0x800000u, 0x400u}, std::pair{0x295D80u, 16u}})
            std::memcpy(b.data() + lo, a.data() + lo, n);
        X86Context x{}; x.eax = output; x.ecx = mesh; x.edx = rng() % 32; x.esi = rng(); x.ebp = rng(); x.esp = e;
        x.fcw = 0x37f; x.mxcsr = 0x1f80;
        for (auto& v : x.xmm) for (auto& u : v.u) u = rng();
        X86Context y = x;
        __imp__sub_000205A0(x, a.data()); sub_000205A0(y, b.data());
        bool same = !std::memcmp(&x, &y, sizeof x);
        for (auto [lo, n] : {std::pair{e - 0x100, 0x200u}, std::pair{0x800000u, 0x400u}})
            same &= !std::memcmp(a.data() + lo, b.data() + lo, n);
        if (!same) {
            std::fprintf(stderr, "decode mismatch trial %u\n", t);
            const auto* xp = reinterpret_cast<uint8_t*>(&x); const auto* yp = reinterpret_cast<uint8_t*>(&y);
            for (unsigned i = 0; i < sizeof x; ++i) if (xp[i] != yp[i])
                std::fprintf(stderr, "ctx[%u] %02x %02x\n", i, xp[i], yp[i]);
            return 1;
        }
    }
    std::puts("PASS 100000 decoded positions: full context/scratch, subnormals, raw bits and frame aliases");
    if (argc > 1) {
        constexpr uint32_t e = 0xA00200, s = ((e - 4) & ~15u) - 0x20;
        uint8_t* base = a.data();
        X86_MEM32(0x800004) = 0x800100;
        for (uint32_t w : {0u, 1u, 0x7fffffu, 0x3f800000u}) {
            X86_MEM32(s + 28) = w;
            double ns[2][5];
            for (unsigned r = 0; r < 5; ++r) for (unsigned step = 0; step < 2; ++step) {
                unsigned which = step ^ (r & 1);
                X86Context ctx{}; ctx.fcw = 0x37f; ctx.mxcsr = 0x1f80;
                auto start = std::chrono::steady_clock::now();
                for (unsigned i = 0; i < 100000; ++i) {
                    ctx.esp = e; ctx.eax = 0x800200; ctx.ecx = 0x800000; ctx.edx = 0;
                    (which ? sub_000205A0 : __imp__sub_000205A0)(ctx, base);
                }
                ns[which][r] = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / 100000;
            }
            std::sort(ns[0], ns[0] + 5); std::sort(ns[1], ns[1] + 5);
            std::printf("BENCH decode w=%08x original %.2f native %.2f ns/call (host/qemu only)\n", w, ns[0][2], ns[1][2]);
        }
    }

}
