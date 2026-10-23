// Native replacement of the sphere test the scene's culling calls for every object (sub_000D0510)
//
// eax points at a sphere (centre x, y, z, radius w), esi at the view volume: two planes (normal x, y, z, distance w)
// at esi and esi + 0x10, then three rows at esi + 0x20, + 0x30, + 0x40 and an offset at + 0x50 that take the centre
// to four more plane distances. eax returns 0 when the sphere is outside a plane, 1 when it crosses one, 2 when it is
// inside all of them. The rewrite keeps the original's operand order, its stores (ebp, edi, the centre and later the
// four distances at [esp+0x10], the last scalar at [esp+0x0C]) and the registers and xmm it leaves on each way out.
// Build with BLACK_VERIFY_HOT to run the original after every call and count the calls where anything differs.
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "game/hot_ordered.h"
#include "game/hot_verify.h"

using namespace hot;

X86_EXTERN_FUNC(__imp__sub_000D0510);

namespace {

#ifdef BLACK_VERIFY_HOT
PathCounts g_cull_paths{{}, {"plane 0 out", "plane 1 out", "box lane 0 out", "box lane 1 out", "box lane 2 out",
                              "box lane 3 out", "crossing", "inside", "frame overlap"}};
#endif

inline void StF(uint8_t* p, float f) { std::memcpy(p, &f, 4); }
inline void StU(uint8_t* p, uint32_t u) { std::memcpy(p, &u, 4); }
inline float LdF(const uint8_t* p) {
    float f;
    std::memcpy(&f, p, 4);
    return f;
}
inline bool Overlap(uint32_t a, uint64_t an, uint32_t b, uint64_t bn) {
    return uint64_t(a) < uint64_t(b) + bn && uint64_t(b) < uint64_t(a) + an;
}

// the epilogue every way out shares: pop edi, mov esp, ebp, pop ebp, ret
inline void Leave(X86Context& __restrict ctx, uint8_t* fp, uint32_t e, uint32_t edi, uint32_t ebp) {
    (void)fp;
    ctx.edi = edi;
    ctx.ebp = ebp;
    ctx.esp = e + 4;
}

static void SphereInView(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp;
    const uint32_t s = ((e - 4) & ~15u) - 0x20;  // esp after push ebp, the alignment, sub esp, 0x1C and push edi
    const uint32_t a = ctx.eax, pl = ctx.esi;
    // the planes read after the frame's stores: a view volume inside the frame would see them
    if (Overlap(pl, 0x60, s, e - s)) {
        HOT_PATH(g_cull_paths, 8);
        __imp__sub_000D0510(ctx, base);
        return;
    }
    const uint32_t ebp = ctx.ebp, edi = ctx.edi;
    uint8_t* fp = base + s;
    const f4 p = Load4(base, a);
    const float r = F32(base, a + 0xC);
    X86_MEM32(e - 4) = ebp;
    StU(fp, edi);
    Store4(base, s + 0x10, p);
    const float nr = 0.0f - r;  // subss from a zeroed xmm5
    const f4 zero = Splat(0.0f);
    PutXmm(ctx.xmm[2], p);
    PutXmm(ctx.xmm[3], zero);
    PutXmm(ctx.xmm[4], Scalar(r));
    PutXmm(ctx.xmm[5], Scalar(nr));
    uint32_t result = 2;
    f4 x6 = zero;
    for (uint32_t i = 0; i < 2; i++) {
        const uint32_t plane = pl + 16 * i;
        const f4 m = VMul(p, Load4(base, plane));
        const f4 x1 = Sh<0x39>(m, m);
        x6 = Sh<0x39>(x1, x1);
        const float dot = SAdd(SAdd(m[0], x1[0]), x6[0]);
        StF(fp + 0xC, dot);
        const float dist = dot - F32(base, plane + 0xC);
        if (Jnbe(dist, r)) {
            HOT_PATH(g_cull_paths, i);
            PutXmm(ctx.xmm[0], Scalar(dist));
            PutXmm(ctx.xmm[1], x1);
            PutXmm(ctx.xmm[6], x6);
            ctx.eax = 0;
            ctx.ecx = i;
            ctx.edx = plane;
            Leave(ctx, fp, e, edi, ebp);
            return;
        }
        if (Jnbe(dist, nr)) result = 1;
        if (i == 1) PutXmm(ctx.xmm[1], x1);
    }
    PutXmm(ctx.xmm[6], x6);
    // the centre back from the frame, one lane at a time through [esp+0x0C], times the rows
    const float pz = LdF(fp + 0x18), py = LdF(fp + 0x14), px = LdF(fp + 0x10);
    const f4 r2 = Load4(base, pl + 0x40), r1 = Load4(base, pl + 0x30);
    StF(fp + 0xC, pz);
    const f4 x1 = VMulS(r2, pz);
    StF(fp + 0xC, py);
    const f4 x2 = VMulS(r1, py);
    StF(fp + 0xC, px);
    const f4 r0 = Load4(base, pl + 0x20);
    f4 x0 = VMulS(r0, px);
    x0 = VAdd(x0, x2);
    x0 = VAdd(x0, x1);
    const f4 r3 = Load4(base, pl + 0x50);
    const f4 q = VSub(x0, r3);
    Store4(base, s + 0x10, q);
    ctx.ecx = s + 0xC;
    ctx.edx = s + 0xC;
    PutXmm(ctx.xmm[0], Scalar(q[0]));
    PutXmm(ctx.xmm[1], r3);
    PutXmm(ctx.xmm[2], x2);
    PutXmm(ctx.xmm[3], Splat(px));
    auto out = [&](int lane) {
        (void)lane;
        HOT_PATH(g_cull_paths, 2 + lane);
        ctx.eax = 0;
        Leave(ctx, fp, e, edi, ebp);
    };
    if (Jnbe(q[0], r)) return out(0);
    PutXmm(ctx.xmm[3], Scalar(q[1]));
    if (Jnbe(q[1], r)) return out(1);
    PutXmm(ctx.xmm[2], Scalar(q[2]));
    if (Jnbe(q[2], r)) return out(2);
    PutXmm(ctx.xmm[1], Scalar(q[3]));
    if (Jnbe(q[3], r)) return out(3);
    if (Jnbe(q[0], nr) || Jnbe(q[1], nr) || Jnbe(q[2], nr) || Jnbe(q[3], nr)) result = 1;
    HOT_PATH(g_cull_paths, result == 1 ? 6 : 7);
    ctx.eax = result;
    Leave(ctx, fp, e, edi, ebp);
}

}  // namespace

#ifndef BLACK_VERIFY_HOT

X86_FUNC(sub_000D0510) {
    SphereInView(ctx, base);
}

#else

namespace {

std::atomic<uint64_t> g_cull_calls{0}, g_cull_bad{0};

// once, synthetic calls in the dead stack 0x800 below esp: spheres and volumes with NaNs of distinct bits in the lanes
// each NaN case names, and spheres placed inside, across and outside each plane
void Synthetic(X86Context& ctx, uint8_t* base) {
    const uint32_t t = (ctx.esp - 0x800) & ~15u;
    for (int k = 0; k < kNanCaseCount; k++)
        for (int where = 0; where < 6; where++)
            VerifyCase("sub_000D0510", ctx, base, SphereInView, __imp__sub_000D0510, t - 0x200, 0x400, [&](X86Context& c) {
                // the sphere at t, the volume at t + 0x20 (two planes, three rows, an offset)
                const float off = where == 0 ? 0.0f : where == 1 ? 9.0f : where == 2 ? -9.0f : where == 3 ? 2.5f : 40.0f;
                PutNanVector(base, t, 1.0f + off, 2.0f - off, 0.5f * off, where == 5 ? 0.0f : 3.0f, 0, k);
                PutNanVector(base, t + 0x20, 0.0f, 1.0f, 0.0f, 4.0f, 1, k);
                PutNanVector(base, t + 0x30, 1.0f, 0.0f, 0.0f, -2.0f, 2, k);
                PutNanVector(base, t + 0x40, 0.5f, 0.25f, 0.0f, 1.0f, 3, k);
                PutNanVector(base, t + 0x50, 0.0f, 1.0f, -0.5f, 0.0f, 4, k);
                PutNanVector(base, t + 0x60, 2.0f, 0.0f, 1.0f, 0.5f, 5, k);
                PutNanVector(base, t + 0x70, 1.0f, -1.0f, 0.5f, 2.0f, 6, k);
                c.eax = t;
                c.esi = t + 0x20;
                c.esp = t - 0x40;
                X86_MEM32(c.esp) = 0;
            });
}

std::atomic<bool> g_synthetic{false};

}  // namespace

X86_FUNC(sub_000D0510) {
    if (!g_synthetic.exchange(true)) Synthetic(ctx, base);
    const uint32_t e = ctx.esp;
    Ranges r;
    r.Add(e - 0x40, 0x40);
    VerifyAll("sub_000D0510", ctx, base, SphereInView, __imp__sub_000D0510, r, g_cull_calls, g_cull_bad, &g_cull_paths);
}

#endif
