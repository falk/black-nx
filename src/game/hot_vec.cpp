// Native replacements of small vector leaves the game calls everywhere
//
// sub_0009C8D0: out = v / |v| over all four lanes (the length from x*x + y*y + z*z + w*w with rsqrtps, which the
// generated code takes as 1 / sqrt), or v * 0 when the squared length is not above the constant at 0x2870BC.
// sub_00012600: out (eax) = v.x * m[0] + m[3] + v.y * m[1] + v.z * m[2], a point through a 4x4 matrix (cdecl: m, v).
// sub_00011C10, sub_00011B90: v (eax) scaled in place by c / |v.xyz| (c at 0x2881C8); sub_00011B90 returns |v.xyz|
// in xmm0. sub_0009C330: out = a * b of two quaternions (cdecl: a, b, out), xyz in SSE, w on the x87 stack.
// The rewrites keep the originals' operand order, their stores (the saved registers, the frame, the result) and the
// registers and xmm they leave.
// Build with BLACK_VERIFY_HOT to run the original after every call and count the calls where anything differs.
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#include "game/hot_ordered.h"
#include "game/hot_synth.h"
#include "game/hot_verify.h"
#include "game/hot_x87.h"

using namespace hot;

X86_EXTERN_FUNC(__imp__sub_0009C8D0);
X86_EXTERN_FUNC(__imp__sub_00012600);
X86_EXTERN_FUNC(__imp__sub_00011C10);
X86_EXTERN_FUNC(__imp__sub_00011B90);
X86_EXTERN_FUNC(__imp__sub_0009C330);

namespace {

#ifdef BLACK_VERIFY_HOT
PathCounts g_norm_paths{{}, {"normalized", "zero"}};
PathCounts g_point_paths{{}, {"native", "operands-in-frame"}};
PathCounts g_unit_paths{{}, {"native", "operand-in-frame"}};
PathCounts g_unitl_paths{{}, {"native", "operand-in-frame"}};
PathCounts g_quat_paths{{}, {"native", "operands-in-frame"}};
#endif

inline bool Overlap(uint32_t a, uint64_t an, uint32_t b, uint64_t bn) {
    return uint64_t(a) < uint64_t(b) + bn && uint64_t(b) < uint64_t(a) + an;
}

// sub_00012600 (cdecl: m, v; out in eax)
// Frame: push ebp, the alignment, sub esp, 0x18, push esi, push edi; the sum at [esp+0x10], copied to out a dword at
// a time
static void TransformPoint(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp;
    const uint32_t s = ((e - 4) & ~15u) - 0x20;
    const uint32_t m = X86_MEM32(e + 4), v = X86_MEM32(e + 8), out = ctx.eax;
    if (Overlap(m, 0x40, s, e - s) || Overlap(v, 12, s, e - s) || Overlap(out, 16, s, e - s)) {
        HOT_PATH(g_point_paths, 1);
        __imp__sub_00012600(ctx, base);
        return;
    }
    HOT_PATH(g_point_paths, 0);
    X86_MEM32(e - 4) = ctx.ebp;
    X86_MEM32(s + 4) = ctx.esi;
    X86_MEM32(s) = ctx.edi;
    f4 x0 = VMul(Splat(F32(base, v)), Load4(base, m));
    x0 = VAdd(x0, Load4(base, m + 0x30));
    f4 x1 = VMul(Splat(F32(base, v + 4)), Load4(base, m + 0x10));
    x0 = VAdd(x0, x1);
    x1 = VMul(Splat(F32(base, v + 8)), Load4(base, m + 0x20));
    x0 = VAdd(x0, x1);
    Store4(base, s + 0x10, x0);
    Store4(base, out, x0);
    ctx.ecx = out;
    ctx.edx = X86_MEM32(s + 0x1C);
    PutXmm(ctx.xmm[0], x0);
    PutXmm(ctx.xmm[1], x1);
    ctx.ebp = X86_MEM32(e - 4);
    ctx.esp = e + 4;
}

// the squared length of v.xyz as the originals add it: lane 0 = (x*x + y*y) + z*z, lanes 1-3 the other squares;
// x3 = the squares rotated twice
inline f4 Dot3(f4 v, f4& x3) {
    f4 x0 = VMul(v, v);
    const f4 x2 = Sh<0x39>(x0, x0);
    x0[0] = SAdd(x0[0], x2[0]);
    x3 = Sh<0x39>(x2, x2);
    x0[0] = SAdd(x0[0], x3[0]);
    return x0;
}

// sub_00011C10 (v in eax). Frame: push ebp, the alignment, sub esp, 0x10; the squares with the length in lane 0 at
// [esp]
static void ScaleToUnit(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp, s = ((e - 4) & ~15u) - 0x10, v = ctx.eax;
    if (Overlap(v, 16, s, e - s)) {
        HOT_PATH(g_unit_paths, 1);
        __imp__sub_00011C10(ctx, base);
        return;
    }
    HOT_PATH(g_unit_paths, 0);
    X86_MEM32(e - 4) = ctx.ebp;
    const f4 x1 = Load4(base, v);
    f4 x3;
    f4 x0 = Dot3(x1, x3);
    x0[0] = std::sqrt(x0[0]);
    Store4(base, s, x0);
    const float c = F32(base, 0x2881C8u);
    const float len = x0[0];
    f4 q = Splat(c);
    q[0] = c / len;
    const f4 x2 = Splat(q[0]);
    const f4 r = VMul(x1, x2);
    Store4(base, v, r);
    ctx.ecx = 0x2881C8u;
    PutXmm(ctx.xmm[0], q);
    PutXmm(ctx.xmm[1], r);
    PutXmm(ctx.xmm[2], x2);
    PutXmm(ctx.xmm[3], x3);
    ctx.ebp = X86_MEM32(e - 4);
    ctx.esp = e + 4;
}

// sub_00011B90 (v in eax). Frame: push ebp, the alignment, sub esp, 0x20; the length at [esp+0x0C], the squares with
// the length in lane 0 at [esp+0x10]
static void ScaleToUnitLength(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp, s = ((e - 4) & ~15u) - 0x20, v = ctx.eax;
    if (Overlap(v, 16, s, e - s)) {
        HOT_PATH(g_unitl_paths, 1);
        __imp__sub_00011B90(ctx, base);
        return;
    }
    HOT_PATH(g_unitl_paths, 0);
    X86_MEM32(e - 4) = ctx.ebp;
    const f4 x1 = Load4(base, v);
    X86_MEM32(s + 0xC) = 0;
    f4 x3;
    f4 x0 = Dot3(x1, x3);
    x0[0] = std::sqrt(x0[0]);
    Store4(base, s + 0x10, x0);
    const float c = F32(base, 0x2881C8u);
    const float len = x0[0];
    X86_MEMF32(s + 0xC) = len;
    f4 q = Splat(c);
    q[0] = c / len;
    const f4 x2 = Splat(q[0]);
    const f4 r = VMul(x1, x2);
    Store4(base, v, r);
    ctx.ecx = s + 0xC;
    ctx.edx = 0x2881C8u;
    PutXmm(ctx.xmm[0], Scalar(F32(base, s + 0xC)));
    PutXmm(ctx.xmm[1], r);
    PutXmm(ctx.xmm[2], x2);
    PutXmm(ctx.xmm[3], x3);
    ctx.ebp = X86_MEM32(e - 4);
    ctx.esp = e + 4;
}

// sub_0009C330 (cdecl: a, b, out): xyz = a.xyz * b.w + (a.yzx * b.zxy - a.zxy * b.yzx) + b.xyz * a.w, w on the x87
// stack as b.w * a.w - ((a.z * b.z + a.y * b.y) + a.x * b.x). Frame: push ebp, the alignment, sub esp, 0x1C,
// push esi; [esp+0x0C] each w in turn, [esp+0x10] the sum, its lane 3 overwritten by the dword the function reads
// from [esp+0x1C] before it writes the frame
static void QuatMul(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp, s = ((e - 4) & ~15u) - 0x20;
    const uint32_t a = X86_MEM32(e + 4), b = X86_MEM32(e + 8), out = X86_MEM32(e + 0xC);
    if (Overlap(a, 16, s, e - s) || Overlap(b, 16, s, e - s) || Overlap(out, 16, s, e - s) || Overlap(out, 16, a, 16) ||
        Overlap(out, 16, b, 16)) {
        HOT_PATH(g_quat_paths, 1);
        __imp__sub_0009C330(ctx, base);
        return;
    }
    HOT_PATH(g_quat_paths, 0);
    X86_MEM32(e - 4) = ctx.ebp;
    const f4 x0in = Load4(base, b), qa = Load4(base, a);
    const uint32_t junk = X86_MEM32(s + 0x1C);
    const f4 x3 = VMul(Sh<0xD2>(qa, qa), Sh<0xC9>(x0in, x0in));
    X86_MEM32(s) = ctx.esi;
    const uint32_t bw = X86_MEM32(b + 0xC);
    X86_MEM32(s + 0xC) = bw;
    f4 x4 = VMul(Sh<0xC9>(qa, qa), Sh<0xD2>(x0in, x0in));
    float bwf;
    std::memcpy(&bwf, &bw, 4);
    const f4 x1a = VMulS(qa, bwf);
    x4 = VSub(x4, x3);
    Store4(base, s + 0x10, x4);
    X86_MEM32(s + 0x1C) = junk;
    f4 x2 = VAdd(Load4(base, s + 0x10), x1a);
    Store4(base, s + 0x10, x2);
    X86_MEM32(s + 0x1C) = junk;
    const uint32_t aw = X86_MEM32(a + 0xC);
    X86_MEM32(s + 0xC) = aw;
    float awf;
    std::memcpy(&awf, &aw, 4);
    const f4 x0 = VMulS(x0in, awf);
    const f4 x1 = VAdd(Load4(base, s + 0x10), x0);
    Store4(base, s + 0x10, x1);
    // the x87 part, slot by slot as the generated code leaves the stack
    const uint32_t t = ctx.top;
    double d0 = double(F32(base, s + 0x10));
    ctx.st[(t - 1) & 7] = d0;
    X86_MEMF32(out) = float(d0);
    X86_MEM32(out + 4) = X86_MEM32(s + 0x14);
    X86_MEM32(out + 8) = X86_MEM32(s + 0x18);
    // fld then fmul: the loaded value is the destination; faddp/fsubp st1, st0: st1 is
    double w = x87_prec(ctx, X87Mul(double(F32(base, b + 0xC)), double(F32(base, a + 0xC))));
    double sum = x87_prec(ctx, X87Mul(double(F32(base, a + 8)), double(F32(base, b + 8))));
    double p1 = x87_prec(ctx, X87Mul(double(F32(base, a + 4)), double(F32(base, b + 4))));
    sum = x87_prec(ctx, X87Add(sum, p1));
    double p0 = x87_prec(ctx, X87Mul(double(F32(base, a)), double(F32(base, b))));
    sum = x87_prec(ctx, X87Add(sum, p0));
    w = x87_prec(ctx, X87Sub(w, sum));
    ctx.st[(t - 1) & 7] = w;
    ctx.st[(t - 2) & 7] = sum;
    ctx.st[(t - 3) & 7] = p0;
    X86_MEMF32(out + 0xC) = float(w);
    ctx.eax = b;
    ctx.ecx = a;
    ctx.edx = out;
    PutXmm(ctx.xmm[0], x0);
    PutXmm(ctx.xmm[1], x1);
    PutXmm(ctx.xmm[2], x2);
    PutXmm(ctx.xmm[3], x3);
    PutXmm(ctx.xmm[4], x4);
    ctx.ebp = X86_MEM32(e - 4);
    ctx.esp = e + 4;
}

// sub_0009C8D0 (cdecl: v, out)
static void Normalize4(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp;
    X86_MEM32(e - 4) = ctx.ebp;
    const uint32_t src = X86_MEM32(e + 4);
    const f4 v = Load4(base, src);
    const f4 m = VMul(v, v);
    const f4 x3 = VAdd(Splat(m[2]), Splat(m[3]));
    const f4 x4 = Splat(m[0]);
    f4 x0 = VAdd(x4, Splat(m[1]));
    const float eps = F32(base, 0x2870BCu);
    x0 = VAdd(x0, x3);
    const uint32_t dst = X86_MEM32(e + 8);
    f4 x1;
    if (Jnbe(x0[0], eps)) {
        HOT_PATH(g_norm_paths, 0);
        for (int i = 0; i < 4; i++) x0[i] = 1.0f / std::sqrt(x0[i]);
        x1 = VMul(v, x0);
    } else {
        HOT_PATH(g_norm_paths, 1);
        x0 = VSub(x0, x0);
        x1 = VMul(v, x0);
    }
    Store4(base, dst, x1);
    ctx.eax = src;
    ctx.ecx = 0x2870BCu;
    ctx.edx = dst;
    PutXmm(ctx.xmm[0], x0);
    PutXmm(ctx.xmm[1], x1);
    PutXmm(ctx.xmm[2], Scalar(eps));
    PutXmm(ctx.xmm[3], x3);
    PutXmm(ctx.xmm[4], x4);
    ctx.ebp = X86_MEM32(e - 4);
    ctx.esp = e + 4;
}

}  // namespace

#ifndef BLACK_VERIFY_HOT

X86_FUNC(sub_0009C8D0) {
    Normalize4(ctx, base);
}
X86_FUNC(sub_00012600) {
    TransformPoint(ctx, base);
}
X86_FUNC(sub_00011C10) {
    ScaleToUnit(ctx, base);
}
X86_FUNC(sub_00011B90) {
    ScaleToUnitLength(ctx, base);
}
X86_FUNC(sub_0009C330) {
    QuatMul(ctx, base);
}

#else

namespace {

std::atomic<uint64_t> g_norm_calls{0}, g_norm_bad{0};
std::atomic<uint64_t> g_point_calls{0}, g_point_bad{0};
std::atomic<uint64_t> g_unit_calls{0}, g_unit_bad{0}, g_unitl_calls{0}, g_unitl_bad{0}, g_quat_calls{0}, g_quat_bad{0};

// once, synthetic calls in the dead stack 0x800 below esp: vectors with NaNs of distinct bits, a zero vector, a
// tiny one and a negative-zero one
void Synthetic(X86Context& ctx, uint8_t* base) {
    const uint32_t t = (ctx.esp - 0x800) & ~15u;
    for (int k = 0; k < kNanCaseCount; k++)
        for (int which = 0; which < 4; which++)
            VerifyCase("sub_0009C8D0", ctx, base, Normalize4, __imp__sub_0009C8D0, t - 0x200, 0x400, [&](X86Context& c) {
                const float s = which == 0 ? 1.0f : which == 1 ? 0.0f : which == 2 ? 1e-20f : -0.0f;
                PutNanVector(base, t, 3.0f * s, -4.0f * s, 0.5f * s, 2.0f * s, 0, k);
                c.esp = t - 0x40;
                X86_MEM32(c.esp) = 0;
                X86_MEM32(c.esp + 4) = t;
                X86_MEM32(c.esp + 8) = t + 0x20;
            });
}

std::atomic<bool> g_synthetic{false};

// a NaN case of a function whose generated reference is not NaN-exact (it commutes the operands of mulps/addss, and
// takes x87 operations as plain doubles): there a difference is expected and named so in the log; the no-NaN case
// keeps the plain name, so a difference there is a regression
std::string NanCaseName(const char* fn, int k) {
    return k ? std::string(fn) + " [" + kNanCases[k].what + ": reference not NaN-exact, a difference is expected]"
             : std::string(fn);
}

// a matrix whose rows hold NaNs of distinct bits in the lanes each NaN case names, a point with NaNs in its lanes
void SyntheticPoint(X86Context& ctx, uint8_t* base) {
    const uint32_t t = (ctx.esp - 0x800) & ~15u;
    for (int k = 0; k < kNanCaseCount; k++)
        VerifyCase(NanCaseName("sub_00012600", k).c_str(), ctx, base, TransformPoint, __imp__sub_00012600, t - 0x200, 0x400, [&](X86Context& c) {
            for (uint32_t i = 0; i < 4; i++)
                PutNanVector(base, t + 16 * i, 1.0f + float(i), -2.0f, 0.5f * float(i), 1.0f, i, k);
            PutNanVector(base, t + 0x40, 3.0f, -1.0f, 2.0f, 9.0f, 4, k);
            c.eax = t + 0x60;
            c.esp = t - 0x40;
            X86_MEM32(c.esp) = 0;
            X86_MEM32(c.esp + 4) = t;
            X86_MEM32(c.esp + 8) = t + 0x40;
        });
}
std::atomic<bool> g_synthetic_point{false};

// vectors and quaternions with NaNs of distinct bits in the lanes each NaN case names, a zero vector
void SyntheticSmall(X86Context& ctx, uint8_t* base) {
    const uint32_t t = (ctx.esp - 0x800) & ~15u;
    for (int k = 0; k < kNanCaseCount; k++)
        for (int z = 0; z < 2; z++) {
            const float m = z ? 0.0f : 1.0f;
            VerifyCase(NanCaseName("sub_00011C10", k).c_str(), ctx, base, ScaleToUnit, __imp__sub_00011C10, t - 0x200, 0x400, [&](X86Context& c) {
                PutNanVector(base, t, 3.0f * m, -4.0f * m, 0.5f * m, 7.0f, 0, k);
                c.eax = t;
                c.esp = t - 0x40;
                X86_MEM32(c.esp) = 0;
            });
            VerifyCase(NanCaseName("sub_00011B90", k).c_str(), ctx, base, ScaleToUnitLength, __imp__sub_00011B90, t - 0x200, 0x400, [&](X86Context& c) {
                PutNanVector(base, t, 3.0f * m, -4.0f * m, 0.5f * m, 7.0f, 1, k);
                c.eax = t;
                c.esp = t - 0x40;
                X86_MEM32(c.esp) = 0;
            });
            VerifyCase(NanCaseName("sub_0009C330", k).c_str(), ctx, base, QuatMul, __imp__sub_0009C330, t - 0x200, 0x400, [&](X86Context& c) {
                PutNanVector(base, t, 0.5f * m, -0.25f, 0.75f, 0.1f, 2, k);
                PutNanVector(base, t + 0x10, -0.5f, 0.125f * m, 0.5f, 0.9f, 3, k);
                c.esp = t - 0x40;
                X86_MEM32(c.esp) = 0;
                X86_MEM32(c.esp + 4) = t;
                X86_MEM32(c.esp + 8) = t + 0x10;
                X86_MEM32(c.esp + 0xC) = t + 0x20;
            });
        }
}
std::atomic<bool> g_synthetic_small{false};

}  // namespace

X86_FUNC(sub_00011C10) {
    if (!g_synthetic_small.exchange(true)) SyntheticSmall(ctx, base);
    const uint32_t e = ctx.esp;
    Ranges r;
    r.Add(e - 0x40, 0x40);
    r.Add(ctx.eax, 16);
    if (SyntheticDepth() > 0) {  // called from another rewrite's synthetic case (the particle quads): not counted
        std::atomic<uint64_t> calls{0}, bad{0};
        VerifyAll("sub_00011C10 (in a synthetic case)", ctx, base, ScaleToUnit, __imp__sub_00011C10, r, calls, bad, nullptr);
        return;
    }
    VerifyAll("sub_00011C10", ctx, base, ScaleToUnit, __imp__sub_00011C10, r, g_unit_calls, g_unit_bad, &g_unit_paths);
}

X86_FUNC(sub_00011B90) {
    if (!g_synthetic_small.exchange(true)) SyntheticSmall(ctx, base);
    const uint32_t e = ctx.esp;
    Ranges r;
    r.Add(e - 0x40, 0x40);
    r.Add(ctx.eax, 16);
    VerifyAll("sub_00011B90", ctx, base, ScaleToUnitLength, __imp__sub_00011B90, r, g_unitl_calls, g_unitl_bad,
              &g_unitl_paths);
}

X86_FUNC(sub_0009C330) {
    if (!g_synthetic_small.exchange(true)) SyntheticSmall(ctx, base);
    const uint32_t e = ctx.esp;
    Ranges r;
    r.Add(e - 0x40, 0x40);
    r.Add(X86_MEM32(e + 0xC), 16);
    VerifyAll("sub_0009C330", ctx, base, QuatMul, __imp__sub_0009C330, r, g_quat_calls, g_quat_bad, &g_quat_paths);
}

X86_FUNC(sub_00012600) {
    if (!g_synthetic_point.exchange(true)) SyntheticPoint(ctx, base);
    const uint32_t e = ctx.esp;
    Ranges r;
    r.Add(e - 0x40, 0x40);
    r.Add(ctx.eax, 16);
    VerifyAll("sub_00012600", ctx, base, TransformPoint, __imp__sub_00012600, r, g_point_calls, g_point_bad, &g_point_paths);
}

X86_FUNC(sub_0009C8D0) {
    if (!g_synthetic.exchange(true)) Synthetic(ctx, base);
    const uint32_t e = ctx.esp;
    Ranges r;
    r.Add(e - 4, 4);
    r.Add(X86_MEM32(e + 8), 16);
    VerifyAll("sub_0009C8D0", ctx, base, Normalize4, __imp__sub_0009C8D0, r, g_norm_calls, g_norm_bad, &g_norm_paths);
}

#endif
