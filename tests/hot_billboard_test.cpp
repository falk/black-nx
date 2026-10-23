#include "cpu/x86_ops.h"
#include "game/hot_colour.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
X86_EXTERN_FUNC(sub_0015D320);
X86_EXTERN_FUNC(__imp__sub_0015D320);
int main(int argc, char **) {
    std::vector<uint8_t> a(1 << 24), b(1 << 24);
    std::mt19937 gen(41277);
    const uint32_t edges[] = {0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc12345,
                              0x7f812345, 0x4f000000, 0xcf000000, 0x4effffff, 0xcf000001};
    for (unsigned t = 0; t < 100000; ++t) {
        hot::f4 rgba;
        uint32_t packed = 0;
        const unsigned shifts[] = {16, 8, 0, 24};
        for (unsigned lane = 0; lane < 4; ++lane) {
            uint32_t bits = t < 40 ? edges[(t + lane) % 10] : gen();
            std::memcpy(&rgba[lane], &bits, 4);
            packed |= (uint32_t(sse_cvtt(rgba[lane])) & 255) << shifts[lane];
        }
        if (hot::PackColour(rgba) != packed) {
            std::fprintf(stderr, "particle colour mismatch at %u\n", t);
            return 1;
        }
    }
    auto put = [](auto &v, uint32_t p, uint32_t u) { std::memcpy(v.data() + p, &u, 4); };
    for (unsigned t = 0; t < 50000; ++t) {
        uint32_t e = 0xA00200 + (t % 4) * 4, index = gen() % 64, out = 0x53CAE8 + index * 32;
        if (t % 29 == 0)
            e = out + 32;
        const uint32_t s = ((e - 4) & ~15u) - 0x80;
        for (auto [lo, n] :
             {std::pair{e - 0x200, 0x300u}, std::pair{0x53CAE0u, 0x1000u}, std::pair{0x800000u, 0x1000u}})
            for (unsigned j = 0; j < n; j += 4)
                put(a, lo + j, gen());
        put(a, 0x295C74, 0x3F800000);
        put(a, 0x295CB4, 0x437F0000);
        uint32_t col = 0x800100, center = 0x800200, size = 0x800300, emitter = 0x800400;
        if (t % 11 == 0)
            center = out - 8;
        if (t % 13 == 0)
            col = s + 0x40;
        if (t % 17 == 0)
            size = s + 0x20;
        if (t % 19 == 0)
            emitter = s;
        if (t % 3 == 0) {
            for (unsigned j = 0; j < 4; j++) {
                float f = float(int(gen() % 2048) - 1024) / 64;
                put(a, col + j * 4, x86_u(f));
                put(a, center + j * 4, x86_u(f));
            }
            put(a, size, x86_u(3.5f));
            put(a, size + 4, x86_u(-0.0f));
            put(a, emitter + 0x20, x86_u(.5f));
        }
        put(a, e + 8, emitter);
        put(a, e + 0xC, index);
        put(a, e + 0x14, col);
        put(a, e + 0x18, center);
        put(a, e + 0x1C, size);
        for (auto [lo, n] : {std::pair{e - 0x200, 0x300u}, std::pair{0x53CAE0u, 0x1000u}, std::pair{0x800000u, 0x1000u},
                             std::pair{0x295C74u, 4u}, std::pair{0x295CB4u, 4u}})
            std::memcpy(b.data() + lo, a.data() + lo, n);
        X86Context x{};
        x.esp = e;
        x.eax = gen();
        x.ecx = gen();
        x.edx = gen();
        x.ebx = gen();
        x.esi = gen();
        x.edi = gen();
        x.ebp = gen();
        x.fcw = 0x37f;
        x.mxcsr = 0x1f80;
        for (auto &v : x.xmm)
            for (auto &u : v.u)
                u = gen();
        X86Context y = x;
        sub_0015D320(x, a.data());
        __imp__sub_0015D320(y, b.data());
        bool regs = !std::memcmp(&x, &y, sizeof x);
        bool mem = !std::memcmp(a.data() + e - 0x200, b.data() + e - 0x200, 0x300) &&
                   !std::memcmp(a.data() + 0x53CAE0, b.data() + 0x53CAE0, 0x1000);
        if (!regs || !mem) {
            std::printf("FAIL trial %u regs %d mem %d\n", t, regs, mem);
            for (unsigned j = 0; j < sizeof x / 4; j++)
                if (((uint32_t *)&x)[j] != ((uint32_t *)&y)[j])
                    std::printf("ctx word %u %08x %08x\n", j, ((uint32_t *)&x)[j], ((uint32_t *)&y)[j]);
            for (unsigned j = e - 0x200; j < e + 0x100; j += 4)
                if (std::memcmp(a.data() + j, b.data() + j, 4))
                    std::printf("mem %08x %08x %08x\n", j, *(uint32_t *)(a.data() + j), *(uint32_t *)(b.data() + j));
            return 1;
        }
    }
    puts("PASS 50000 particle trials: complete context, scratch, vertices; random bits and alias fallbacks");
    if (argc > 1) {
        uint8_t *base = a.data();
        const uint32_t e = 0xA00200;
        X86_MEM32(e + 8) = 0x800400;
        X86_MEM32(e + 0xC) = 0;
        X86_MEM32(e + 0x14) = 0x800100;
        X86_MEM32(e + 0x18) = 0x800200;
        X86_MEM32(e + 0x1C) = 0x800300;
        for (unsigned i = 0; i < 16; i++) {
            X86_MEM32(0x800100 + i * 4) = x86_u(.2f);
            X86_MEM32(0x800200 + i * 4) = x86_u(0.0f);
            X86_MEM32(0x800300 + i * 4) = x86_u(.25f);
        }
        for (auto fn : {__imp__sub_0015D320, sub_0015D320}) {
            X86Context c{};
            c.mxcsr = 0x1f80;
            auto start = std::chrono::steady_clock::now();
            for (unsigned i = 0; i < 2000000; i++) {
                c.esp = e;
                fn(c, base);
            }
            double ns =
                std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / 2000000;
            printf("%s %.2f ns/call\n", fn == sub_0015D320 ? "native" : "original", ns);
        }
    }
}
