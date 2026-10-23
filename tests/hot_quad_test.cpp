// Exercise both upright and spinning particle builders against the generated
// originals, including the normalization call, guest frame aliases and statics.
#include "cpu/x86_ops.h"
#include <cstdio>
#include <random>
#include <vector>
X86_EXTERN_FUNC(sub_0015CFF0);
X86_EXTERN_FUNC(__imp__sub_0015CFF0);
X86_EXTERN_FUNC(sub_0015C620);
X86_EXTERN_FUNC(__imp__sub_0015C620);
int main() {
    std::vector<uint8_t> a(1 << 24), b(a.size());
    std::mt19937 rng(501721);
    for (unsigned kind = 0; kind < 2; ++kind) for (unsigned t = 0; t < 20000; ++t) {
        const uint32_t e = 0xA00200 + (t % 4) * 4, index = rng() % 16;
        const uint32_t s = ((e - 4) & ~15u) - (kind ? 0xA0 : 0x90);
        uint8_t* base = a.data();
        for (auto [lo, n] : {std::pair{e - 0x300, 0x400u}, std::pair{0x800000u, 0x1000u},
                            std::pair{0x53CAE0u, 0x1000u}, std::pair{0x565600u, 0x100u},
                            std::pair{0x295C00u, 0x100u}, std::pair{0x288100u, 0x100u}})
            for (unsigned j = 0; j < n; j += 4) {
                uint32_t bits = rng();
                if (t % 3) bits = x86_u(float(int(rng() % 1024) - 512) / 256);
                X86_MEM32(lo + j) = bits;
            }
        uint32_t obj = 0x800000, emitter = 0x800200, up = 0x800300, centre = 0x800400, uv = 0x800500;
        if (t % 11 == 0) centre = 0x53CAE0 + index * 32;
        if (t % 13 == 0) up = s + 0x20;
        if (t % 17 == 0) emitter = s + 0x14;
        if (t % 19 == 0) uv = e - 16;
        X86_MEM32(e + 4) = obj; X86_MEM32(e + 8) = emitter; X86_MEM32(e + 12) = index;
        X86_MEM32(e + 16) = t % 3 ? x86_u(.5f) : rng();
        X86_MEM32(e + 20) = up; X86_MEM32(e + 24) = centre; X86_MEM32(e + 28) = uv;
        X86_MEM32(0x565660) = t & 1; X86_MEM32(0x565680) = t & 1;
        X86_MEMF32(0x295C70) = .5f; X86_MEMF32(0x295C74) = 1;
        X86_MEMF32(0x295C8C) = .5f; X86_MEMF32(0x295CB4) = 255; X86_MEMF32(0x2881C8) = 1;
        for (auto [lo, n] : {std::pair{e - 0x300, 0x400u}, std::pair{0x800000u, 0x1000u},
                            std::pair{0x53CAE0u, 0x1000u}, std::pair{0x565600u, 0x100u},
                            std::pair{0x295C00u, 0x100u}, std::pair{0x288100u, 0x100u}})
            std::memcpy(b.data() + lo, a.data() + lo, n);
        X86Context x{};
        x.eax = rng(); x.ebx = rng(); x.ecx = rng(); x.edx = rng(); x.esi = rng(); x.edi = rng(); x.ebp = rng(); x.esp = e;
        x.fcw = 0x37F; x.mxcsr = 0x1F80; x.top = rng() & 7; x.fsw = rng();
        for (auto& v : x.xmm) for (auto& u : v.u) u = rng();
        for (auto& v : x.st) v = double(int(rng()));
        X86Context y = x;
        (kind ? __imp__sub_0015C620 : __imp__sub_0015CFF0)(x, a.data());
        (kind ? sub_0015C620 : sub_0015CFF0)(y, b.data());
        bool same = std::memcmp(&x, &y, sizeof x) == 0;
        for (auto [lo, n] : {std::pair{e - 0x300, 0x400u}, std::pair{0x800000u, 0x1000u},
                            std::pair{0x53CAE0u, 0x1000u}, std::pair{0x565600u, 0x100u}})
            same &= std::memcmp(a.data() + lo, b.data() + lo, n) == 0;
        if (!same) {
            std::fprintf(stderr, "particle quad kind %u mismatch trial %u\n", kind, t);
            auto* xp = reinterpret_cast<uint8_t*>(&x); auto* yp = reinterpret_cast<uint8_t*>(&y);
            for (unsigned i = 0; i < sizeof x; ++i) if (xp[i] != yp[i])
                std::fprintf(stderr, "ctx[%u] %02x %02x\n", i, xp[i], yp[i]);
            for (uint32_t i = e - 0x300; i < e + 0x100; ++i) if (a[i] != b[i])
                std::fprintf(stderr, "frame[%x] %02x %02x\n", i, a[i], b[i]);
            return 1;
        }
    }
    std::puts("PASS 40000 upright/spinning particle quads: full context, frame, statics and vertices; raw bits and aliases");
}
