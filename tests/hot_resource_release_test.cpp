#include "d3d/resource_refs.h"
#include "xdk/hle.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
X86_EXTERN_FUNC(__imp__sub_0026C410);
namespace kernel {
uint8_t* g_base;
}
static std::vector<uint32_t> freed;
void x86_trap(X86Context&, uint32_t, const char*) { std::abort(); }
void x86_icall(X86Context& ctx, uint8_t* base, uint32_t target) {
    ctx.last_icall = target;
    if (target != 0x26C410)
        std::abort();
    __imp__sub_0026C410(ctx, base);
}
X86_FUNC(sub_0026C360) {
    freed.push_back(ctx.edi);
    X86_MEM32(ctx.edi) = 0xface1234;
    ctx.eax = 0x12345678;
    ctx.ecx = 0x87654321;
    ctx.xmm[0].u[0] ^= 0xff;
    ctx.esp += 4;
}
static void Original(X86Context& ctx, uint8_t* base, uint32_t resource) {
    kernel::g_base = base;
    hle::Call(ctx, 0x26C410, hle::Make({hle::esi}), {resource});
}
static void Native(X86Context& ctx, uint8_t* base, uint32_t resource) {
    if (!d3d::TryReleaseHeld(ctx, base, resource, 0x26C410))
        Original(ctx, base, resource);
}
int main(int argc, char**) {
    std::mt19937 rng(26410);
    std::vector<uint8_t> a(1 << 24), b(a.size());
    constexpr uint32_t e = 0xA00200, p = 0x800000, parent = 0x800100;
    unsigned fast = 0, slow = 0;
    for (unsigned t = 0; t < 100000; ++t) {
        uint8_t* base = a.data();
        for (auto [lo, n] : {std::pair{e - 0x100, 0x200u}, std::pair{p, 0x200u}})
            for (unsigned j = 0; j < n; j += 4)
                X86_MEM32(lo + j) = rng();
        uint32_t resource = t % 19 == 0 ? e - 4 : t % 23 == 0 ? e - 8 : p;
        uint32_t common = (rng() & 0xff800000u) | ((rng() % 5) << 19) | ((rng() % 8) << 16) | (t % 7 ? rng() % 4 : 0);
        X86_MEM32(resource) = common;
        X86_MEM32(resource + 0x14) = t & 1 ? parent : 0;
        X86_MEM32(parent) = 0x50000u | ((rng() % 5) << 19) | (t % 3 ? rng() % 4 : 0);
        if (t % 29 == 0)
            X86_MEM32(resource + 0x14) = resource;
        for (auto [lo, n] : {std::pair{e - 0x100, 0x200u}, std::pair{p, 0x200u}})
            std::memcpy(b.data() + lo, a.data() + lo, n);
        X86Context x{};
        x.eax = rng();
        x.ecx = rng();
        x.edx = rng();
        x.ebx = rng();
        x.esi = rng();
        x.edi = rng();
        x.ebp = rng();
        x.esp = e;
        for (auto& v : x.xmm)
            for (auto& u : v.u)
                u = rng();
        x.fcw = 0x37f;
        x.mxcsr = 0x1f80;
        auto y = x;
        freed.clear();
        Original(x, a.data(), resource);
        auto expected = freed;
        freed.clear();
        bool used = d3d::TryReleaseHeld(y, b.data(), resource, 0x26C410);
        if (used)
            ++fast;
        else {
            ++slow;
            Original(y, b.data(), resource);
        }
        bool same = expected == freed && !std::memcmp(&x, &y, sizeof x);
        for (auto [lo, n] : {std::pair{e - 0x100, 0x200u}, std::pair{p, 0x200u}})
            same &= !std::memcmp(a.data() + lo, b.data() + lo, n);
        if (!same) {
            std::printf("FAIL resource trial %u common %08x fast %d\n", t, common, used);
            return 1;
        }
    }
    std::printf(
        "PASS 100000 resource releases: %u fast/%u fallback, full context/stack, parent and destruction order\n", fast,
        slow);
    if (argc > 1) {
        double ns[2][5];
        uint8_t* base = a.data();
        for (unsigned r = 0; r < 5; ++r)
            for (unsigned step = 0; step < 2; ++step) {
                unsigned which = step ^ (r & 1);
                X86Context c{};
                c.esp = e;
                auto start = std::chrono::steady_clock::now();
                for (unsigned i = 0; i < 100000; ++i) {
                    X86_MEM32(p) = 0x140001;
                    (which ? Native : Original)(c, base, p);
                }
                ns[which][r] =
                    std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / 100000;
            }
        std::sort(ns[0], ns[0] + 5);
        std::sort(ns[1], ns[1] + 5);
        std::printf("BENCH resource release generic %.2f held fast path %.2f ns (host/qemu only)\n", ns[0][2],
                    ns[1][2]);
    }
}
