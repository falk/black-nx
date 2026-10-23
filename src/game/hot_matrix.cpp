// Native replacements of the matrix leaves the game calls everywhere (transforms, skinning, the scene graph)
//
// Matrices are four 16-byte rows. A product row is sum_j a[i][j] * b[j], added left to right; each rewrite keeps the
// original's operand order and its stores (the frame, the rows it builds there) and leaves the registers, xmm and
// stack bytes as the original does. Build with BLACK_VERIFY_HOT to run the original after every call and count the
// calls where anything differs.
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "game/hot_ordered.h"
#include "game/hot_verify.h"

using namespace hot;

X86_EXTERN_FUNC(__imp__sub_00012500);
X86_EXTERN_FUNC(__imp__sub_00014DE0);
X86_EXTERN_FUNC(__imp__sub_00017100);

namespace {

#ifdef BLACK_VERIFY_HOT
PathCounts g_affine_paths{{}, {"native", "operands-in-frame", "w-nans-differ"}};
PathCounts g_inplace_paths{{}, {"native", "operands-in-frame", "w-nans-differ"}};
PathCounts g_mul4_paths{{}, {"native", "operands-in-frame"}};
PathCounts g_mulvec_paths{{}, {"vectors", "none"}};
#endif

inline void StF(uint8_t* p, float f) { std::memcpy(p, &f, 4); }
inline void StU(uint8_t* p, uint32_t u) { std::memcpy(p, &u, 4); }
inline bool Overlap(uint32_t a, uint64_t an, uint32_t b, uint64_t bn) {
    return uint64_t(a) < uint64_t(b) + bn && uint64_t(b) < uint64_t(a) + an;
}
inline f4 Bcast(f4 v, int i) { return Splat(v[i]); }

// rows of a * b, a's rows read as three scalars (movss, shufps 0) times b's first three rows, + b's fourth row in
// the last: the 3x4 affine product of sub_00012500 and sub_00014DE0. x1 ends as a[3][2] * b[2].
inline void AffineRows(uint8_t* base, uint32_t a, uint32_t b, f4 rows[4], f4& x1) {
    const f4 b0 = Load4(base, b), b1 = Load4(base, b + 0x10), b2 = Load4(base, b + 0x20), b3 = Load4(base, b + 0x30);
    for (int i = 0; i < 4; i++) {
        const uint32_t r = a + 16 * i;
        f4 x0 = VMul(Splat(F32(base, r)), b0);
        x1 = VMul(Splat(F32(base, r + 4)), b1);
        x0 = VAdd(x0, x1);
        x1 = VMul(Splat(F32(base, r + 8)), b2);
        x0 = VAdd(x0, x1);
        rows[i] = x0;
    }
    rows[3] = VAdd(rows[3], b3);
}

// rep movsd of 16 dwords from esi to edi, as the generated code does it
inline void Copy64(X86Context& __restrict ctx, uint8_t* base, uint32_t from, uint32_t to) {
    ctx.esi = from;
    ctx.edi = to;
    ctx.ecx = 16;
    x86_rep_movs(ctx, base, 4);
}

// sub_00012500: out = a * b, a 3x4 affine (thiscall a in ecx; stack out, b; ret 8)
// Frame: [esp+0x0C] a, [esp+0x10..0x4F] the rows, copied to out by rep movsd.
static void MulAffine(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp;
    const uint32_t s = ((e - 4) & ~15u) - 0x50;  // esp after the alignment, sub esp, 0x48 and two pushes
    const uint32_t a = ctx.ecx, out = X86_MEM32(e + 4), b = X86_MEM32(e + 8);
    if (Overlap(a, 0x3C, s, e - s) || Overlap(b, 0x40, s, e - s)) {
        HOT_PATH(g_affine_paths, 1);
        __imp__sub_00012500(ctx, base);
        return;
    }
    HOT_PATH(g_affine_paths, 0);
    uint8_t* fp = base + s;
    X86_MEM32(e - 4) = ctx.ebp;
    const uint32_t esi = ctx.esi, edi = ctx.edi;
    StU(fp + 4, esi);
    StU(fp, edi);
    StU(fp + 0xC, a);
    f4 rows[4], x1;
    AffineRows(base, a, b, rows, x1);
    for (int i = 0; i < 4; i++) Store4(base, s + 0x10 + 16 * i, rows[i]);
    Copy64(ctx, base, s + 0x10, out);
    ctx.eax = out;
    ctx.esi = esi;
    ctx.edi = edi;
    PutXmm(ctx.xmm[0], rows[3]);
    PutXmm(ctx.xmm[1], x1);
    ctx.esp = e + 12;
}

// sub_00014DE0: a = a * b, a 3x4 affine, then copied to out (thiscall a in ecx; stack out, b; ret 8)
// Frame: [esp+0x0C] a, [esp+0x10..0x4F] the rows, stored to a and then copied from a to out by rep movsd.
static void MulAffineInPlace(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp;
    const uint32_t s = ((e - 4) & ~15u) - 0x50;
    const uint32_t a = ctx.ecx, out = X86_MEM32(e + 4), b = X86_MEM32(e + 8);
    if (Overlap(a, 0x40, s, e - s) || Overlap(b, 0x40, s, e - s)) {
        HOT_PATH(g_inplace_paths, 1);
        __imp__sub_00014DE0(ctx, base);
        return;
    }
    HOT_PATH(g_inplace_paths, 0);
    uint8_t* fp = base + s;
    X86_MEM32(e - 4) = ctx.ebp;
    const uint32_t esi = ctx.esi, edi = ctx.edi;
    StU(fp + 4, esi);
    StU(fp, edi);
    StU(fp + 0xC, a);
    f4 rows[4], x1;
    AffineRows(base, a, b, rows, x1);
    for (int i = 0; i < 4; i++) Store4(base, s + 0x10 + 16 * i, rows[i]);
    for (int i = 0; i < 4; i++) Store4(base, a + 16 * i, rows[i]);
    Copy64(ctx, base, a, out);
    ctx.eax = out;
    ctx.esi = esi;
    ctx.edi = edi;
    PutXmm(ctx.xmm[0], rows[3]);
    PutXmm(ctx.xmm[1], x1);
    ctx.esp = e + 12;
}

// sub_00017100: out = a * b, 4x4 (eax out, ecx a, edx b; ret)
// Row i: ((b0 * a[i][0] + b1 * a[i][1]) + b2 * a[i][2]) + b3 * a[i][3], each a[i][j] passed through [esp+0x0C];
// b1 * a[i][1] through [esp+0x10] in rows 0 to 2. Frame: the rows at [esp+0x20..0x5F], copied to out by rep movsd.
static void Mul4(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp;
    const uint32_t s = ((e - 4) & ~15u) - 0x60;  // esp after the alignment, sub esp, 0x58 and two pushes
    const uint32_t out = ctx.eax, a = ctx.ecx, b = ctx.edx;
    if (Overlap(a, 0x40, s, e - s) || Overlap(b, 0x40, s, e - s)) {
        HOT_PATH(g_mul4_paths, 1);
        __imp__sub_00017100(ctx, base);
        return;
    }
    HOT_PATH(g_mul4_paths, 0);
    uint8_t* fp = base + s;
    X86_MEM32(e - 4) = ctx.ebp;
    const uint32_t esi = ctx.esi, edi = ctx.edi;
    StU(fp + 4, esi);
    StU(fp, edi);
    const f4 b0 = Load4(base, b), b1 = Load4(base, b + 0x10), b2 = Load4(base, b + 0x20), b3 = Load4(base, b + 0x30);
    f4 x0 = b3, x1 = b2, x2 = b1, x3 = b0, x4, x5, x6, x7, t1;
    for (int i = 0; i < 3; i++) {
        const uint32_t r = a + 16 * i;
        x5 = VMul(x0, Splat(F32(base, r + 0xC)));
        x6 = VMul(x1, Splat(F32(base, r + 8)));
        t1 = VMul(x2, Splat(F32(base, r + 4)));
        x7 = Splat(F32(base, r));
        x4 = VMul(x3, x7);
        x4 = VAdd(x4, t1);
        x4 = VAdd(x4, x6);
        x4 = VAdd(x4, x5);
        Store4(base, s + 0x20 + 16 * i, x4);
    }
    const uint32_t r = a + 0x30;
    x0 = VMul(x0, Splat(F32(base, r + 0xC)));
    x1 = VMul(x1, Splat(F32(base, r + 8)));
    x2 = VMul(x2, Splat(F32(base, r + 4)));
    const float a30 = F32(base, r);
    x4 = Splat(a30);
    x3 = VMul(x3, x4);
    x3 = VAdd(x3, x2);
    x3 = VAdd(x3, x1);
    x3 = VAdd(x3, x0);
    StF(fp + 0xC, a30);
    Store4(base, s + 0x10, t1);
    Store4(base, s + 0x50, x3);
    Copy64(ctx, base, s + 0x20, out);
    ctx.edx = s + 0xC;
    ctx.esi = esi;
    ctx.edi = edi;
    PutXmm(ctx.xmm[0], x0);
    PutXmm(ctx.xmm[1], x1);
    PutXmm(ctx.xmm[2], x2);
    PutXmm(ctx.xmm[3], x3);
    PutXmm(ctx.xmm[4], x4);
    PutXmm(ctx.xmm[5], x5);
    PutXmm(ctx.xmm[6], x6);
    PutXmm(ctx.xmm[7], x7);
    ctx.esp = e + 4;
}

// sub_00022424: out = a * b, 4x4, D3DX style (stdcall out, a, b; ret 0x0C; eax and ecx out)
// Row i: ((a[i][0] * b0 + a[i][1] * b1) + a[i][2] * b2) + a[i][3] * b3, stored after all four rows.
static void Mul4Stdcall(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp;
    const uint32_t a = X86_MEM32(e + 8), b = X86_MEM32(e + 0xC);
    const f4 b0 = Load4(base, b), b1 = Load4(base, b + 0x10), b2 = Load4(base, b + 0x20), b3 = Load4(base, b + 0x30);
    f4 rows[4], x0, x1;
    for (int i = 0; i < 4; i++) {
        const f4 ai = Load4(base, a + 16 * i);
        f4 x = VMul(Bcast(ai, 0), b0);
        x1 = VMul(Bcast(ai, 1), b1);
        x0 = VMul(Bcast(ai, 2), b2);
        x = VAdd(x, x1);
        x1 = VMul(Bcast(ai, 3), b3);
        x = VAdd(x, x0);
        x = VAdd(x, x1);
        rows[i] = x;
    }
    const uint32_t out = X86_MEM32(e + 4);
    for (int i = 0; i < 4; i++) Store4(base, out + 16 * i, rows[i]);
    ctx.eax = out;
    ctx.ecx = out;
    PutXmm(ctx.xmm[0], x0);
    PutXmm(ctx.xmm[1], x1);
    PutXmm(ctx.xmm[2], rows[0]);
    PutXmm(ctx.xmm[3], rows[1]);
    PutXmm(ctx.xmm[4], rows[2]);
    PutXmm(ctx.xmm[5], rows[3]);
    ctx.esp = e + 16;
}

// sub_0009C940: out[k] = m * in[k] for n vectors (cdecl m, in, out, n), one vector after another (in and out may
// be the same array): ((m0 v.x + m1 v.y) ... added as m3 v.w + (m2 v.z + (m1 v.y + m0 v.x))
// Registers: xmm1 to xmm4 the rows of m; eax, ecx past the arrays and edx 0 when n > 0.
static void MulVectors(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp;
    X86_MEM32(e - 4) = ctx.ebp;
    const uint32_t m = X86_MEM32(e + 4);
    const f4 m0 = Load4(base, m), m1 = Load4(base, m + 0x10), m2 = Load4(base, m + 0x20), m3 = Load4(base, m + 0x30);
    uint32_t n = X86_MEM32(e + 0x10), in = X86_MEM32(e + 8), out = X86_MEM32(e + 0xC);
    PutXmm(ctx.xmm[1], m0);
    PutXmm(ctx.xmm[2], m1);
    PutXmm(ctx.xmm[3], m2);
    PutXmm(ctx.xmm[4], m3);
    if (int32_t(n) > 0) {
        HOT_PATH(g_mulvec_paths, 0);
        f4 x0, x5, x6, x7;
        do {
            const f4 v = Load4(base, in);
            x6 = VMul(m0, Bcast(v, 0));
            x7 = VMul(m1, Bcast(v, 1));
            x7 = VAdd(x7, x6);
            x6 = VMul(m2, Bcast(v, 2));
            x5 = Bcast(v, 3);
            x6 = VAdd(x6, x7);
            x0 = VMul(m3, x5);
            x0 = VAdd(x0, x6);
            Store4(base, out, x0);
            in += 16;
            out += 16;
        } while (--n);
        PutXmm(ctx.xmm[0], x0);
        PutXmm(ctx.xmm[5], x5);
        PutXmm(ctx.xmm[6], x6);
        PutXmm(ctx.xmm[7], x7);
    } else {
        HOT_PATH(g_mulvec_paths, 1);
    }
    ctx.eax = in;
    ctx.ecx = out;
    ctx.edx = n;
    ctx.esp = e + 4;
}

}  // namespace

#ifndef BLACK_VERIFY_HOT

X86_FUNC(sub_00012500) { MulAffine(ctx, base); }
X86_FUNC(sub_00014DE0) { MulAffineInPlace(ctx, base); }
X86_FUNC(sub_00017100) { Mul4(ctx, base); }
X86_FUNC(sub_00022424) { Mul4Stdcall(ctx, base); }
X86_FUNC(sub_0009C940) { MulVectors(ctx, base); }

#else

X86_EXTERN_FUNC(__imp__sub_00022424);
X86_EXTERN_FUNC(__imp__sub_0009C940);

namespace {

std::atomic<uint64_t> g_affine_calls{0}, g_affine_bad{0};
std::atomic<uint64_t> g_inplace_calls{0}, g_inplace_bad{0};
std::atomic<uint64_t> g_mul4_calls{0}, g_mul4_bad{0};
std::atomic<uint64_t> g_mul4s_calls{0}, g_mul4s_bad{0};
std::atomic<uint64_t> g_mulvec_calls{0}, g_mulvec_bad{0};

// b's w column holds NaNs of different bits: the case where the operand order of the adds decides the result
void CountNanW(uint8_t* base, uint32_t b, PathCounts& paths) {
    uint32_t w[4];
    for (int i = 0; i < 4; i++) w[i] = X86_MEM32(b + 16 * i + 12);
    auto nan = [](uint32_t u) { return (u & 0x7F800000u) == 0x7F800000u && (u & 0x7FFFFFu); };
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++)
            if (nan(w[i]) && nan(w[j]) && w[i] != w[j]) {
                HOT_PATH(paths, 2);
                return;
            }
}

// once per function, a synthetic call: matrices whose w column holds quiet NaNs of different bits in every row,
// where the operand order of each add and multiply decides the NaN a lane ends with. In the dead stack 0x800 below
// esp: a at T, b at T+0x40, out at T+0x80, four vectors at T+0xC0, their products at T+0x100, the call's esp at
// T-0x40.
enum Kind { kAffine, kInPlace, kMul4, kMul4s, kVectors };
void Synthetic(const char* name, X86Context& ctx, uint8_t* base, void (*native)(X86Context&, uint8_t*),
               void (*original)(X86Context&, uint8_t*), Kind kind) {
    const uint32_t t = (ctx.esp - 0x800) & ~15u, e = t - 0x40;
    VerifyCase(name, ctx, base, native, original, t - 0x200, 0x400, [&](X86Context& c) {
        static const uint32_t wa[4] = {0xFFFFFFFFu, 0x7FFFFFFFu, 0xFFC00001u, 0x7FC00002u};
        static const uint32_t wb[4] = {0xFFC12345u, 0x7FC54321u, 0xFFFFFFFFu, 0x7FFFFFFFu};
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 3; j++) {
                X86_MEMF32(t + 16 * i + 4 * j) = 0.25f * float(i + 1) - 0.5f * float(j);
                X86_MEMF32(t + 0x40 + 16 * i + 4 * j) = 1.5f - 0.125f * float(i * 3 + j);
                X86_MEMF32(t + 0xC0 + 16 * i + 4 * j) = 2.0f + float(i) - 0.75f * float(j);
            }
            X86_MEM32(t + 16 * i + 12) = wa[i];
            X86_MEM32(t + 0x40 + 16 * i + 12) = wb[i];
            X86_MEM32(t + 0xC0 + 16 * i + 12) = wa[3 - i];
        }
        X86_MEM32(e) = 0;
        switch (kind) {
            case kAffine:
            case kInPlace:
                c.ecx = t;
                X86_MEM32(e + 4) = t + 0x80;
                X86_MEM32(e + 8) = t + 0x40;
                break;
            case kMul4:
                c.eax = t + 0x80;
                c.ecx = t;
                c.edx = t + 0x40;
                break;
            case kMul4s:
                X86_MEM32(e + 4) = t + 0x80;
                X86_MEM32(e + 8) = t;
                X86_MEM32(e + 0xC) = t + 0x40;
                break;
            case kVectors:
                X86_MEM32(e + 4) = t + 0x40;
                X86_MEM32(e + 8) = t + 0xC0;
                X86_MEM32(e + 0xC) = t + 0x100;
                X86_MEM32(e + 0x10) = 4;
                break;
        }
        c.esp = e;
    });
}

std::atomic<int> g_synthetic{0};
inline void Once(int bit, const char* name, X86Context& ctx, uint8_t* base, void (*native)(X86Context&, uint8_t*),
                 void (*original)(X86Context&, uint8_t*), Kind kind) {
    if (!(g_synthetic.fetch_or(1 << bit) & (1 << bit))) Synthetic(name, ctx, base, native, original, kind);
}

}  // namespace

X86_FUNC(sub_00012500) {
    Once(0, "sub_00012500", ctx, base, MulAffine, __imp__sub_00012500, kAffine);
    const uint32_t e = ctx.esp;
    Ranges r;
    r.Add(e - 0x70, 0x70);
    r.Add(X86_MEM32(e + 4), 0x40);
    const uint32_t b = X86_MEM32(e + 8);
    CountNanW(base, b, g_affine_paths);
    VerifyAll("sub_00012500", ctx, base, MulAffine, __imp__sub_00012500, r, g_affine_calls, g_affine_bad,
              &g_affine_paths);
}

X86_FUNC(sub_00014DE0) {
    Once(1, "sub_00014DE0", ctx, base, MulAffineInPlace, __imp__sub_00014DE0, kInPlace);
    const uint32_t e = ctx.esp;
    Ranges r;
    r.Add(e - 0x70, 0x70);
    r.Add(ctx.ecx, 0x40);
    r.Add(X86_MEM32(e + 4), 0x40);
    const uint32_t b = X86_MEM32(e + 8);
    CountNanW(base, b, g_inplace_paths);
    VerifyAll("sub_00014DE0", ctx, base, MulAffineInPlace, __imp__sub_00014DE0, r, g_inplace_calls, g_inplace_bad,
              &g_inplace_paths);
}

X86_FUNC(sub_00017100) {
    Once(2, "sub_00017100", ctx, base, Mul4, __imp__sub_00017100, kMul4);
    const uint32_t e = ctx.esp;
    Ranges r;
    r.Add(e - 0x80, 0x80);
    r.Add(ctx.eax, 0x40);
    VerifyAll("sub_00017100", ctx, base, Mul4, __imp__sub_00017100, r, g_mul4_calls, g_mul4_bad, &g_mul4_paths);
}

X86_FUNC(sub_00022424) {
    Once(3, "sub_00022424", ctx, base, Mul4Stdcall, __imp__sub_00022424, kMul4s);
    Ranges r;
    r.Add(X86_MEM32(ctx.esp + 4), 0x40);
    VerifyAll("sub_00022424", ctx, base, Mul4Stdcall, __imp__sub_00022424, r, g_mul4s_calls, g_mul4s_bad, nullptr);
}

X86_FUNC(sub_0009C940) {
    Once(4, "sub_0009C940", ctx, base, MulVectors, __imp__sub_0009C940, kVectors);
    const uint32_t e = ctx.esp, n = X86_MEM32(e + 0x10);
    Ranges r;
    r.Add(e - 4, 4);
    if (int32_t(n) > 0) r.Add(X86_MEM32(e + 0xC), 16 * (n < 0x1000 ? n : 0x1000));
    VerifyAll("sub_0009C940", ctx, base, MulVectors, __imp__sub_0009C940, r, g_mulvec_calls, g_mulvec_bad,
              &g_mulvec_paths);
}

#endif
