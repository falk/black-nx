#include "cpu/x86_ops.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
X86_EXTERN_FUNC(sub_000D1D10);
X86_EXTERN_FUNC(__imp__sub_000D1D10);
int main(int argc, char**) {
    std::vector<uint8_t> a(1 << 24), b(a.size());
    std::mt19937 rng(0xd1d10);
    constexpr uint32_t mesh = 0x800000, indices = mesh + 0x100, vertices = mesh + 0x1000;
    constexpr uint32_t ray = mesh + 0x2000, result = mesh + 0x2100;
    uint64_t hits = 0;
    for (unsigned t = 0; t < 20000; ++t) {
        uint8_t* base = a.data();
        const uint32_t e = 0xA00200 + (t % 4) * 4;
        for (auto [lo, n] : {std::pair{e - 0x400, 0x500u}, std::pair{mesh, 0x3000u}})
            for (unsigned j = 0; j < n; j += 4)
                X86_MEM32(lo + j) = rng();
        X86_MEM32(mesh) = indices;
        X86_MEM32(mesh + 4) = vertices;
        X86_MEM8(mesh + 10) = X86_MEM8(mesh + 11) = X86_MEM8(mesh + 12) = 0;
        X86_MEM8(mesh + 13) = t % 29 ? 4 + t % 4 : t % 4;
        X86_MEM8(mesh + 14) = t % 25;
        for (unsigned j = 0; j < 256 * 3; ++j)
            X86_MEM16(vertices + j * 2) = rng() % 65536;
        for (unsigned j = 0; j < t % 25; ++j) {
            uint32_t p = indices + j * X86_MEM8(mesh + 13);
            for (unsigned k = 0; k < 3; ++k)
                X86_MEM8(p + k) = rng() % 256;
            X86_MEM8(p + 3) = t % 3 ? 255 : rng() % 255;
        }
        for (unsigned j = 0; j < 8; ++j)
            X86_MEMF32(ray + j * 4) = j % 4 == 3 ? 0 : float(int(rng() % 2400) - 1200);
        if (t % 13 == 0)
            for (unsigned j = 0; j < 8; ++j)
                X86_MEM32(ray + j * 4) = rng();
        if (t % 11 == 0)
            for (unsigned j = 10; j < 13; ++j)
                X86_MEM8(mesh + j) = rng();
        X86_MEMF32(result + 0x50) = t % 7 ? 1.0f : 0.0f;
        if (t % 17 == 0)
            X86_MEM32(result + 0x50) = 0x7fc12345;
        const uint32_t query = t % 19 == 0 ? (((e - 4) & ~15u) - 0x1c0) : ray;
        if (query != ray)
            std::memcpy(base + query, base + ray, 32);
        X86_MEMF32(0x295D80) = 65536;
        X86_MEMF32(0x295D84) = 500;
        X86_MEMF32(0x295D8C) = 1000;
        X86_MEMF32(0x28B534) = 0.000001f;
        X86_MEMF32(0x295C74) = 1;
        X86_MEMF32(0x295C88) = -0.001f;
        X86_MEMF32(0x295E90) = 1.001f;
        X86_MEM32(e + 4) = query;
        X86_MEM32(e + 8) = result;
        X86Context x{};
        x.eax = mesh;
        x.esp = e;
        x.ebp = rng();
        x.esi = rng();
        x.edi = rng();
        x.ebx = rng();
        x.ecx = rng();
        x.edx = rng();
        x.fcw = 0x37f;
        x.mxcsr = 0x1f80;
        for (auto& v : x.xmm)
            for (auto& u : v.u)
                u = rng();
        for (auto [lo, n] : {std::pair{e - 0x400, 0x500u}, std::pair{mesh, 0x3000u}, std::pair{0x280000u, 0x20000u}})
            std::memcpy(b.data() + lo, a.data() + lo, n);
        auto y = x;
        __imp__sub_000D1D10(x, a.data());
        sub_000D1D10(y, b.data());
        hits += x.eax != 0;
        bool same = !std::memcmp(&x, &y, sizeof x);
        for (auto [lo, n] : {std::pair{e - 0x400, 0x500u}, std::pair{mesh, 0x3000u}})
            same &= !std::memcmp(a.data() + lo, b.data() + lo, n);
        if (!same) {
            std::printf("FAIL mesh ray %u hit %u/%u\n", t, x.eax, y.eax);
            for (unsigned j = 0; j < sizeof x / 4; ++j)
                if (((uint32_t*)&x)[j] != ((uint32_t*)&y)[j])
                    std::printf("ctx %u %08x/%08x\n", j, ((uint32_t*)&x)[j], ((uint32_t*)&y)[j]);
            for (unsigned p = e - 0x400; p < e + 0x100; p += 4)
                if (std::memcmp(a.data() + p, b.data() + p, 4))
                    std::printf("stack %x %08x/%08x\n", p, *(uint32_t*)(a.data() + p), *(uint32_t*)(b.data() + p));
            return 1;
        }
    }
    if (argc > 1) {
        uint8_t* base = a.data();
        constexpr uint32_t e = 0xA00200;
        X86_MEM8(mesh + 13) = 4;
        X86_MEM8(mesh + 14) = 24;
        X86_MEM8(mesh + 10) = X86_MEM8(mesh + 11) = X86_MEM8(mesh + 12) = 0;
        X86_MEM32(mesh) = indices;
        X86_MEM32(mesh + 4) = vertices;
        X86_MEM32(e + 4) = ray;
        X86_MEM32(e + 8) = result;
        for (unsigned j = 0; j < 8; ++j)
            X86_MEMF32(ray + j * 4) = j % 4 == 3 ? 0 : float(int(rng() % 2400) - 1200);
        for (unsigned quad : {0u, 1u}) {
            for (unsigned j = 0; j < 24; ++j) {
                for (unsigned k = 0; k < 3; ++k)
                    X86_MEM8(indices + j * 4 + k) = rng() % 32;
                X86_MEM8(indices + j * 4 + 3) = quad ? rng() % 32 : 255;
            }
            double ns[2][5];
            for (unsigned r = 0; r < 5; ++r)
                for (unsigned step = 0; step < 2; ++step) {
                    unsigned which = step ^ (r & 1);
                    X86Context c{};
                    c.mxcsr = 0x1f80;
                    c.fcw = 0x37f;
                    auto start = std::chrono::steady_clock::now();
                    for (unsigned j = 0; j < 10000; ++j) {
                        c.eax = mesh;
                        c.esp = e;
                        X86_MEMF32(result + 0x50) = 1;
                        (which ? sub_000D1D10 : __imp__sub_000D1D10)(c, base);
                    }
                    ns[which][r] =
                        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() /
                        10000;
                }
            std::sort(ns[0], ns[0] + 5);
            std::sort(ns[1], ns[1] + 5);
            std::printf("BENCH mesh ray %s: current %.2f fused %.2f ns/query (host/qemu only)\n",
                        quad ? "quads" : "triangles", ns[0][2], ns[1][2]);
        }
    }
    std::printf("PASS 20000 mesh ray queries, %llu hits: full context, scratch and result records\n",
                (unsigned long long)hits);
}
