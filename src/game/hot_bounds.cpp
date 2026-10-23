// Native replacement of the oriented box's world bounds (sub_0006FA50, thiscall: box in ecx; stack: matrix or 0,
// unused, out; ret 0x0C): the box's rows (times the matrix when there is one, through sub_00012500), then
// out[0] = centre - extent, out[1] = centre + extent with extent = |row0| * e.x + |row1| * e.y + |row2| * e.z + e.w,
// e the box's half sizes at +0x40. Returns 1.
//
// A transliteration with the x86 registers in locals: the original's stores (its frame, the abs rows, the scalars it
// passes through [esp+0x10..0x1C]), the same operand order, the call to sub_00012500 through the dispatcher symbol
// with the stack the original makes, and the registers and xmm it leaves. Build with BLACK_VERIFY_HOT to run the
// original after every call and count the calls where anything differs.
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "game/hot_ordered.h"
#include "game/hot_synth.h"
#include "game/hot_verify.h"

using namespace hot;

X86_EXTERN_FUNC(__imp__sub_0006FA50);
X86_EXTERN_FUNC(sub_00012500);

namespace {

#ifdef BLACK_VERIFY_HOT
PathCounts g_bounds_paths{{}, {"box rows", "times a matrix", "operands-in-frame"}};
#endif

inline bool Overlap(uint32_t a, uint64_t an, uint32_t b, uint64_t bn) {
    return uint64_t(a) < uint64_t(b) + bn && uint64_t(b) < uint64_t(a) + an;
}
inline f4 And(f4 a, uint32_t mask) {
    uint32_t x[4];
    std::memcpy(x, &a, 16);
    for (int i = 0; i < 4; i++) x[i] &= mask;
    std::memcpy(&a, x, 16);
    return a;
}

// Frame: push ebp, the alignment, sub esp, 0xC8, push esi, push edi (esp = s from then on)
static void BoxBounds(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp;
    const uint32_t A = (e - 4) & ~15u, s = A - 0xD0;
    const uint32_t box = ctx.ecx, matrix = X86_MEM32(e + 4), out = X86_MEM32(e + 0xC);
    const uint32_t lo = s - 0x100, n = e - lo;  // sub_00012500's frame below
    if (Overlap(box, 0x50, lo, n) || (matrix && Overlap(matrix, 0x40, lo, n)) || Overlap(out, 0x20, lo, n) ||
        Overlap(out, 0x20, box, 0x50)) {
        HOT_PATH(g_bounds_paths, 2);
        __imp__sub_0006FA50(ctx, base);
        return;
    }
    const uint32_t esi0 = ctx.esi, edi0 = ctx.edi;
    X86_MEM32(e - 4) = ctx.ebp;
    X86_MEM32(s + 4) = esi0;
    X86_MEM32(s) = edi0;
    uint32_t rows = box;
    if (matrix) {
        HOT_PATH(g_bounds_paths, 1);
        X86_MEM32(s - 4) = matrix;
        X86_MEM32(s - 8) = s + 0x90;
        X86_MEM32(s - 12) = 0x6FA75u;
        ctx.eax = s + 0x90;
        ctx.edi = box;
        ctx.ebp = e - 4;
        ctx.esp = s - 12;
        sub_00012500(ctx, base);
        rows = ctx.eax;
    } else {
        HOT_PATH(g_bounds_paths, 0);
    }
    const f4 r0 = Load4(base, rows), r1 = Load4(base, rows + 0x10), r2 = Load4(base, rows + 0x20),
             r3 = Load4(base, rows + 0x30);
    Store4(base, s + 0x20, r0);
    Store4(base, s + 0x30, r1);
    Store4(base, s + 0x40, r2);
    Store4(base, s + 0x50, r3);
    const f4 a2 = And(r2, 0x7FFFFFFFu), a1 = And(r1, 0x7FFFFFFFu), a0 = And(r0, 0x7FFFFFFFu);
    Store4(base, s + 0x60, a2);
    Store4(base, s + 0x70, a1);
    Store4(base, s + 0x80, a0);
    const uint32_t ex = X86_MEM32(box + 0x40), ey = X86_MEM32(box + 0x44), ez = X86_MEM32(box + 0x48),
                   ew = X86_MEM32(box + 0x4C);
    X86_MEM32(s + 0x18) = ez;
    X86_MEM32(s + 0x1C) = ey;
    X86_MEM32(s + 0x10) = ex;
    X86_MEM32(s + 0x14) = ew;
    const f4 x2 = VMulS(a2, F32(base, s + 0x18));
    const f4 x3 = VMulS(a1, F32(base, s + 0x1C));
    f4 x4 = VMulS(a0, F32(base, s + 0x10));
    const f4 w = Splat(F32(base, s + 0x14));
    x4 = VAdd(x4, x3);
    f4 x0 = VAdd(x4, x2);
    x0 = VAdd(x0, w);
    const f4 lo4 = VSub(r3, x0), hi4 = VAdd(r3, x0);
    Store4(base, out, lo4);
    Store4(base, out + 0x10, hi4);
    ctx.eax = 1;
    ctx.ecx = s + 0x1C;
    ctx.edx = s + 0x10;
    ctx.esi = esi0;
    ctx.edi = edi0;
    PutXmm(ctx.xmm[0], x0);
    PutXmm(ctx.xmm[1], hi4);
    PutXmm(ctx.xmm[2], lo4);
    PutXmm(ctx.xmm[3], x3);
    PutXmm(ctx.xmm[4], x4);
    ctx.ebp = X86_MEM32(e - 4);
    ctx.esp = e + 0x10;
}

}  // namespace

#ifndef BLACK_VERIFY_HOT

X86_FUNC(sub_0006FA50) {
    BoxBounds(ctx, base);
}

#else

namespace {
std::atomic<uint64_t> g_bounds_calls{0}, g_bounds_bad{0};
std::atomic<bool> g_synthetic{false};

// once, synthetic calls in the dead stack 0x800 below esp: a box (rows, half sizes at +0x40) and a matrix with NaNs
// of distinct bits in the lanes each NaN case names, with and without the matrix
void Synthetic(X86Context& live, uint8_t* base) {
    const uint32_t t = (live.esp - 0x800) & ~15u;
    for (int k = 0; k < kNanCaseCount; k++)
        for (int with = 0; with < 2; with++)
            SyntheticCall(std::string("sub_0006FA50 ") + kNanCases[k].what + (with ? ", times a matrix" : ""), live, base,
                          BoxBounds, __imp__sub_0006FA50, t, {}, [&](X86Context& c) {
                for (uint32_t i = 0; i < 4; i++) {
                    PutNanVector(base, t + 16 * i, 0.5f - float(i), 0.25f * float(i), -1.0f, i == 3 ? 1.0f : 0.0f, i, k);
                    PutNanVector(base, t + 0x60 + 16 * i, 1.0f + float(i), -0.5f, 0.75f, i == 3 ? 1.0f : 0.0f, 5 + i, k);
                }
                PutNanVector(base, t + 0x40, 2.0f, 3.0f, 0.5f, 0.125f, 4, k);
                c.ecx = t;
                c.esp = t - 0x40;
                X86_MEM32(c.esp) = 0;
                X86_MEM32(c.esp + 4) = with ? t + 0x60 : 0;
                X86_MEM32(c.esp + 8) = 0;
                X86_MEM32(c.esp + 0xC) = t + 0xC0;
            });
}
}  // namespace

X86_FUNC(sub_0006FA50) {
    if (!g_synthetic.exchange(true)) Synthetic(ctx, base);
    const uint32_t e = ctx.esp;
    Ranges r;
    r.Add(e - 0x200, 0x200);
    r.Add(X86_MEM32(e + 0xC), 0x20);
    VerifyAll("sub_0006FA50", ctx, base, BoxBounds, __imp__sub_0006FA50, r, g_bounds_calls, g_bounds_bad, &g_bounds_paths);
}

#endif
