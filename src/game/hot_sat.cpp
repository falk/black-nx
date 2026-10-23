// Native replacements of the separating axis test between two convex collision shapes (boxes, triangles)
//
// sub_00071CC0 gathers the candidate axes (sub_00071770), projects both shapes onto them through the shape's vtable
// +0x98 (sub_0006E140 for a box, sub_0006C140 for a triangle) and keeps the axis of the largest separation.
// Each rewrite leaves the guest exactly as the original does: results, registers, xmm, the x87 stack and the stack
// bytes. Build with BLACK_VERIFY_HOT to run the original after every call and count the calls where anything differs.
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "game/hot_ordered.h"
#include "game/hot_verify.h"

using namespace hot;

X86_EXTERN_FUNC(__imp__sub_0006E140);
X86_EXTERN_FUNC(__imp__sub_0006C140);

namespace {

#ifdef BLACK_VERIFY_HOT
PathCounts g_box_paths{{}, {"axes", "no-axes", "overlap"}};
PathCounts g_tri_paths{{}, {"axes", "no-axes", "overlap"}};
PathCounts g_axes_paths{{},
                        {"faces-a-0/4+", "faces-a-1", "faces-a-2", "faces-a-3", "faces-b-0/4+", "faces-b-1", "faces-b-2",
                         "faces-b-3", "edge-kept", "edge-dropped", "edges-1x1", "1x1-first", "1x1-second", "fallback-1x1",
                         "fallback-1edge", "fallback-centres", "normalize", "out-axes", "out-none"}};
PathCounts g_test_paths{{}, {"native-box", "native-tri", "icall", "unrolled", "tail", "flip", "no-flip", "no-axes"}};
#endif

typedef uint32_t u4 __attribute__((vector_size(16)));

inline f4 GetXmm(const X86Xmm& x) {
    f4 v;
    std::memcpy(&v, &x, 16);
    return v;
}
inline void StF(uint8_t* p, float f) { std::memcpy(p, &f, 4); }
inline void StU(uint8_t* p, uint32_t u) { std::memcpy(p, &u, 4); }
inline float FromBits(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
// the first three lanes summed as the x86 does it: lane 0 + lane 1 (shufps 0x09), then + lane 2 (shufps 0x42)
inline float Dot3(f4 p) { return (p[0] + p[1]) + p[2]; }
inline bool Overlap(uint32_t a, uint64_t an, uint32_t b, uint64_t bn) {
    return uint64_t(a) < uint64_t(b) + bn && uint64_t(b) < uint64_t(a) + an;
}

// sub_0006E140: a box projected onto n axes (the box shape's vtable +0x98), cdecl
//   stack  box: centre +0x00, axes +0x10/+0x20/+0x30, half extents +0x70/+0x74/+0x78; axes (16 bytes each); n;
//          out (n pairs of floats)
//   out[i] = c - r, c + r with c = axis . centre, r = (|h2 (a3 . axis)| + |h1 (a2 . axis)|) + |h0 (a1 . axis)|
// Each |x| is x87 fld, fabs, fstp: exact in float, and the last one stays in the slot below the x87 top.
// Locals of the last axis: [esp+0x14..0x1C] the three |x|, [esp+0x20] c, [esp+0x24..0x2C] the three dots.
// When out overlaps the box, the axes or the frame, the original runs: there the order of loads and stores counts.
static void BoxProject(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp;
    const uint32_t s = ((e - 4) & ~15u) - 0x30;  // esp after the alignment, sub esp, 0x24 and three pushes
    const uint32_t box = X86_MEM32(e + 4), axes = X86_MEM32(e + 8), n = X86_MEM32(e + 0xC),
                   out = X86_MEM32(e + 0x10);
    if (n && (Overlap(out, 8ull * n, box, 0x7C) || Overlap(out, 8ull * n, axes, 16ull * n) ||
              Overlap(out, 8ull * n, s, e - s))) {
        HOT_PATH(g_box_paths, 2);
        __imp__sub_0006E140(ctx, base);
        return;
    }
    uint8_t* fp = base + s;
    X86_MEM32(e - 4) = ctx.ebp;
    StU(fp + 8, ctx.ebx);
    StU(fp + 4, ctx.esi);
    StU(fp, ctx.edi);
    ctx.esp = e + 4;
    if (n == 0) {
        HOT_PATH(g_box_paths, 1);
        ctx.eax = 0;
        return;
    }
    HOT_PATH(g_box_paths, 0);

    const f4 b0 = Load4(base, box), b1 = Load4(base, box + 0x10), b2 = Load4(base, box + 0x20),
             b3 = Load4(base, box + 0x30);
    const float h0 = F32(base, box + 0x70), h1 = F32(base, box + 0x74), h2 = F32(base, box + 0x78);
    float c = 0, d1 = 0, d2 = 0, d3 = 0, a1 = 0, a2 = 0, a3 = 0, r = 0;
    f4 q3 = {};
    for (uint32_t i = 0; i < n; i++) {
        const f4 ax = Load4(base, axes + 16 * i);
        c = Dot3(VMul(ax, b0));
        d1 = Dot3(VMul(b1, ax));
        a1 = std::fabs(h0 * d1);
        d2 = Dot3(VMul(b2, ax));
        a2 = std::fabs(h1 * d2);
        q3 = VMul(b3, ax);
        d3 = Dot3(q3);
        a3 = std::fabs(h2 * d3);
        r = (a3 + a2) + a1;
        uint8_t* o = X86_PTR(uint8_t, out + 8 * i);
        StF(o, c - r);
        StF(o + 4, c + r);
    }
    StF(fp + 0x14, a3);
    StF(fp + 0x18, a2);
    StF(fp + 0x1C, a1);
    StF(fp + 0x20, c);
    StF(fp + 0x24, d1);
    StF(fp + 0x28, d2);
    StF(fp + 0x2C, d3);
    ctx.st[(ctx.top - 1) & 7] = double(a3);
    ctx.eax = n;
    ctx.ecx = box;
    ctx.edx = out;
    PutXmm(ctx.xmm[0], Scalar(r));
    PutXmm(ctx.xmm[1], Scalar(c + r));
    PutXmm(ctx.xmm[2], Sh<0x09>(q3, q3));
}

// sub_0006C140: a triangle projected onto n axes (the triangle shape's vtable +0x98, set in sub_0006DBD0), cdecl
//   stack  triangle: corners +0x00, +0x20, +0x30; axes (16 bytes each); n; out (n pairs of floats)
//   out[i] = minss/maxss of the corner dots, in the order (d0, d2) then d3
// Locals of the last axis: [esp+0x14] d3, [+0x18] d2, [+0x1C] d0, [+0x20..0x28] d3 d2 d0, [+0x2C] min,
// [+0x30..0x38] d3 d2 d0, [+0x3C] max.
static void TriProject(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp;
    const uint32_t s = ((e - 4) & ~15u) - 0x40;  // esp after the alignment, sub esp, 0x34 and three pushes
    const uint32_t tri = X86_MEM32(e + 4), axes = X86_MEM32(e + 8), n = X86_MEM32(e + 0xC),
                   out = X86_MEM32(e + 0x10);
    if (n && (Overlap(out, 8ull * n, tri, 0x40) || Overlap(out, 8ull * n, axes, 16ull * n) ||
              Overlap(out, 8ull * n, s, e - s))) {
        HOT_PATH(g_tri_paths, 2);
        __imp__sub_0006C140(ctx, base);
        return;
    }
    uint8_t* fp = base + s;
    X86_MEM32(e - 4) = ctx.ebp;
    StU(fp + 8, ctx.ebx);
    StU(fp + 4, ctx.esi);
    StU(fp, ctx.edi);
    ctx.esp = e + 4;
    if (n == 0) {
        HOT_PATH(g_tri_paths, 1);
        ctx.eax = 0;
        return;
    }
    HOT_PATH(g_tri_paths, 0);

    const f4 v0 = Load4(base, tri), v2 = Load4(base, tri + 0x20), v3 = Load4(base, tri + 0x30);
    float d0 = 0, d2 = 0, d3 = 0, lo = 0, hi = 0;
    for (uint32_t i = 0; i < n; i++) {
        const f4 ax = Load4(base, axes + 16 * i);
        d0 = Dot3(VMul(ax, v0));
        d2 = Dot3(VMul(ax, v2));
        d3 = Dot3(VMul(ax, v3));
        lo = sse_min(sse_min(d0, d2), d3);
        hi = sse_max(sse_max(d0, d2), d3);
        uint8_t* o = X86_PTR(uint8_t, out + 8 * i);
        StF(o, lo);
        StF(o + 4, hi);
    }
    StF(fp + 0x14, d3);
    StF(fp + 0x18, d2);
    StF(fp + 0x1C, d0);
    StF(fp + 0x20, d3);
    StF(fp + 0x24, d2);
    StF(fp + 0x28, d0);
    StF(fp + 0x2C, lo);
    StF(fp + 0x30, d3);
    StF(fp + 0x34, d2);
    StF(fp + 0x38, d0);
    StF(fp + 0x3C, hi);
    ctx.eax = n;
    ctx.ecx = tri;
    ctx.edx = axes + 16 * n;
    PutXmm(ctx.xmm[0], Scalar(hi));
    PutXmm(ctx.xmm[1], Scalar(d3));
    PutXmm(ctx.xmm[2], Scalar(d2));
    PutXmm(ctx.xmm[3], Scalar(d0));
}

// sub_00071770: the candidate separating axes of shapes A and B, cdecl (out, A, B); eax the end of the axes written,
// or 0 when none.
//   shape  +0x00 centre, +0x10/+0x20/+0x30 face normals ([+0x88] of them, 1 to 3, copied last first),
//          +0x40.. edge directions ([+0x8C] of them)
//   1. the face normals of A, then of B
//   2. each edge of A x each edge of B, kept when its squared length is above [0x28599C] and normalized: one Newton
//      step on rsqrt, with [0x2858F0] = {c0, -, c2, -}: r = rsqrt(l) * c0 * (c2 - l * r * r), masked by 0 < l
//   3. one edge each: n = eA x eB; when |n| is above [0x28599C], eB x n and n x eA (lengths by sqrtss), normalized
//      by sub_00069210
//   4. no axes so far: with one edge each, eA x (eA x d), d = centre A - centre B; with one edge in all,
//      that edge x (e x d) (an uninitialised [esp+0x30] when it is B's... read as the original does); then d itself
// sub_00069210 (normalize [ecx] in place, the same Newton step, rsqrtps on all four lanes) is inlined with its call.
// Stack: [esp+0x10] A's edge pointer, [+0x14] A's edge index, [+0x18] squared length, [+0x1C] length,
// [+0x20] its xmm, [+0x30] the second axis of step 3.
static void SatAxes(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp;
    const uint32_t s = ((e - 4) & ~15u) - 0x40;  // esp after the alignment, sub esp, 0x34 and three pushes
    uint8_t* fp = base + s;
    X86_MEM32(e - 4) = ctx.ebp;
    uint32_t ecx = X86_MEM32(e + 4);
    StU(fp + 8, ctx.ebx);
    const uint32_t ebx = X86_MEM32(e + 8);
    StU(fp + 4, ctx.esi);
    StU(fp, ctx.edi);
    uint32_t esi = ecx, eax = 0, edx = ctx.edx;
    f4 x0 = GetXmm(ctx.xmm[0]), x1 = GetXmm(ctx.xmm[1]), x2 = GetXmm(ctx.xmm[2]), x3 = GetXmm(ctx.xmm[3]),
       x4 = GetXmm(ctx.xmm[4]), x5 = GetXmm(ctx.xmm[5]);
    const float eps = F32(base, 0x28599C);
    const f4 newton = Load4(base, 0x2858F0);

    auto faces = [&](uint32_t shape, int path) {
        const uint32_t k = X86_MEM32(shape + 0x88);
        if (k == 0 || k > 3) {
            HOT_PATH(g_axes_paths, path);
            return;
        }
        HOT_PATH(g_axes_paths, path + int(k));
        if (k == 3) {
            x0 = Load4(base, shape + 0x30);
            Store4(base, esi, x0);
            esi += 16;
        }
        if (k >= 2) {
            x0 = Load4(base, shape + 0x20);
            Store4(base, esi, x0);
            esi += 16;
        }
        x0 = Load4(base, shape + 0x10);
        Store4(base, esi, x0);
        esi += 16;
    };
    // sub_00069210 on [ecx], called from ret: x0 to x4 and [ecx] change
    auto normalize = [&](uint32_t ret) {
        HOT_PATH(g_axes_paths, 16);
        X86_MEM32(s - 4) = ret;
        x1 = Load4(base, ecx);
        x0 = VMul(x1, x1);
        x2 = Sh<0x09>(x0, x0);
        x0 = Lane0(x0, x0[0] + x2[0]);
        x3 = Sh<0x39>(x2, x2);
        x0 = Lane0(x0, x0[0] + x3[0]);
        x3 = newton;
        for (int i = 0; i < 4; i++) x2[i] = 1.0f / std::sqrt(x0[i]);
        x0 = Lane0(x0, x0[0] * x2[0]);
        x0 = Lane0(x0, x0[0] * x2[0]);
        x4 = Sh<0xAA>(x3, x3);
        x4 = Lane0(x4, x4[0] - x0[0]);
        x2 = Lane0(x2, x2[0] * x3[0]);
        x2 = Lane0(x2, x2[0] * x4[0]);
        x0 = Sh<0x00>(x2, x2);
        x1 = VMul(x1, x0);
        Store4(base, ecx, x1);
    };
    // lane 0 of v = |first three lanes of v * v|, through [esp+0x20] (16 bytes) and [esp+0x1C]; a = Sh<0x42>,
    // b = Sh<0x09> of the squares; v becomes the length alone
    auto length = [&](f4& v, f4& a, f4& b) {
        v = VMul(v, v);
        a = Sh<0x42>(v, v);
        b = Sh<0x09>(v, v);
        v = Lane0(v, v[0] + b[0]);
        v = Lane0(v, v[0] + a[0]);
        v = Lane0(v, std::sqrt(v[0]));
        Store4(base, s + 0x20, v);
        StF(fp + 0x1C, v[0]);
        v = Scalar(v[0]);
    };

    faces(ebx, 0);
    const uint32_t edi = X86_MEM32(e + 0xC);
    faces(edi, 4);

    // 2. edge x edge
    eax = X86_MEM32(ebx + 0x8C);
    StU(fp + 0x14, 0);
    if (int32_t(eax) > 0) {
        uint32_t ea = ebx + 0x40, ia = 0;
        StU(fp + 0x10, ea);
        do {
            eax = X86_MEM32(edi + 0x8C);
            ecx = 0;
            if (int32_t(eax) > 0) {
                eax = edi + 0x40;
                do {
                    edx = ea;
                    x1 = Load4(base, edx);
                    x0 = Load4(base, eax);
                    x0 = Sh<0xD2>(x0, x0);
                    x1 = Sh<0xC9>(x1, x1);
                    x2 = Sh<0xD2>(x0, x0);
                    x3 = Sh<0xC9>(x1, x1);
                    x3 = VMul(x3, x2);
                    x1 = VMul(x1, x0);
                    x1 = VSub(x1, x3);  // eA x eB
                    x0 = VMul(x1, x1);
                    x2 = Sh<0x42>(x0, x0);
                    x3 = Sh<0x09>(x0, x0);
                    x0 = Lane0(x0, x0[0] + x3[0]);
                    x0 = Lane0(x0, x0[0] + x2[0]);
                    edx = s + 0x18;
                    StF(fp + 0x18, x0[0]);
                    x2 = Scalar(x0[0]);
                    Store4(base, esi, x1);
                    if (Jnbe(x2[0], eps)) {
                        HOT_PATH(g_axes_paths, 8);
                        x3 = newton;
                        const uint32_t mask = 0.0f < x2[0] ? 0xFFFFFFFFu : 0u;
                        x4 = f4{FromBits(mask), 0, 0, 0};  // x2's upper lanes are zero
                        const float inf = 1.0f / std::sqrt(0.0f);
                        x0 = f4{1.0f / std::sqrt(x2[0]), inf, inf, inf};
                        x5 = Lane0(x0, x0[0] * x2[0]);
                        x2 = Lane0(x5, x5[0] * x0[0]);
                        x0 = Lane0(x0, x0[0] * x3[0]);
                        x5 = Sh<0xAA>(x3, x3);
                        x5 = Lane0(x5, x5[0] - x2[0]);
                        x0 = Lane0(x0, x0[0] * x5[0]);
                        x2 = f4{FromBits(Bits(x0[0]) & mask), 0, 0, 0};
                        Store4(base, s + 0x20, x2);
                        StF(fp + 0x1C, x2[0]);
                        edx = s + 0x1C;
                        x0 = Splat(x2[0]);
                        x1 = VMul(x1, x0);
                        Store4(base, esi, x1);
                        esi += 16;
                    } else {
                        HOT_PATH(g_axes_paths, 9);
                    }
                    edx = X86_MEM32(edi + 0x8C);
                    ecx++;
                    eax += 16;
                } while (int32_t(ecx) < int32_t(edx));
            }
            eax = ia + 1;
            edx = ea + 16;
            ecx = X86_MEM32(ebx + 0x8C);
            ia = eax;
            ea = edx;
            StU(fp + 0x14, ia);
            StU(fp + 0x10, ea);
        } while (int32_t(eax) < int32_t(ecx));
    }

    // 3. one edge each
    ecx = X86_MEM32(ebx + 0x8C);
    eax = 1;
    if (ecx == 1 && X86_MEM32(edi + 0x8C) == 1) {
        HOT_PATH(g_axes_paths, 10);
        x0 = Load4(base, ebx + 0x40);
        x1 = Load4(base, edi + 0x40);
        x1 = Sh<0xD2>(x1, x1);
        x2 = Sh<0xD2>(x1, x1);
        x0 = Sh<0xC9>(x0, x0);
        x3 = Sh<0xC9>(x0, x0);
        x0 = VMul(x0, x1);
        x3 = VMul(x3, x2);
        x0 = VSub(x0, x3);  // n = eA x eB
        x1 = VMul(x0, x0);
        x3 = Sh<0x09>(x1, x1);
        x2 = Sh<0x42>(x1, x1);
        x1 = Lane0(x1, x1[0] + x3[0]);
        x3 = Scalar(eps);
        x1 = Lane0(x1, x1[0] + x2[0]);
        x1 = Lane0(x1, std::sqrt(x1[0]));
        Store4(base, s + 0x20, x1);
        ecx = s + 0x1C;
        StF(fp + 0x1C, x1[0]);
        x1 = Scalar(x1[0]);
        if (Jnbe(x1[0], x3[0])) {
            x2 = Load4(base, edi + 0x40);
            x1 = Load4(base, ebx + 0x40);
            x0 = Sh<0xD2>(x0, x0);
            x4 = Sh<0xD2>(x0, x0);
            x2 = Sh<0xC9>(x2, x2);
            x5 = Sh<0xC9>(x2, x2);
            x2 = VMul(x2, x0);
            x5 = VMul(x5, x4);
            x4 = Sh<0xD2>(x0, x0);
            x1 = Sh<0xC9>(x1, x1);
            x2 = VSub(x2, x5);  // eB x n
            x5 = Sh<0xC9>(x1, x1);
            x1 = VMul(x1, x0);
            x5 = VMul(x5, x4);
            x0 = VSub(x1, x5);  // n x eA
            Store4(base, esi, x0);
            length(x0, x1, x4);
            edx = s + 0x1C;
            Store4(base, s + 0x30, x2);
            if (Jnbe(x0[0], x3[0])) {
                HOT_PATH(g_axes_paths, 11);
                ecx = esi;
                normalize(0x71A52);
                x2 = Load4(base, s + 0x30);
                esi += 16;
                eax = 1;
            }
            x0 = x2;
            Store4(base, esi, x2);
            length(x0, x1, x2);
            ecx = s + 0x1C;
            if (Jnbe(x0[0], F32(base, 0x28599C))) {
                HOT_PATH(g_axes_paths, 12);
                ecx = esi;
                normalize(0x71AB0);
                esi += 16;
                eax = 1;
            }
        }
    }

    // 4. no axes yet
    if (esi == X86_MEM32(e + 4)) {
        if (X86_MEM32(ebx + 0x8C) == eax && X86_MEM32(edi + 0x8C) == eax) {
            HOT_PATH(g_axes_paths, 13);
            x1 = Load4(base, edi);
            x0 = Load4(base, ebx);
            x0 = VSub(x0, x1);  // d
            x1 = Load4(base, ebx + 0x40);
            x0 = Sh<0xC9>(x0, x0);
            x3 = Sh<0xC9>(x0, x0);
            x1 = Sh<0xD2>(x1, x1);
            x0 = VMul(x0, x1);
            x2 = Sh<0xD2>(x1, x1);
            x1 = Load4(base, ebx + 0x40);
            x3 = VMul(x3, x2);
            x0 = VSub(x0, x3);
            x0 = Sh<0xC9>(x0, x0);
            x3 = Sh<0xC9>(x0, x0);
            x1 = Sh<0xD2>(x1, x1);
            x0 = VMul(x0, x1);
            x2 = Sh<0xD2>(x1, x1);
            x3 = VMul(x3, x2);
            x0 = VSub(x0, x3);
            Store4(base, esi, x0);
            length(x0, x1, x2);
            edx = s + 0x1C;
            if (Jnbe(x0[0], F32(base, 0x28599C))) {
                ecx = esi;
                normalize(0x71B76);
                esi += 16;
            }
        }
        eax = X86_MEM32(ebx + 0x8C);
        ecx = X86_MEM32(edi + 0x8C);
        edx = ecx + eax;
        if (edx == 1) {
            HOT_PATH(g_axes_paths, 14);
            x0 = eax ? Load4(base, ebx + 0x40) : Load4(base, s + 0x30);
            if (ecx) x0 = Load4(base, edi + 0x40);
            x2 = Load4(base, edi);
            x1 = Load4(base, ebx);
            x1 = VSub(x1, x2);
            x0 = Sh<0xD2>(x0, x0);
            x1 = Sh<0xC9>(x1, x1);
            x2 = Sh<0xD2>(x0, x0);
            x3 = Sh<0xC9>(x1, x1);
            x1 = VMul(x1, x0);
            x3 = VMul(x3, x2);
            x1 = VSub(x1, x3);
            x2 = Sh<0xD2>(x0, x0);
            x1 = Sh<0xC9>(x1, x1);
            x3 = Sh<0xC9>(x1, x1);
            x1 = VMul(x1, x0);
            x3 = VMul(x3, x2);
            x0 = VSub(x1, x3);
            Store4(base, esi, x0);
            length(x0, x1, x2);
            eax = s + 0x1C;
            if (Jnbe(x0[0], F32(base, 0x28599C))) {
                ecx = esi;
                normalize(0x71C3C);
                esi += 16;
            }
        }
        if (esi == X86_MEM32(e + 4)) {
            HOT_PATH(g_axes_paths, 15);
            x1 = Load4(base, edi);
            x0 = Load4(base, ebx);
            x0 = VSub(x0, x1);
            Store4(base, esi, x0);
            length(x0, x1, x2);
            ecx = s + 0x1C;
            if (Jnbe(x0[0], F32(base, 0x28599C))) {
                ecx = esi;
                normalize(0x71C9B);
                esi += 16;
            }
        }
    }
    if (esi == X86_MEM32(e + 4)) {
        HOT_PATH(g_axes_paths, 18);
        eax = 0;
    } else {
        HOT_PATH(g_axes_paths, 17);
        eax = esi;
    }

    ctx.eax = eax;
    ctx.ecx = ecx;
    ctx.edx = edx;
    PutXmm(ctx.xmm[0], x0);
    PutXmm(ctx.xmm[1], x1);
    PutXmm(ctx.xmm[2], x2);
    PutXmm(ctx.xmm[3], x3);
    PutXmm(ctx.xmm[4], x4);
    PutXmm(ctx.xmm[5], x5);
    ctx.esp = e + 4;
}

// a vtable entry, read as a plain uint32_t (devkitA64's GCC stops with an internal error when a value of the
// unaligned X86_MEM32 type reaches x86_icall's argument through inlining)
inline uint32_t Target(uint8_t* base, uint32_t va) {
    uint32_t t;
    std::memcpy(&t, base + va, 4);
    return t;
}

// a call through a shape's vtable +0x98 (the projection), the return address already pushed: the native box and
// triangle projections directly, anything else through the dispatcher
static void Project(X86Context& __restrict ctx, uint8_t* base, uint32_t target) {
    ctx.last_icall = target;
    if (target == 0x6E140) {
        HOT_PATH(g_test_paths, 0);
        BoxProject(ctx, base);
    } else if (target == 0x6C140) {
        HOT_PATH(g_test_paths, 1);
        TriProject(ctx, base);
    } else {
        HOT_PATH(g_test_paths, 2);
        x86_icall(ctx, base, target);
    }
}

// sub_00071CC0: the separating axis test of shapes A and B, cdecl (out, A, B)
//   the axes from sub_00071770 at [esp+0x120], n of them ([esp+0x18]); A projected to [esp+0x30], B to [esp+0xA8]
//   (n pairs each); per axis the separation max(A.lo - B.hi, B.lo - A.hi), the largest kept: [esp+0x10] its value,
//   [esp+0x14] 1 when it is A.lo - B.hi, [esp+0x1C] its axis
//   out = the axis, negated when [esp+0x14]; the separation returned in st0
// After each call the registers are taken from the context, as the original does.
static void SatTest(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp;
    const uint32_t s = ((e - 4) & ~15u) - 0x210;  // esp after the alignment, sub esp, 0x204 and three pushes
    uint8_t* fp = base + s;
    X86_MEM32(e - 4) = ctx.ebp;
    ctx.ebp = e - 4;
    StU(fp + 8, ctx.ebx);
    const uint32_t ebx = X86_MEM32(e + 8);
    StU(fp + 4, ctx.esi);
    StU(fp, ctx.edi);
    const uint32_t edi = X86_MEM32(e + 0xC);
    const uint32_t list = s + 0x120;
    StU(fp - 4, edi);
    StU(fp - 8, ebx);
    StU(fp - 0xC, list);
    StU(fp - 0x10, 0x71CE4);
    ctx.eax = list;
    ctx.ebx = ebx;
    ctx.edi = edi;
    ctx.esp = s - 0x10;
    SatAxes(ctx, base);

    // A's projection
    const uint32_t n = uint32_t(int32_t(ctx.eax - list) >> 4);
    StU(fp - 0x10, s + 0x30);
    StU(fp - 0x14, n);
    StU(fp + 0x18, n);
    StU(fp - 0x18, list);
    StU(fp - 0x1C, ebx);
    StU(fp + 0x1C, 0);
    StU(fp + 0x10, 0);  // 0.0f
    StU(fp + 0x14, 0);
    StU(fp - 0x20, 0x71D17);
    PutXmm(ctx.xmm[0], f4{0, 0, 0, 0});
    ctx.eax = list;
    ctx.ecx = list;
    ctx.edx = s + 0x30;
    ctx.esi = 0;
    ctx.esp = s - 0x20;
    Project(ctx, base, Target(base, ebx + 0x98));

    // B's projection
    uint32_t sp = ctx.esp;
    const uint32_t count = X86_MEM32(sp + 0x34);
    X86_MEM32(sp - 4) = sp + 0xC4;
    X86_MEM32(sp - 8) = count;
    X86_MEM32(sp - 0xC) = sp + 0x13C;
    X86_MEM32(sp - 0x10) = ctx.edi;
    X86_MEM32(sp - 0x14) = 0x71D33;
    ctx.ebx = count;
    ctx.ecx = sp + 0xC4;
    ctx.edx = sp + 0x13C;
    ctx.esp = sp - 0x14;
    Project(ctx, base, Target(base, ctx.edi + 0x98));

    // the largest separation
    const uint32_t t = ctx.esp + 0x2C;  // the frame again
    uint8_t* tp = base + t;
    const uint32_t m = ctx.ebx;
    uint32_t eax = 0, ecx = ctx.ecx, edx = ctx.edx, esi = ctx.esi;
    float best = 0, x0f = 0, x1f = 0;
    uint32_t flag = 0;
    bool any = false, better = false;
    std::memcpy(&best, tp + 0x10, 4);
    std::memcpy(&flag, tp + 0x14, 4);
    auto axis = [&](uint32_t i, uint32_t ptr, uint32_t& which) {
        const uint8_t* p = tp + 8 * i;
        float lo_a, hi_a, lo_b, hi_b;
        std::memcpy(&lo_a, p + 0x30, 4);
        std::memcpy(&hi_a, p + 0x34, 4);
        std::memcpy(&lo_b, p + 0xA8, 4);
        std::memcpy(&hi_b, p + 0xAC, 4);
        x0f = lo_a - hi_b;
        x1f = lo_b - hi_a;
        if (Jnbe(x0f, x1f)) {
            which = 1;
        } else {
            x0f = x1f;
            which = 0;
        }
        if (esi == 0 || Jnbe(x0f, best)) {
            esi = ptr;
            best = x0f;
            flag = which;
            better = true;
        }
    };
    if (int32_t(m) >= 4) {
        HOT_PATH(g_test_paths, 3);
        any = true;
        const uint32_t last = m - 3;
        ecx = t + 0x130;
        do {
            axis(eax, ecx - 0x10, edx);
            axis(eax + 1, ecx, edx);
            axis(eax + 2, ecx + 0x10, edx);
            axis(eax + 3, ecx + 0x20, edx);
            eax += 4;
            ecx += 0x40;
        } while (eax < last);
        StU(tp + 0x1C, esi);
    }
    if (eax < m) {
        HOT_PATH(g_test_paths, 4);
        any = true;
        edx = t + (eax << 4) + 0x120;
        ecx = eax << 4;
        do {
            axis(eax, edx, ecx);
            eax++;
            edx += 16;
        } while (eax < m);
        StU(tp + 0x1C, esi);
    }
    if (!any) HOT_PATH(g_test_paths, 7);
    if (better) {
        StF(tp + 0x10, best);
        StU(tp + 0x14, flag);
    }
    if (any) {
        PutXmm(ctx.xmm[0], Scalar(x0f));
        PutXmm(ctx.xmm[1], Scalar(x1f));
    }

    eax = X86_MEM32(t + 0x14);
    const uint32_t bp = ctx.ebp;
    f4 x0;
    if (eax) {
        HOT_PATH(g_test_paths, 5);
        StU(tp + 0x18, 0x80000000u);
        esi = X86_MEM32(t + 0x1C);
        const u4 sign = {0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u};
        x0 = (f4)((u4)Load4(base, esi) ^ sign);
        Store4(base, t + 0x20, x0);
        x87_push(ctx, double(F32(base, t + 0x10)));
        edx = X86_MEM32(bp + 8);
        Store4(base, edx, x0);
    } else {
        HOT_PATH(g_test_paths, 6);
        x0 = Load4(base, esi);
        x87_push(ctx, double(F32(base, t + 0x10)));
        eax = X86_MEM32(bp + 8);
        Store4(base, eax, x0);
    }
    PutXmm(ctx.xmm[0], x0);
    ctx.eax = eax;
    ctx.ecx = ecx;
    ctx.edx = edx;
    ctx.edi = X86_MEM32(t);
    ctx.esi = X86_MEM32(t + 4);
    ctx.ebx = X86_MEM32(t + 8);
    ctx.ebp = X86_MEM32(bp);
    ctx.esp = bp + 8;
}

}  // namespace

#ifndef BLACK_VERIFY_HOT

X86_FUNC(sub_0006E140) { BoxProject(ctx, base); }
X86_FUNC(sub_0006C140) { TriProject(ctx, base); }
X86_FUNC(sub_00071770) { SatAxes(ctx, base); }
X86_FUNC(sub_00071CC0) { SatTest(ctx, base); }

#else

X86_EXTERN_FUNC(__imp__sub_00071770);
X86_EXTERN_FUNC(__imp__sub_00071CC0);

namespace {

std::atomic<uint64_t> g_box_calls{0}, g_box_bad{0};
std::atomic<uint64_t> g_tri_calls{0}, g_tri_bad{0};
std::atomic<uint64_t> g_axes_calls{0}, g_axes_bad{0};
std::atomic<uint64_t> g_test_calls{0}, g_test_bad{0};

// a projection writes its frame and n pairs at out
Ranges ProjectRanges(X86Context& ctx, uint8_t* base) {
    const uint32_t e = ctx.esp, n = X86_MEM32(e + 0xC);
    Ranges r;
    r.Add(e - 0x60, 0x60);
    r.Add(X86_MEM32(e + 0x10), 8 * (n < 0x400 ? n : 0x400));
    return r;
}

}  // namespace

X86_FUNC(sub_0006E140) {
    VerifyAll("sub_0006E140", ctx, base, BoxProject, __imp__sub_0006E140, ProjectRanges(ctx, base), g_box_calls,
              g_box_bad, &g_box_paths);
}

X86_FUNC(sub_0006C140) {
    VerifyAll("sub_0006C140", ctx, base, TriProject, __imp__sub_0006C140, ProjectRanges(ctx, base), g_tri_calls,
              g_tri_bad, &g_tri_paths);
}

// once, synthetic shape pairs in the dead stack 0x800 below esp (A at T, B at T+0xA0, out at T+0x140, the call's
// esp at T-0x40), for the ways gameplay seldom takes and for edge w lanes holding quiet NaNs of different bits:
//   faces/edges 3/3 against 1/3; 2/1 against 2/1; parallel single edges and no faces (step 4 with one edge each);
//   one edge in all, A's or B's; no edges and no faces, the centres apart or together (no axes)
namespace {
struct ShapeCase {
    uint32_t fa, ea, fb, eb;
    float ex, dx;  // the edges' x (B's is twice A's: parallel), the centres' x offset
};
std::atomic<bool> g_axes_synthetic{false};

void AxesSynthetic(X86Context& ctx, uint8_t* base) {
    static const ShapeCase cases[] = {{3, 3, 1, 3, 1, 1}, {2, 1, 2, 1, 1, 1}, {0, 1, 0, 1, 1, 0.5f},
                                      {0, 1, 0, 0, 1, 0.5f}, {0, 0, 0, 1, 1, 0.5f}, {0, 0, 0, 0, 1, 2},
                                      {0, 0, 0, 0, 1, 0}};
    const uint32_t t = (ctx.esp - 0x800) & ~15u, e = t - 0x40;
    for (const ShapeCase& k : cases) {
        VerifyCase("sub_00071770", ctx, base, SatAxes, __imp__sub_00071770, t - 0x200, 0x500, [&](X86Context& c) {
            static const uint32_t w[4] = {0xFFFFFFFFu, 0x7FFFFFFFu, 0xFFC00001u, 0x7FC00002u};
            for (int s2 = 0; s2 < 2; s2++) {
                const uint32_t sh = t + 0xA0 * s2;
                for (int r = 0; r < 7; r++) {
                    const uint32_t v = sh + 16 * r;
                    X86_MEMF32(v) = 0.3f * float(r + 1) + 0.1f * float(s2);
                    X86_MEMF32(v + 4) = 1.0f - 0.2f * float(r) + 0.05f * float(s2);
                    X86_MEMF32(v + 8) = 0.7f * float(r % 3) - 0.4f;
                    X86_MEM32(v + 12) = w[(r + s2) & 3];
                }
                X86_MEMF32(sh) = s2 ? k.dx : 0.0f;  // centre
                X86_MEMF32(sh + 4) = s2 ? 0.25f : 0.0f;
                X86_MEMF32(sh + 8) = 0.0f;
                if (k.ea == 1 && k.eb == 1 && k.fa == 0) {  // parallel single edges
                    X86_MEMF32(sh + 0x40) = k.ex * float(s2 + 1);
                    X86_MEMF32(sh + 0x44) = 0;
                    X86_MEMF32(sh + 0x48) = 0;
                }
                X86_MEM32(sh + 0x88) = s2 ? k.fb : k.fa;
                X86_MEM32(sh + 0x8C) = s2 ? k.eb : k.ea;
            }
            if (k.dx == 0.0f) {  // centres together: no axis at all
                X86_MEMF32(t + 0xA0 + 4) = 0.0f;
            }
            X86_MEM32(e) = 0;
            X86_MEM32(e + 4) = t + 0x140;
            X86_MEM32(e + 8) = t;
            X86_MEM32(e + 0xC) = t + 0xA0;
            c.esp = e;
        });
    }
}
}  // namespace

// the frame (with sub_00069210's return address below it) and the axes: the faces and edge pairs of both, plus
// the step 3 and 4 axes
X86_FUNC(sub_00071770) {
    if (!g_axes_synthetic.exchange(true)) AxesSynthetic(ctx, base);
    const uint32_t e = ctx.esp, a = X86_MEM32(e + 8), b = X86_MEM32(e + 0xC);
    uint32_t axes = X86_MEM32(a + 0x88) + X86_MEM32(b + 0x88) + X86_MEM32(a + 0x8C) * X86_MEM32(b + 0x8C) + 3;
    if (axes > 64) axes = 64;
    Ranges r;
    r.Add(e - 0x60, 0x60);
    r.Add(X86_MEM32(e + 4), 16 * axes);
    VerifyAll("sub_00071770", ctx, base, SatAxes, __imp__sub_00071770, r, g_axes_calls, g_axes_bad, &g_axes_paths);
}

// its frame and the frames of the calls it makes, and out
X86_FUNC(sub_00071CC0) {
    const uint32_t e = ctx.esp;
    Ranges r;
    r.Add(e - 0x300, 0x300);
    r.Add(X86_MEM32(e + 4), 16);
    VerifyAll("sub_00071CC0", ctx, base, SatTest, __imp__sub_00071CC0, r, g_test_calls, g_test_bad, &g_test_paths);
}

#endif
