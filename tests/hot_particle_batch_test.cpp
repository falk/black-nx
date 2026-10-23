// Compare full particle batches, including builder dispatch and outgoing draw
// arguments, against the generated loop. The GPU submission is the only stub.
#include "cpu/x86_ops.h"
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
X86_EXTERN_FUNC(sub_0015B380);
X86_EXTERN_FUNC(__imp__sub_0015B380);
X86_EXTERN_FUNC(sub_0015D320);
X86_EXTERN_FUNC(__imp__sub_0015D320);
X86_EXTERN_FUNC(sub_0015BF30);
X86_EXTERN_FUNC(__imp__sub_0015BF30);
X86_EXTERN_FUNC(__imp__sub_00011DD0);
X86Func* const* x86_icall_table = nullptr;
uint32_t x86_icall_lo = 0, x86_icall_count = 0;
static bool reference;
static std::vector<std::array<uint32_t, 4>> draws;
void x86_icall(X86Context& ctx, uint8_t* base, uint32_t target) {
    ctx.last_icall = target;
    if (target == 0x15D320) (reference ? __imp__sub_0015D320 : sub_0015D320)(ctx, base);
    else if (target == 0x15BF30) (reference ? __imp__sub_0015BF30 : sub_0015BF30)(ctx, base);
    else std::abort();
}
X86_FUNC(sub_00011DD0) { __imp__sub_00011DD0(ctx, base); }
X86_FUNC(sub_0002AC00) {
    draws.push_back({X86_MEM32(ctx.esp + 4), X86_MEM32(ctx.esp + 8),
                     X86_MEM32(ctx.esp + 12), X86_MEM32(ctx.esp + 16)});
    ctx.esp += 4; // return; caller pops the four draw arguments
}
int main(int argc, char**) {
    constexpr uint32_t e = 0xA00200, batch = 0x800000, emitter = 0x800100, view = 0x800400, parts = 0x801000;
    std::vector<uint8_t> a(1 << 24), b(a.size());
    std::mt19937 rng(93817);
    uint64_t built = 0;
    for (unsigned t = 0; t < 2000; ++t) {
        uint8_t* base = a.data();
        std::memset(base + 0x800000, 0, 0x4000);
        for (unsigned j = e - 0x600; j < e + 0x100; j += 4) X86_MEM32(j) = rng();
        X86_MEM32(e + 4) = batch; X86_MEM32(e + 8) = emitter; X86_MEM32(e + 12) = view;
        const unsigned count = t % 65;
        X86_MEM32(batch + 8) = count; X86_MEM32(batch + 12) = parts;
        X86_MEM32(emitter + 0x64) = t & 1;
        X86_MEM32(0x289DF0) = 0x15D320; X86_MEM32(0x289DF4) = 0x15BF30;
        X86_MEMF32(0x295C74) = 1; X86_MEMF32(0x295CB4) = 255; X86_MEMF32(0x2F81B0) = 0;
        X86_MEMF32(emitter + 0x70) = 10; X86_MEMF32(emitter + 0x74) = .5f;
        X86_MEMF32(emitter + 0x7C) = 1; X86_MEMF32(emitter + 0x84) = 1;
        X86_MEMF32(emitter + 0xA4) = .5f; X86_MEMF32(emitter + 0xA8) = .6f; X86_MEMF32(emitter + 0xAC) = .7f;
        for (unsigned k = 0; k < 3; ++k)
            for (unsigned l = 0; l < 4; ++l) X86_MEMF32(emitter + k * 16 + l * 4) = .25f * (k + 1);
        X86_MEMF32(view + 0x44) = 100; X86_MEMF32(view + 0x48) = .5f; X86_MEMF32(view + 0x4C) = 1.f / 98;
        X86_MEMF32(view + 0x50) = 2; X86_MEMF32(view + 0x54) = 2;
        X86_MEMF32(view + 0x60) = 1; X86_MEMF32(view + 0x64) = 1;
        X86_MEMF32(view + 0x68) = 10; X86_MEMF32(view + 0x6C) = 10;
        X86_MEMF32(view + 0x140) = 1 + float(t % 8);
        for (unsigned l = 0; l < 4; ++l) {
            X86_MEMF32(view + 0x80 + l * 4) = 1;
            X86_MEMF32(view + 0xF0 + l * 20) = 1;
        }
        const uint32_t s = ((e - 4) & ~15u) - 0x120;
        X86_MEMF32(s + 0x18) = 2;
        for (unsigned i = 0; i < count; ++i) {
            const uint32_t p = parts + i * 48;
            X86_MEMF32(p) = float(int(rng() % 64) - 32) / 64;
            X86_MEMF32(p + 4) = float(int(rng() % 64) - 32) / 64;
            X86_MEMF32(p + 8) = (t % 7 == 0 && i % 3 == 0) ? 200 : 2;
            X86_MEMF32(p + 12) = float(i % 4);
            X86_MEMF32(p + 0x1C) = .1f;
            X86_MEMF32(p + 0x20) = .75f; X86_MEMF32(p + 0x24) = .1f;
            X86_MEMF32(p + 0x28) = .5f; X86_MEMF32(p + 0x2C) = 1;
        }
        for (auto [lo, n] : {std::pair{0x289DF0u, 8u}, std::pair{0x295C74u, 0x44u}, std::pair{0x2F81B0u, 4u},
                            std::pair{0x53CAE0u, 0x8000u}, std::pair{0x800000u, 0x4000u}, std::pair{e - 0x600, 0x700u}})
            std::memcpy(b.data() + lo, a.data() + lo, n);
        X86Context x{}; x.esp = e; x.fcw = 0x37F; x.mxcsr = 0x1F80;
        for (auto& reg : x.xmm) for (auto& lane : reg.u) lane = rng();
        X86Context y = x;
        reference = false; draws.clear(); sub_0015B380(x, a.data()); const auto mine = draws;
        reference = true; draws.clear(); __imp__sub_0015B380(y, b.data());
        bool same = !std::memcmp(&x, &y, offsetof(X86Context, cf)) && !std::memcmp(x.xmm, y.xmm, sizeof x.xmm) &&
                    x.top == y.top && x.fsw == y.fsw && !std::memcmp(x.st, y.st, sizeof x.st) && mine == draws;
        same &= !std::memcmp(a.data() + e - 0x600, b.data() + e - 0x600, 0x700);
        same &= !std::memcmp(a.data() + 0x53CAE0, b.data() + 0x53CAE0, 0x8000);
        if (!same) {
            std::printf("FAIL batch trial %u, draws %zu/%zu\n", t, mine.size(), draws.size());
            for (unsigned i = 0; i < sizeof x / 4; ++i)
                if (std::memcmp((char*)&x + i*4, (char*)&y + i*4, 4))
                    std::printf("ctx[%u] %08X/%08X\n", i, ((uint32_t*)&x)[i], ((uint32_t*)&y)[i]);
            for (unsigned i = e - 0x600; i < e + 0x100; i += 4)
                if (std::memcmp(a.data()+i, b.data()+i, 4))
                    std::printf("scratch[%08X] %08X/%08X\n", i, *(uint32_t*)(a.data()+i), *(uint32_t*)(b.data()+i));
            for (const auto& d : mine) std::printf("native draw %X %X %X %X\n", d[0],d[1],d[2],d[3]);
            for (const auto& d : draws) std::printf("original draw %X %X %X %X\n", d[0],d[1],d[2],d[3]);
            return 1;
        }
        for (const auto& d : draws) built += d[1] / 4;
    }
    if (built < 1000) { puts("FAIL insufficient builder coverage"); return 1; }
    std::printf("PASS 2000 particle batches, %llu built particles: registers, x87, scratch, vertices and draw arguments\n", (unsigned long long)built);
    if (argc > 1) {
        double ns[2][5];
        for (unsigned r = 0; r < 5; ++r) for (unsigned step = 0; step < 2; ++step) {
            const unsigned which = step ^ (r & 1);
            reference = !which;
            X86Context ctx{}; ctx.fcw = 0x37f; ctx.mxcsr = 0x1f80;
            auto start = std::chrono::steady_clock::now();
            for (unsigned i = 0; i < 2000; ++i) {
                ctx.esp = e;
                draws.clear();
                (which ? sub_0015B380 : __imp__sub_0015B380)(ctx, a.data());
            }
            ns[which][r] = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / 2000;
        }
        std::sort(ns[0], ns[0] + 5); std::sort(ns[1], ns[1] + 5);
        std::printf("BENCH particle batch original %.2f native %.2f ns/batch (host/qemu only)\n", ns[0][2], ns[1][2]);
    }

}
