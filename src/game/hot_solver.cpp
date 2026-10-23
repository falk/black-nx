// Native replacements of the rigid-body solver's hot path: the sequential-impulse iterations (sub_00076890,
// sub_00076F20) and the per-contact Jacobian setup (sub_0007AFE0)
//
// Each one leaves the guest exactly as the original does: the same velocities and accumulated impulses, the same
// registers and the same stack bytes. The arithmetic is the original's, operation by operation and in its order
// (no reassociation, no fused multiply-add), so every rounding matches. Build with BLACK_VERIFY_HOT to run the original
// after every call and count the calls where anything differs.
//
// The solver object (ecx, thiscall, no stack arguments):
//   +0x08  bodies: 64-byte velocity records, [+0x00] and [+0x10] linear terms, [+0x20] and [+0x30] angular terms
//   +0x0C  contact rows, 256 bytes each       +0x30  their count (never 0 when called: sub_00075640 checks)
//   +0x10  joint rows, 0x180 bytes each       +0x38  their count (never 0 for sub_00076F20)
//   +0x398 iterations (the loop runs at least once)
// A row names body A by index at +0x0C and body B at +0x1C; B takes part when bit 0 of the flag word is set
// (+0x2C in a contact row, +0xBC in a joint row).
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "game/hot_ordered.h"

using namespace hot;

// NaN results: when both operands of an addps, addss, mulps or mulss are NaN, the x86 result is the first operand's
// (quieted). Every add and multiply here takes rows and velocities from guest memory on both sides, so all of them
// go through the ordered helpers of hot_ordered.h (VAdd, VMul, SAdd, SMul) in the original's operand order (the
// destination register first). Subtractions, divisions, minps/maxps and compares are not commutative and stay as
// written; the selects (Maxps, Minps, the cmpps masks) give the x86 result for NaNs by construction.

namespace {

typedef int32_t i4 __attribute__((vector_size(16)));

// shufps a, a, imm (Sh of hot_ordered.h with both operands the same)
using hot::Sh;
template <int I>
inline f4 Sh(f4 a) {
    return hot::Sh<I>(a, a);
}
inline f4 Bcast0(f4 a) { return __builtin_shufflevector(a, a, 0, 0, 0, 0); }  // movss x, y + shufps x, x, 0
inline f4 And(f4 a, f4 b) { return (f4)((i4)a & (i4)b); }
inline f4 AndNot(f4 mask, f4 b) { return (f4)(~(i4)mask & (i4)b); }  // andnps mask, b
inline f4 Xor(f4 a, f4 b) { return (f4)((i4)a ^ (i4)b); }
inline f4 CmpLt(f4 a, f4 b) { return (f4)(a < b); }      // cmpps 1: false on NaN
inline f4 CmpNlt(f4 a, f4 b) { return (f4)(~(a < b)); }  // cmpps 5: true on NaN
inline f4 Sel(i4 m, f4 a, f4 b) { return (f4)((m & (i4)a) | (~m & (i4)b)); }
inline f4 Maxps(f4 a, f4 b) { return Sel(a > b, a, b); }  // b when either is NaN or both are zero
inline f4 Minps(f4 a, f4 b) { return Sel(a < b, a, b); }

// ways through a row, counted in the verify build only (the native run of each verified call)
#ifdef BLACK_VERIFY_HOT
struct Paths {
    uint64_t rows_b, rows_no_b, hi_kept, hi_replaced, lo_kept, lo_replaced;
    uint64_t jrows_b, jrows_no_b, max_kept, max_zero, min_kept, min_zero;
};
Paths g_paths;
uint64_t g_setup_paths[3];  // sub_0007AFE0: both bodies move, A alone, B alone
inline uint64_t Lanes12(f4 m) { return uint64_t(((i4)m)[1] != 0) + uint64_t(((i4)m)[2] != 0); }
inline uint64_t Lanes(i4 m) {
    return uint64_t(m[0] != 0) + uint64_t(m[1] != 0) + uint64_t(m[2] != 0) + uint64_t(m[3] != 0);
}
#define PATH(stmt) stmt
#else
#define PATH(stmt)
#endif

// guest memory through a host pointer, so offsets within a row or a body become immediates
inline f4 Ld(const uint8_t* p) {
    f4 v;
    std::memcpy(&v, p, 16);
    return v;
}
inline void St(uint8_t* p, f4 v) { std::memcpy(p, &v, 16); }
inline float LdF(const uint8_t* p) {
    float f;
    std::memcpy(&f, p, 4);
    return f;
}
inline uint32_t Ld32(const uint8_t* p) {
    uint32_t u;
    std::memcpy(&u, p, 4);
    return u;
}

struct Xmm {
    f4 x0, x1, x2, x3, x4, x5, x6, x7;
};

// One 256-byte contact row at r (sub_00076890 0x768D0..0x76B87, sub_00076F20 0x76F70..0x77227): the relative velocity
// along the row's three axes, the accumulated impulse at +0x50 updated and clamped lane by lane to
// [-[+0x4C] * f, [+0x3C] * f] with f = {0, -n, -n, 0} built from lane 0 of the old impulse (cmpps / and / andnot, so a
// NaN bound keeps the other side), the change applied to A (+) and B (-).
//   abs_mask: andps constant (0x285C40 or 0x285D60); sign: xorps constant (0x285C50 or 0x285D70)
// eax is only set when B takes part; edx gets the flag bit; edi body A.
__attribute__((always_inline)) static inline void ContactRow(uint8_t* base, uint32_t r, uint32_t bodies, f4 abs_mask,
                                                             f4 sign, Xmm& x, uint32_t& eax, uint32_t& edx,
                                                             uint32_t& edi) {
    f4 x0, x1, x2, x3, x4, x5, x6, x7;
    uint8_t* const rp = base + r;
    uint8_t* bp = nullptr;
    edi = (Ld32(rp + 0x0C) << 6) + bodies;
    uint8_t* const ap = base + edi;
    x0 = Sh<0x09>(Ld(rp));
    x1 = Sh<0x12>(Ld(ap + 0x20));
    x2 = Sh<0x12>(Ld(ap + 0x30));
    x6 = VMul(x0, x1);
    x7 = VMul(x0, x2);
    edx = Ld32(rp + 0x2C) & 1;
    x0 = Sh<0x09>(x0);
    x1 = Sh<0x12>(x1);
    x2 = Sh<0x12>(x2);
    x6 = x6 - Ld(ap);
    x7 = x7 - Ld(ap + 0x10);
    x1 = VMul(x1, x0);
    x2 = VMul(x2, x0);
    x6 = x6 - x1;
    x7 = x7 - x2;
    PATH(edx ? ++g_paths.rows_b : ++g_paths.rows_no_b);
    if (edx) {
        eax = (Ld32(rp + 0x1C) << 6) + bodies;
        bp = base + eax;
        x0 = Sh<0x12>(Ld(rp + 0x10));
        x1 = Sh<0x09>(Ld(bp + 0x20));
        x2 = Sh<0x09>(Ld(bp + 0x30));
        x3 = VMul(x0, x1);
        x4 = VMul(x0, x2);
        x0 = Sh<0x12>(x0);
        x1 = Sh<0x09>(x1);
        x2 = Sh<0x09>(x2);
        x6 = VAdd(x6, x3);
        x7 = VAdd(x7, x4);
        x1 = VMul(x1, x0);
        x2 = VMul(x2, x0);
        x6 = VAdd(x6, Ld(bp));
        x7 = VAdd(x7, Ld(bp + 0x10));
        x6 = x6 - x1;
        x7 = x7 - x2;
    }

    // the three axes' relative speeds: lanes 1..3 from x6 (sum of both halves), lane 0 from the second half alone
    x6 = VAdd(x6, x7);
    x2 = x7;
    x0 = Sh<0x24>(Ld(rp + 0x20));
    x1 = Bcast0(x6);
    x7 = f4{x2[0], 0, 0, 0};
    x1 = VMul(x1, x0);
    x3 = Ld(rp + 0x30);
    x7[0] = SMul(x7[0], x0[0]);
    x4 = x3;
    x6 = Sh<0x95>(x6);
    x2 = Sh<0x09>(x2);
    x3 = VMul(x3, x6);
    x4[0] = SMul(x4[0], x2[0]);
    x0 = Ld(rp + 0x40);
    x1 = VAdd(x1, x3);
    x7[0] = SAdd(x7[0], x4[0]);
    x6 = Sh<0xFF>(x6);
    x2 = Sh<0x09>(x2);
    x6 = VMul(x6, x0);
    x2[0] = SMul(x2[0], x0[0]);
    x6 = VAdd(x6, x1);
    x7[0] = SAdd(x7[0], x2[0]);
    x6 = And(x6, abs_mask);
    x7 = Sh<0x3F>(x7);
    x7 = VAdd(x7, x6);
    x6 = Ld(rp + 0x50);  // the accumulated impulse
    x7 = VAdd(x7, Ld(rp + 0x60));
    x5 = f4{0, 0, 0, 0};
    x5[0] = x5[0] - x6[0];
    x7 = VAdd(x7, x6);
    x5 = Sh<0x41>(x5);
    x0 = Splat(LdF(rp + 0x3C));
    x1 = Splat(LdF(rp + 0x4C));
    x0 = VMul(x0, x5);
    x1 = VMul(x1, x5);
    x2 = CmpLt(x0, x7);  // the upper bound
    PATH(g_paths.hi_kept += Lanes12(x2); g_paths.hi_replaced += 2 - Lanes12(x2));
    x7 = And(x7, x2);
    x2 = AndNot(x2, x1);
    x0 = Xor(x0, sign);
    x7 = VAdd(x7, x2);
    x1 = Xor(x1, sign);
    x0 = CmpNlt(x0, x7);  // the lower bound
    PATH(g_paths.lo_kept += Lanes12(x0); g_paths.lo_replaced += 2 - Lanes12(x0));
    x7 = And(x7, x0);
    x0 = AndNot(x0, x1);
    x7 = VAdd(x7, x0);
    St(rp + 0x50, x7);
    x7 = x7 - x6;  // the impulse applied this pass

    // body A: linear += [+0x70..0x90] * impulse * [+0xAC], angular += [+0xA0..0xC0] * impulse
    x5 = Sh<0x24>(Ld(rp + 0x70));
    x2 = Sh<0x40>(x7);
    x3 = Sh<0xBF>(x7);
    x6 = x5;
    x5 = VMul(x5, x2);
    x6 = VMul(x6, x3);
    x0 = Sh<0x24>(Ld(rp + 0x80));
    x2 = Sh<0xFF>(x2);
    x2 = VMul(x2, x0);
    x0 = Sh<0x24>(Ld(rp + 0x90));
    x3 = Sh<0xFF>(x3);
    x3 = VMul(x3, x0);
    x5 = VAdd(x5, x2);
    x5 = VAdd(x5, x3);
    x1 = Splat(LdF(rp + 0xAC));
    x2 = Ld(ap);
    x3 = Ld(ap + 0x10);
    x0 = x5;
    x0 = VMul(x0, x1);
    x1 = VMul(x1, x6);
    x2 = VAdd(x2, x0);
    x3 = VAdd(x3, x1);
    St(ap, x2);
    St(ap + 0x10, x3);
    x0 = Ld(rp + 0xA0);
    x2 = Sh<0x40>(x7);
    x1 = x0;
    x3 = Sh<0xBF>(x7);
    x0 = VMul(x0, x2);
    x1 = VMul(x1, x3);
    x2 = Sh<0xFF>(x2);
    x3 = Sh<0xFF>(x3);
    x2 = VMul(x2, Ld(rp + 0xB0));
    x3 = VMul(x3, Ld(rp + 0xC0));
    x0 = VAdd(x0, x2);
    x0 = VAdd(x0, x3);
    x2 = Ld(ap + 0x20);
    x3 = Ld(ap + 0x30);
    x2 = VAdd(x2, x0);
    x3 = VAdd(x3, x1);
    St(ap + 0x20, x2);
    St(ap + 0x30, x3);

    // body B: linear -= the same * [+0xDC], angular -= [+0xD0..0xF0] * impulse
    if (edx) {
        const float mb = LdF(rp + 0xDC);
        x2 = Ld(bp);
        x3 = Ld(bp + 0x10);
        x5 = VMulS(x5, mb);  // x1 = [+0xDC] in every lane, overwritten below
        x6 = VMulS(x6, mb);
        x2 = x2 - x5;
        x3 = x3 - x6;
        St(bp, x2);
        St(bp + 0x10, x3);
        x0 = Ld(rp + 0xD0);
        x2 = Sh<0x40>(x7);
        x1 = x0;
        x7 = Sh<0xBF>(x7);
        x0 = VMul(x0, x2);
        x1 = VMul(x1, x7);
        x2 = Sh<0xFF>(x2);
        x7 = Sh<0xFF>(x7);
        x2 = VMul(x2, Ld(rp + 0xE0));
        x7 = VMul(x7, Ld(rp + 0xF0));
        x0 = VAdd(x0, x2);
        x0 = VAdd(x0, x7);
        x2 = Ld(bp + 0x20);
        x3 = Ld(bp + 0x30);
        x2 = x2 - x0;
        x3 = x3 - x1;
        St(bp + 0x20, x2);
        St(bp + 0x30, x3);
    }
    x = Xmm{x0, x1, x2, x3, x4, x5, x6, x7};
}

// One 0x180-byte joint row at r (sub_00076F20 0x77250..0x77515): A's linear velocity and its negated angular
// velocity (B's added), mapped through [+0x20..0x70] and offset by [+0x80], [+0x90], clamped per lane to >= 0 (maxps)
// or <= 0 (minps) against the accumulated impulses at +0x80 and +0x90 (lanes 0, 1 and 3 of each kept from the sums,
// lane 2 the old value); the change applied to A (+) and B (-).
__attribute__((always_inline)) static inline void JointRow(uint8_t* base, uint32_t r, uint32_t bodies, Xmm& x,
                                                           uint32_t& eax, uint32_t& edx, uint32_t& edi) {
    f4 x0, x1, x2, x3, x4, x5, x6, x7;
    uint8_t* const rp = base + r;
    uint8_t* bp = nullptr;
    const f4 zero = {0, 0, 0, 0};
    x7 = zero;
    edi = (Ld32(rp + 0x0C) << 6) + bodies;
    uint8_t* const ap = base + edi;
    x0 = Ld(rp);
    x1 = Ld(ap + 0x20);
    x6 = x1;
    x0 = Sh<0x09>(x0);
    x6 = Sh<0x12>(x6);
    edx = Ld32(rp + 0xBC) & 1;
    x6 = VMul(x6, x0);
    x0 = Sh<0x09>(x0);
    x1 = Sh<0x09>(x1);
    x6 = x6 - Ld(ap);
    x1 = VMul(x1, x0);
    x7 = x7 - Ld(ap + 0x20);
    x6 = x6 - x1;
    PATH(edx ? ++g_paths.jrows_b : ++g_paths.jrows_no_b);
    if (edx) {
        eax = (Ld32(rp + 0x1C) << 6) + bodies;
        bp = base + eax;
        x0 = Ld(rp + 0x10);
        x1 = Ld(bp + 0x20);
        x2 = x1;
        x0 = Sh<0x09>(x0);
        x2 = Sh<0x12>(x2);
        x2 = VMul(x2, x0);
        x0 = Sh<0x09>(x0);
        x1 = Sh<0x09>(x1);
        x6 = VAdd(x6, Ld(bp));
        x1 = VMul(x1, x0);
        x7 = VAdd(x7, Ld(bp + 0x20));
        x2 = x2 - x1;
        x6 = x6 - x2;
    }

    x0 = Ld(rp + 0x20);
    x1 = Ld(rp + 0x30);
    x4 = Bcast0(x6);
    x5 = Bcast0(x7);
    x4 = VMul(x4, x0);
    x5 = VMul(x5, x1);
    x2 = Ld(rp + 0x40);
    x3 = Ld(rp + 0x50);
    x0 = Sh<0xFF>(x0, x2);
    x1 = Sh<0xFF>(x1, x3);
    x6 = Sh<0x95>(x6);
    x7 = Sh<0x95>(x7);
    x2 = VMul(x2, x6);
    x3 = VMul(x3, x7);
    x4 = VAdd(x4, x2);
    x5 = VAdd(x5, x3);
    x2 = Ld(rp + 0x60);
    x3 = Ld(rp + 0x70);
    x0 = Sh<0xF8>(x0, x2);
    x1 = Sh<0xF8>(x1, x3);
    x6 = Sh<0xFF>(x6);
    x7 = Sh<0xFF>(x7);
    x6 = VMul(x6, x2);
    x7 = VMul(x7, x3);
    x6 = VAdd(x6, x4);
    x7 = VAdd(x7, x5);
    x4 = Ld(rp + 0x80);  // the accumulated impulses
    x3 = Scalar(LdF(rp + 0x8C));
    x5 = Ld(rp + 0x90);
    x2 = Ld(rp + 0xA0);
    x3 = Sh<0xF0>(x3, x5);
    x3 = Sh<0xF8>(x3, x2);
    x6 = VAdd(x6, x4);
    x7 = VAdd(x7, x5);
    x0 = VAdd(x0, x6);
    x6 = VAdd(x6, x1);
    x1 = zero;
    PATH(g_paths.max_kept += Lanes(x0 > x1) + Lanes(x2 + x7 > x1);
         g_paths.min_kept += Lanes(x6 < x1) + Lanes(x7 + x3 < x1));
    x0 = Maxps(x0, x1);
    x6 = Minps(x6, x1);
    x2 = VAdd(x2, x7);
    x7 = VAdd(x7, x3);
    x2 = Maxps(x2, x1);
    x7 = Minps(x7, x1);
    x6 = VAdd(x6, x0);
    x7 = VAdd(x7, x2);
    x0 = Sh<0xFA>(x6, x4);
    x1 = Sh<0xFA>(x7, x5);
    x6 = Sh<0x84>(x6, x0);
    x7 = Sh<0x84>(x7, x1);
    St(rp + 0x80, x6);
    St(rp + 0x90, x7);
    x6 = x6 - x4;  // the impulses applied this pass
    x7 = x7 - x5;

    // body A
    x5 = Bcast0(x6);
    x2 = Sh<0x24>(Ld(rp + 0xB0));
    x1 = x6;
    x5 = VMul(x5, x2);
    x0 = Sh<0x24>(Ld(rp + 0xC0));
    x1 = Sh<0x95>(x1);
    x0 = VMul(x0, x1);
    x1 = Sh<0xFF>(x1);
    x5 = VAdd(x5, x0);
    x1 = VMul(x1, Ld(rp + 0xD0));
    x5 = VAdd(x5, x1);
    x0 = Splat(LdF(rp + 0xEC));
    x1 = Ld(ap);
    x0 = VMul(x0, x5);
    x1 = VAdd(x1, x0);
    St(ap, x1);
    x0 = Ld(rp + 0xE0);
    x4 = Ld(rp + 0xF0);
    x2 = Sh<0x80>(x6);
    x3 = Sh<0x55>(x6);
    x0 = VMul(x0, x2);
    x3 = VMul(x3, x4);
    x2 = Sh<0xFF>(x2);
    x1 = Ld(rp + 0x100);
    x3 = VAdd(x3, x0);
    x2 = VMul(x2, x1);
    x4 = Sh<0xFF>(x4, x1);
    x3 = VAdd(x3, x2);
    x0 = Bcast0(x7);
    x1 = Ld(rp + 0x110);
    x4 = Sh<0xF8>(x4, x1);
    x2 = x7;
    x0 = VMul(x0, x1);
    x2 = Sh<0x95>(x2);
    x1 = Ld(rp + 0x120);
    x3 = VAdd(x3, x0);
    x1 = VMul(x1, x2);
    x2 = Sh<0xFF>(x2);
    x3 = VAdd(x3, x1);
    x4 = VMul(x4, x2);
    x0 = Ld(ap + 0x20);
    x3 = VAdd(x3, x4);
    x0 = VAdd(x0, x3);
    St(ap + 0x20, x0);

    // body B
    if (edx) {
        x0 = Splat(LdF(rp + 0x13C));
        x1 = Ld(bp);
        x0 = VMul(x0, x5);
        x1 = x1 - x0;
        St(bp, x1);
        x0 = Ld(rp + 0x130);
        x4 = Ld(rp + 0x140);
        x2 = Sh<0x80>(x6);
        x3 = Sh<0x55>(x6);
        x0 = VMul(x0, x2);
        x3 = VMul(x3, x4);
        x2 = Sh<0xFF>(x2);
        x1 = Ld(rp + 0x150);
        x3 = VAdd(x3, x0);
        x2 = VMul(x2, x1);
        x4 = Sh<0xFF>(x4, x1);
        x3 = VAdd(x3, x2);
        x0 = Bcast0(x7);
        x1 = Ld(rp + 0x160);
        x4 = Sh<0xF8>(x4, x1);
        x2 = x7;
        x0 = VMul(x0, x1);
        x2 = Sh<0x95>(x2);
        x1 = Ld(rp + 0x170);
        x3 = VAdd(x3, x0);
        x1 = VMul(x1, x2);
        x2 = Sh<0xFF>(x2);
        x3 = VAdd(x3, x1);
        x4 = VMul(x4, x2);
        x0 = Ld(bp + 0x20);
        x3 = VAdd(x3, x4);
        x0 = x0 - x3;
        St(bp + 0x20, x0);
    }
    x = Xmm{x0, x1, x2, x3, x4, x5, x6, x7};
}

inline void PutAll(X86Context& ctx, const Xmm& x) {
    PutXmm(ctx.xmm[0], x.x0);
    PutXmm(ctx.xmm[1], x.x1);
    PutXmm(ctx.xmm[2], x.x2);
    PutXmm(ctx.xmm[3], x.x3);
    PutXmm(ctx.xmm[4], x.x4);
    PutXmm(ctx.xmm[5], x.x5);
    PutXmm(ctx.xmm[6], x.x6);
    PutXmm(ctx.xmm[7], x.x7);
}

// sub_00076890: the solver's iterations over the contact rows alone
//   frame (ebp = esp - 4): [ebp-0x14] rows, [ebp-0x10] row count, [ebp-0x0C] bodies, [ebp-0x08] iterations,
//   [ebp-0x04] the iteration index (last stored: iterations - 1, or 0), then saved esi, edi
//   out: eax the iteration count reached (k + 1 of the last pass), ecx 0, edx the last row's B flag; esi, edi
//   restored; xmm0-7 from the last row
static void SolveContacts(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp, ebp = e - 4, self = ctx.ecx;
    uint8_t* const fp = base + ebp;
    const uint32_t rows = X86_MEM32(self + 0x0C), bodies = X86_MEM32(self + 0x08);
    const uint32_t count = X86_MEM32(self + 0x30), iters = X86_MEM32(self + 0x398);
    const f4 abs_mask = Load4(base, 0x285C40), sign = Load4(base, 0x285C50);
    std::memcpy(fp, &ctx.ebp, 4);
    std::memcpy(fp - 0x14, &rows, 4);
    std::memcpy(fp - 0x18, &ctx.esi, 4);
    std::memcpy(fp - 0x1C, &ctx.edi, 4);
    std::memcpy(fp - 0x0C, &bodies, 4);
    std::memcpy(fp - 0x10, &count, 4);
    std::memcpy(fp - 0x08, &iters, 4);
    std::memset(fp - 0x04, 0, 4);

    uint32_t eax = count, edx = 0, edi = 0, esi = 0, k = 0;
    Xmm x;
    for (;;) {
        esi = rows;
        uint32_t n = count;
        do {
            ContactRow(base, esi, bodies, abs_mask, sign, x, eax, edx, edi);
            esi += 0x100;
        } while (--n);
        eax = k + 1;
        if (eax >= iters) break;
        k = eax;
        std::memcpy(fp - 0x04, &k, 4);
    }

    ctx.eax = eax;
    ctx.ecx = 0;
    ctx.edx = edx;
    PutAll(ctx, x);
    ctx.esp = e + 4;
}

// sub_00076F20: the solver's iterations over the contact rows, then the joint rows, each pass
//   frame (ebp = esp - 4): [ebp-0x1C] contact rows, [ebp-0x18] their count, [ebp-0x14] bodies, [ebp-0x10] joint rows,
//   [ebp-0x0C] their count, [ebp-0x08] iterations, [ebp-0x04] the iteration index, then saved esi, edi
//   out: eax the iteration count reached, ecx 0, edx the last joint row's B flag; esi, edi restored
static void SolveContactsJoints(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp, ebp = e - 4, self = ctx.ecx;
    uint8_t* const fp = base + ebp;
    const uint32_t rows = X86_MEM32(self + 0x0C), joints = X86_MEM32(self + 0x10), bodies = X86_MEM32(self + 0x08);
    const uint32_t count = X86_MEM32(self + 0x30), jcount = X86_MEM32(self + 0x38), iters = X86_MEM32(self + 0x398);
    const f4 abs_mask = Load4(base, 0x285D60), sign = Load4(base, 0x285D70);
    std::memcpy(fp, &ctx.ebp, 4);
    std::memcpy(fp - 0x1C, &rows, 4);
    std::memcpy(fp - 0x10, &joints, 4);
    std::memcpy(fp - 0x14, &bodies, 4);
    std::memcpy(fp - 0x20, &ctx.esi, 4);
    std::memcpy(fp - 0x24, &ctx.edi, 4);
    std::memcpy(fp - 0x18, &count, 4);
    std::memcpy(fp - 0x0C, &jcount, 4);
    std::memcpy(fp - 0x08, &iters, 4);
    std::memset(fp - 0x04, 0, 4);

    uint32_t eax = jcount, edx = 0, edi = 0, esi = 0, k = 0;
    Xmm x;
    for (;;) {
        esi = rows;
        uint32_t n = count;
        do {
            ContactRow(base, esi, bodies, abs_mask, sign, x, eax, edx, edi);
            esi += 0x100;
        } while (--n);
        esi = joints;
        n = jcount;
        do {
            JointRow(base, esi, bodies, x, eax, edx, edi);
            esi += 0x180;
        } while (--n);
        eax = k + 1;
        if (eax >= iters) break;
        k = eax;
        std::memcpy(fp - 0x04, &k, 4);
    }

    ctx.eax = eax;
    ctx.ecx = 0;
    ctx.edx = edx;
    PutAll(ctx, x);
    ctx.esp = e + 4;
}

inline void StF(uint8_t* p, float f) { std::memcpy(p, &f, 4); }
inline void St32(uint8_t* p, uint32_t u) { std::memcpy(p, &u, 4); }
inline f4 Unpcklo(f4 a, f4 b) { return __builtin_shufflevector(a, b, 0, 4, 1, 5); }  // unpcklps a, b
inline f4 Unpckhi(f4 a, f4 b) { return __builtin_shufflevector(a, b, 2, 6, 3, 7); }  // unpckhps a, b
// cmpss 6 (not less or equal: true on NaN) as lane 0 of a mask, the upper lanes of a kept
inline f4 CmpNleSs(f4 a, f4 b) {
    i4 m = (i4)a;
    m[0] = (a[0] <= b[0]) ? 0 : -1;
    return (f4)m;
}
// one axis' angular row: (v.zxy * p) - (v.yzx * q), with p, q the lever arm's yzx and zxy
inline f4 CrossRow(f4 v, f4 p, f4 q) {
    f4 a = Sh<0x12>(v), b = Sh<0x09>(v);
    a = VMul(a, p);
    b = VMul(b, q);
    return a - b;
}

// sub_0007AFE0: one contact's Jacobian setup, called per contact by sub_000775C0 (leaf, no stack arguments)
//   ecx  the contact row (256 bytes), returned in eax; body A's pointer at +0x0C, B's at +0x1C
//   in:  +0x00, +0x10 the contact point seen from A and B; +0x20, +0x30, +0x40 the three axes; +0x2C, +0x3C, +0x4C
//        scalars in their lane 3 (+0x2C scales the stored +0x50 lanes); +0x50 (+0x5C a word), +0x60
//   body: +0x10 centre, +0x1C and +0x7C scalars (+0x7C is added to the effective-mass diagonal), +0x70 and +0x80
//        the 3x3 inverse inertia packed in two vectors, +0x8C flags, +0x90 and +0xA0 linear and angular velocity
//   Paths by flag bit 2 of the bodies: both set (rows for A and B, row flags | 1), A's alone, else B's alone (axes
//   negated). The row flags at +0x2C become (A.flags | B.flags) & 8 (| 1).
//   out: +0x00, +0x10 the lever arms (zxy) with the bodies' +0x1C in lane 3; +0x70, +0x80, +0x90 the axes with
//        the +0x5C word, then the body pointers in lane 3; +0xA0..+0xC0 the angular rows (arm x axis) through the
//        moving body's inverse inertia, +0xD0..+0xF0 B's when both move, else zero; +0x20..+0x40 the axes times
//        1 / (diagonal sum) (rcpps, recompiled as an exact reciprocal), lanes 3 of +0x3C, +0x4C kept; +0x50 zero;
//        +0x60 the target relative speed, +0x6C the axis-0 term
//   frame: ebx saved at esp - 4, ecx at esp - 8, ebp at F = (esp - 8) & ~15; [F-4] the row, [F-0x50] the relative
//   velocity (zxy), [F-0x40], [F-0x30], [F-0x20] A's row products (both moving only); esi, edi at F-0x5C, F-0x60,
//   eax at F-0x64 (push/pop); esi, edi, ebx, ebp restored; ecx the row flags, edx the +0x5C word
static void ContactSetup(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp, F = (e - 8) & ~15u, eax = ctx.ecx;
    uint8_t* const fp = base + F;
    St32(base + e - 4, ctx.ebx);
    St32(base + e - 8, ctx.ecx);
    St32(fp, ctx.ebp);
    St32(fp - 0x5C, ctx.esi);
    St32(fp - 0x60, ctx.edi);
    St32(fp - 4, eax);
    uint8_t* const cp = base + eax;
    const uint32_t esi = Ld32(cp + 0x0C), edi = Ld32(cp + 0x1C);
    uint8_t* const ap = base + esi;
    uint8_t* const bp = base + edi;
    const f4 sign = Ld(base + 0x285FF0);
    f4 x0, x1, x2, x3, x4, x5, x6, x7;
    uint32_t ecx, edx;

    x0 = Sh<0x09>(Ld(cp));
    x1 = Sh<0x09>(Ld(cp + 0x10));
    x2 = Sh<0x09>(Ld(ap + 0x10));
    x3 = Sh<0x09>(Ld(bp + 0x10));
    x4 = x0;
    St32(fp - 0x64, eax);
    x5 = x1;
    edx = Ld32(ap + 0x8C);
    x0 = x0 - x2;  // A's lever arm, yzx
    ecx = Ld32(bp + 0x8C);
    x1 = x1 - x3;  // B's
    const bool both = (ecx & edx & 4) != 0;
    ecx = (ecx | edx) & 8;
    x2 = Sh<0x09>(x0);
    x3 = Sh<0x09>(x1);
    x6 = Sh<0xD2>(x0);
    x7 = Sh<0xD2>(x1);

    const int path = both ? 0 : (edx & 4) ? 1 : 2;
    if (path == 0) {  // 0x7B065
        PATH(++g_setup_paths[0]);
        ecx |= 1;
        x5 = x5 - x4;
        St(cp, x6);
        St(cp + 0x10, x7);
        x5 = Sh<0xD2>(x5);
        x6 = Scalar(LdF(ap + 0x1C));
        x7 = Scalar(LdF(bp + 0x1C));
        St(fp - 0x50, x5);
        StF(cp + 0x0C, x6[0]);
        StF(cp + 0x1C, x7[0]);
        x4 = Ld(ap + 0xA0);
        x6 = Ld(bp + 0xA0);
        x5 = x4;
        x7 = x6;
        x4 = Sh<0x09>(x4);
        x6 = Sh<0x09>(x6);
        x4 = VMul(x4, x2);
        x6 = VMul(x6, x3);
        x5 = Sh<0x12>(x5);
        x7 = Sh<0x12>(x7);
        x5 = VMul(x5, x0);
        x7 = VMul(x7, x1);
        x4 = x4 - x5;
        x6 = x6 - x7;
        x5 = Sh<0x24>(Ld(ap + 0x90));
        x7 = Sh<0x24>(Ld(bp + 0x90));
        x4 = VAdd(x4, x5);
        x6 = VAdd(x6, x7);
        x5 = Ld(cp + 0x50);
        x7 = Ld(fp - 0x50);
        edx = Ld32(cp + 0x5C);
        x5 = Sh<0x24>(x5);
        x6 = x6 - x4;
        x4 = Xor(Splat(LdF(cp + 0x2C)), sign);
        x7 = VAdd(x7, x5);
        x5 = VMul(x5, x4);
        x6 = VAdd(x6, x7);
        St(cp + 0x50, x5);
        St(cp + 0x60, x6);
        const uint32_t ids[3] = {edx, esi, edi};
        for (int k = 0; k < 3; k++) {
            const f4 v = Ld(cp + 0x20 + 0x10 * k);
            St(cp + 0x70 + 0x10 * k, v);
            St32(cp + 0x7C + 0x10 * k, ids[k]);
            St(cp + 0xA0 + 0x10 * k, CrossRow(v, x0, x2));
            St(cp + 0xD0 + 0x10 * k, CrossRow(v, x1, x3));
        }
    } else if (path == 1) {  // 0x7B3A9: A alone
        PATH(++g_setup_paths[1]);
        x5 = x5 - x4;
        St(cp, x6);
        St(cp + 0x10, x7);
        x5 = Sh<0xD2>(x5);
        x6 = Scalar(LdF(ap + 0x1C));
        x7 = Scalar(LdF(bp + 0x1C));
        St(fp - 0x50, x5);
        StF(cp + 0x0C, x6[0]);
        StF(cp + 0x1C, x7[0]);
        x4 = Ld(ap + 0xA0);
        x5 = x4;
        x4 = Sh<0x09>(x4);
        x5 = Sh<0x12>(x5);
        x4 = VMul(x4, x2);
        x5 = VMul(x5, x0);
        x5 = x5 - x4;
        x5 = x5 - Sh<0x24>(Ld(ap + 0x90));
        x4 = Ld(cp + 0x50);
        x6 = Ld(fp - 0x50);
        edx = Ld32(cp + 0x5C);
        x4 = Sh<0x24>(x4);
        x7 = Xor(Splat(LdF(cp + 0x2C)), sign);
        x6 = VAdd(x6, x4);
        x4 = VMul(x4, x7);
        x5 = VAdd(x5, x6);
        St(cp + 0x50, x4);
        St(cp + 0x60, x5);
        const uint32_t ids[3] = {edx, esi, edi};
        for (int k = 0; k < 3; k++) {
            const f4 v = Ld(cp + 0x20 + 0x10 * k);
            St(cp + 0x70 + 0x10 * k, v);
            St32(cp + 0x7C + 0x10 * k, ids[k]);
            St(cp + 0xA0 + 0x10 * k, CrossRow(v, x0, x2));
        }
    } else {  // 0x7B5A0: B alone (or neither), the axes negated
        PATH(++g_setup_paths[2]);
        x4 = x4 - x5;
        St(cp, x7);
        St(cp + 0x10, x6);
        x4 = Sh<0xD2>(x4);
        x6 = Scalar(LdF(ap + 0x1C));
        x7 = Scalar(LdF(bp + 0x1C));
        St(fp - 0x50, x4);
        StF(cp + 0x0C, x7[0]);
        StF(cp + 0x1C, x6[0]);
        x4 = Ld(bp + 0xA0);
        x5 = x4;
        x4 = Sh<0x09>(x4);
        x5 = Sh<0x12>(x5);
        x4 = VMul(x4, x3);
        x5 = VMul(x5, x1);
        x5 = x5 - x4;
        x5 = x5 - Sh<0x24>(Ld(bp + 0x90));
        x4 = Ld(cp + 0x50);
        x6 = Ld(fp - 0x50);
        x4 = Sh<0x24>(x4);
        edx = Ld32(cp + 0x5C);
        x6 = x6 - x4;
        x7 = Splat(LdF(cp + 0x2C));
        x4 = VMul(x4, x7);
        x5 = VAdd(x5, x6);
        St(cp + 0x50, x4);
        St(cp + 0x60, x5);
        const uint32_t ids[3] = {edx, edi, esi};
        for (int k = 0; k < 3; k++) {
            f4 v = Ld(cp + 0x20 + 0x10 * k);
            if (k < 2) v = Xor(v, sign);
            St(cp + 0x70 + 0x10 * k, v);
            St32(cp + 0x7C + 0x10 * k, ids[k]);
            St(cp + 0xA0 + 0x10 * k, CrossRow(v, x1, x3));
        }
    }

    // three angular rows at cp + rows through a body's inverse inertia: each row r replaced by I r (the first keeps
    // the inertia's lane 3), the products r * (I r) returned in p0..p2 and, with save, stored at save, +0x10, +0x20
    auto inertia = [&](const uint8_t* body, uint32_t rows, uint8_t* save, f4& p0, f4& p1, f4& p2) {
        x0 = Ld(body + 0x70);
        x3 = Ld(body + 0x80);
        x1 = Unpcklo(x0, x3);
        x2 = Unpckhi(x0, x3);
        x1 = Sh<0xAE>(x1, x3);
        x2 = Sh<0x04>(x2, x3);
        f4* const out[3] = {&p0, &p1, &p2};
        for (int k = 0; k < 3; k++) {
            const f4 r = Ld(cp + rows + 0x10 * k);
            x4 = Bcast0(r);
            x4 = VMul(x4, x0);
            x3 = Sh<0x55>(r);
            x3 = VMul(x3, x1);
            x4 = VAdd(x4, x3);
            x3 = Sh<0xAA>(r);
            x3 = VMul(x3, x2);
            x4 = VAdd(x4, x3);
            if (k == 0) {
                x3 = Unpckhi(x4, x0);
                x4 = Sh<0xC4>(x4, x3);
            }
            St(cp + rows + 0x10 * k, x4);
            *out[k] = VMul(r, x4);
            if (save) St(save + 0x10 * k, *out[k]);
        }
    };
    const f4 zero = {0, 0, 0, 0};
    if (path == 0) {
        f4 a0, a1, a2;
        inertia(ap, 0xA0, fp - 0x40, a0, a1, a2);
        inertia(bp, 0xD0, nullptr, x5, x6, x7);
        x5 = VAdd(x5, Ld(fp - 0x40));
        x6 = VAdd(x6, Ld(fp - 0x30));
        x7 = VAdd(x7, Ld(fp - 0x20));
        x2 = Scalar(LdF(ap + 0x7C));
        x3 = Scalar(LdF(bp + 0x7C));
        x2[0] = SAdd(x2[0], x3[0]);
    } else {
        const uint8_t* const moving = path == 1 ? ap : bp;
        inertia(moving, 0xA0, nullptr, x5, x6, x7);
        x0 = zero;
        x2 = Scalar(LdF(moving + 0x7C));
        St(cp + 0xD0, zero);
        St(cp + 0xE0, zero);
        St(cp + 0xF0, zero);
    }

    // 0x7B790: the effective masses 1 / (A's + B's diagonal term + the products' sums), per axis
    x0 = Unpcklo(x5, x6);
    x1 = Unpckhi(x5, x6);
    x5 = Sh<0x04>(x0, x7);
    x0 = Sh<0x5E>(x0, x7);
    x1 = Sh<0xA4>(x1, x7);
    x2 = Bcast0(x2);
    x0 = VAdd(x0, x5);
    x1 = VAdd(x1, x2);
    x0 = VAdd(x0, x1);
    x0 = f4{1.0f, 1.0f, 1.0f, 1.0f} / x0;  // rcpps, recompiled as an exact reciprocal
    x1 = Ld(cp + 0x70);
    x2 = Ld(cp + 0x80);
    x3 = Ld(cp + 0x90);
    x4 = Unpcklo(x1, x2);
    x2 = Unpckhi(x2, x1);
    x1 = x4;
    x4 = Sh<0x04>(x4, x3);
    x1 = Sh<0x5E>(x1, x3);
    x2 = Sh<0xA1>(x2, x3);
    x1 = VMul(x1, x0);
    x2 = VMul(x2, x0);
    x0 = VMul(x0, x4);
    x6 = Scalar(LdF(cp + 0x3C));
    x7 = Scalar(LdF(cp + 0x4C));
    St(cp + 0x20, x0);
    St(cp + 0x30, x1);
    St(cp + 0x40, x2);
    St32(cp + 0x2C, ecx);
    StF(cp + 0x3C, x6[0]);
    StF(cp + 0x4C, x7[0]);
    x3 = Ld(cp + 0x60);
    x4 = Sh<0x55>(x3);
    x5 = Sh<0xAA>(x3);
    x3 = Bcast0(x3);
    x3 = VMul(x3, x0);
    x0 = Bcast0(x0);
    x4 = VMul(x4, x1);
    x1 = Bcast0(x1);
    x5 = VMul(x5, x2);
    x2 = Bcast0(x2);
    x3 = VAdd(x3, x4);
    x3 = VAdd(x3, x5);
    x4 = Ld(fp - 0x50);
    x6 = Ld(cp + 0x50);
    x5 = Unpckhi(x4, x4);
    x4 = Unpcklo(x4, x6);
    x0 = VMul(x0, x4);
    x4 = Sh<0x4E>(x4);
    x2 = VMul(x2, x5);
    x1 = VMul(x1, x4);
    x0 = VAdd(x0, x2);
    x0 = VAdd(x0, x1);
    x1 = Sh<0x01>(x1, x0);
    x2 = x0;
    x2[0] = SAdd(x2[0], x1[0]);
    x4 = zero;
    x5 = zero;
    St(cp + 0x50, x4);
    x6[0] = x3[0];
    x4 = CmpNleSs(x4, x1);
    x5 = CmpNleSs(x5, x0);
    x6 = CmpNleSs(x6, x1);
    x4 = And(x4, x5);
    x5 = AndNot(x5, x6);
    x4 = And(x4, x6);
    x5 = And(x5, x1);
    x4 = And(x4, x2);
    x5[0] = SAdd(x5[0], x4[0]);
    x3[0] = x3[0] - x5[0];
    St(cp + 0x60, x3);
    StF(cp + 0x6C, x0[0]);

    ctx.eax = eax;
    ctx.ecx = ecx;
    ctx.edx = edx;
    ctx.esp = e + 4;
    PutAll(ctx, Xmm{x0, x1, x2, x3, x4, x5, x6, x7});
}

}  // namespace

#ifndef BLACK_VERIFY_HOT

X86_FUNC(sub_00076890) { SolveContacts(ctx, base); }
X86_FUNC(sub_00076F20) { SolveContactsJoints(ctx, base); }
X86_FUNC(sub_0007AFE0) { ContactSetup(ctx, base); }

#else

#include <vector>

#include "game/hot_verify.h"

X86_EXTERN_FUNC(__imp__sub_00076890);
X86_EXTERN_FUNC(__imp__sub_00076F20);
X86_EXTERN_FUNC(__imp__sub_0007AFE0);

namespace {

// Verify over regions of any size: the stack frame, every row array, and the bodies the rows name (from the lowest
// to the highest index, read before the call; the solver never writes the indices or flags). Runs the native one,
// restores, runs the original, compares registers, xmm and the regions.
struct Span {
    uint32_t va, size;
};

struct Counters {
    uint64_t calls = 0, bad = 0, iters_one = 0, iters_many = 0, iters_max = 0;
};
Counters g_contacts, g_joints;

void VerifySpans(const char* name, X86Context& ctx, uint8_t* base, void (*native)(X86Context&, uint8_t*),
                 void (*original)(X86Context&, uint8_t*), const std::vector<Span>& spans, Counters& c) {
    std::vector<std::vector<uint8_t>> before(spans.size()), after(spans.size());
    for (size_t i = 0; i < spans.size(); i++) {
        const uint8_t* p = X86_PTR(uint8_t, spans[i].va);
        before[i].assign(p, p + spans[i].size);
    }
    const X86Context start = ctx;
    native(ctx, base);
    const X86Context mine = ctx;
    for (size_t i = 0; i < spans.size(); i++) {
        const uint8_t* p = X86_PTR(uint8_t, spans[i].va);
        after[i].assign(p, p + spans[i].size);
        std::memcpy(X86_PTR(uint8_t, spans[i].va), before[i].data(), spans[i].size);
    }
    ctx = start;
    original(ctx, base);

    bool same = std::memcmp(&mine, &ctx, offsetof(X86Context, cf)) == 0 &&
                std::memcmp(&mine.xmm, &ctx.xmm, sizeof ctx.xmm) == 0;
    size_t where = spans.size();
    for (size_t i = 0; i < spans.size() && same; i++)
        if (std::memcmp(after[i].data(), X86_PTR(uint8_t, spans[i].va), spans[i].size) != 0) {
            same = false;
            where = i;
        }
    const uint64_t n = ++c.calls;
    if (!same && ++c.bad <= 8) {
        std::fprintf(stderr, "verify %s: mismatch at call %llu (region %zu of %zu)\n", name, (unsigned long long)n,
                     where, spans.size());
        static const char* const gpr[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
        const uint32_t* a = &mine.eax;
        const uint32_t* b = &ctx.eax;
        for (int i = 0; i < 8; i++)
            if (a[i] != b[i]) std::fprintf(stderr, "  %s %08X, original %08X\n", gpr[i], a[i], b[i]);
        for (int i = 0; i < 8; i++)
            for (int l = 0; l < 4; l++)
                if (mine.xmm[i].u[l] != ctx.xmm[i].u[l])
                    std::fprintf(stderr, "  xmm%d[%d] %08X, original %08X\n", i, l, mine.xmm[i].u[l], ctx.xmm[i].u[l]);
    }
    if ((n & (n - 1)) == 0 && n >= 64) {
        const Paths& p = g_paths;
        std::fprintf(stderr,
                     "verify %s: %llu calls, %llu mismatches; iterations 1: %llu, >1: %llu, max %llu\n"
                     "verify solver paths: contact rows with B %llu, without %llu; lanes 1-2 upper bound kept %llu, "
                     "replaced %llu; lower bound kept %llu, replaced %llu; joint rows with B %llu, without %llu; "
                     "joint lanes max kept %llu, to 0 %llu; min kept %llu, to 0 %llu\n",
                     name, (unsigned long long)n, (unsigned long long)c.bad, (unsigned long long)c.iters_one,
                     (unsigned long long)c.iters_many, (unsigned long long)c.iters_max, (unsigned long long)p.rows_b,
                     (unsigned long long)p.rows_no_b, (unsigned long long)p.hi_kept, (unsigned long long)p.hi_replaced,
                     (unsigned long long)p.lo_kept, (unsigned long long)p.lo_replaced, (unsigned long long)p.jrows_b,
                     (unsigned long long)p.jrows_no_b, (unsigned long long)p.max_kept,
                     (unsigned long long)(4 * (p.jrows_b + p.jrows_no_b) * 2 - p.max_kept),
                     (unsigned long long)p.min_kept,
                     (unsigned long long)(4 * (p.jrows_b + p.jrows_no_b) * 2 - p.min_kept));
    }
}

// the lowest and highest body index the rows at [rows, rows + count * stride) name
void BodyRange(uint8_t* base, uint32_t rows, uint32_t count, uint32_t stride, uint32_t flag_at, uint32_t& lo,
               uint32_t& hi) {
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t r = rows + i * stride, a = X86_MEM32(r + 0x0C);
        lo = a < lo ? a : lo;
        hi = a > hi ? a : hi;
        if (X86_MEM32(r + flag_at) & 1) {
            const uint32_t b = X86_MEM32(r + 0x1C);
            lo = b < lo ? b : lo;
            hi = b > hi ? b : hi;
        }
    }
}

std::vector<Span> SolverSpans(X86Context& ctx, uint8_t* base, bool with_joints, Counters& c) {
    const uint32_t self = ctx.ecx, bodies = X86_MEM32(self + 0x08), rows = X86_MEM32(self + 0x0C),
                   count = X86_MEM32(self + 0x30), iters = X86_MEM32(self + 0x398);
    std::vector<Span> spans = {{ctx.esp - 0x40, 0x40}, {rows, count * 0x100}};
    uint32_t lo = ~0u, hi = 0;
    BodyRange(base, rows, count, 0x100, 0x2C, lo, hi);
    if (with_joints) {
        const uint32_t joints = X86_MEM32(self + 0x10), jcount = X86_MEM32(self + 0x38);
        spans.push_back({joints, jcount * 0x180});
        BodyRange(base, joints, jcount, 0x180, 0xBC, lo, hi);
    }
    spans.push_back({bodies + (lo << 6), (hi - lo + 1) << 6});
    ++(iters <= 1 ? c.iters_one : c.iters_many);
    c.iters_max = iters > c.iters_max ? iters : c.iters_max;
    return spans;
}

}  // namespace

// A dry run: the call verified on altered inputs (ways the game rarely or never takes), then every byte and register
// put back as it was, before the real call. mutate alters guest memory inside spans only.
template <class Mutate>
void DryRun(const char* name, X86Context& ctx, uint8_t* base, void (*native)(X86Context&, uint8_t*),
            void (*original)(X86Context&, uint8_t*), const std::vector<Span>& spans, Counters& c, Mutate mutate) {
    std::vector<std::vector<uint8_t>> keep(spans.size());
    for (size_t i = 0; i < spans.size(); i++) {
        const uint8_t* p = X86_PTR(uint8_t, spans[i].va);
        keep[i].assign(p, p + spans[i].size);
    }
    const X86Context start = ctx;
    mutate();
    VerifySpans(name, ctx, base, native, original, spans, c);
    for (size_t i = 0; i < spans.size(); i++) std::memcpy(X86_PTR(uint8_t, spans[i].va), keep[i].data(), spans[i].size);
    ctx = start;
}

Counters g_contacts_dry, g_joints_dry, g_setup_dry;
Counters g_contacts_nan, g_joints_nan, g_setup_nan;

// NaNs of case k (hot_verify.h) in the lanes the case names of every 16-byte vector in [va, va + count * stride),
// the words at the three offsets packed in keep (a byte each, 0xFF for none: indices, flags, pointers) left alone;
// tag tells inputs apart in the NaN bits
static void NanFill(uint8_t* base, uint32_t va, uint32_t count, uint32_t stride, uint32_t keep, uint32_t tag,
                    int k) {
    const uint32_t keep0 = keep & 0xFF, keep1 = keep >> 8 & 0xFF, keep2 = keep >> 16 & 0xFF;
    for (uint32_t i = 0; i < count; i++)
        for (uint32_t off = 0; off < stride; off += 4) {
            const bool kept = off == keep0 || off == keep1 || off == keep2;
            const uint32_t lane = (off >> 2) & 3;
            if (!kept && (kNanCases[k].lanes >> lane & 1))
                X86_MEM32(va + i * stride + off) = NanBits(tag + (off >> 4) + 3 * i, lane, k, kNanCases[k].signaling);
        }
}

// the solver's NaN dry run: the contact rows, with joints the joint rows, and the bodies in [lo, lo + size)
static void NanRows(uint8_t* base, uint32_t self, bool joints, uint32_t lo, uint32_t size, int k) {
    const uint32_t rows = X86_MEM32(self + 0x0C), count = X86_MEM32(self + 0x30);
    const uint32_t jrows = X86_MEM32(self + 0x10), jcount = joints ? uint32_t(X86_MEM32(self + 0x38)) : 0u;
    NanFill(base, rows, count, 0x100, 0x2C1C0C, 0, k);
    NanFill(base, jrows, jcount, 0x180, 0xBC1C0C, 0x40, k);
    NanFill(base, lo, size / 64, 64, 0xFFFFFF, 0x80, k);
}

// the contact setup's NaN dry run: the row and both bodies (pointers at +0x0C, +0x1C, the +0x5C word, flags kept)
static void NanSetup(uint8_t* base, uint32_t row, int k) {
    const uint32_t a = X86_MEM32(row + 0x0C), b = X86_MEM32(row + 0x1C);
    NanFill(base, row, 1, 0x100, 0x5C1C0C, 0, k);
    NanFill(base, a, 1, 0xB0, 0xFFFF8C, 0x20, k);
    if (b != a) NanFill(base, b, 1, 0xB0, 0xFFFF8C, 0x40, k);
}

static void ReportNan(const char* name, const Counters& c) {
    if ((c.calls & (c.calls - 1)) == 0)
        std::fprintf(stderr, "verify %s NaN dry runs: %llu calls, %llu mismatches\n", name,
                     (unsigned long long)c.calls, (unsigned long long)c.bad);
}

X86_FUNC(sub_00076890) {
    const uint32_t self = ctx.ecx;
    std::vector<Span> spans = SolverSpans(ctx, base, false, g_contacts);
    if ((g_contacts.calls & 15) == 0) {  // one pass, or none asked (the loop still runs once)
        spans.push_back({self + 0x398, 4});
        const uint32_t iters = (g_contacts.calls >> 4) & 1;
        DryRun("sub_00076890 (dry: 1 or 0 iterations)", ctx, base, SolveContacts, __imp__sub_00076890, spans,
               g_contacts_dry, [&] { X86_MEM32(self + 0x398) = iters; });
        spans.pop_back();
    }
    if ((g_contacts.calls & 7) == 3) {  // NaNs in the rows and the bodies, the cases of hot_verify.h in turn
        const int k = int((g_contacts.calls >> 3) % kNanCaseCount);
        const Span bodies = spans.back();
        DryRun("sub_00076890 (dry: NaNs)", ctx, base, SolveContacts, __imp__sub_00076890, spans, g_contacts_nan,
               [&] { NanRows(base, self, false, bodies.va, bodies.size, k); });
        ReportNan("sub_00076890", g_contacts_nan);
    }
    VerifySpans("sub_00076890", ctx, base, SolveContacts, __imp__sub_00076890, spans, g_contacts);
}

X86_FUNC(sub_00076F20) {
    const uint32_t self = ctx.ecx, joints = X86_MEM32(self + 0x10), jcount = X86_MEM32(self + 0x38);
    std::vector<Span> spans = SolverSpans(ctx, base, true, g_joints);
    if ((g_joints.calls & 7) == 0) {  // B left out of every other joint row; one pass
        spans.push_back({self + 0x398, 4});
        DryRun("sub_00076F20 (dry: joints without B, 1 iteration)", ctx, base, SolveContactsJoints, __imp__sub_00076F20,
               spans, g_joints_dry, [&] {
                   X86_MEM32(self + 0x398) = 1;
                   for (uint32_t i = 0; i < jcount; i += 2) X86_MEM32(joints + i * 0x180 + 0xBC) &= ~1u;
               });
        spans.pop_back();
    }
    if ((g_joints.calls & 7) == 3) {  // NaNs in the contact and joint rows and the bodies
        const int k = int((g_joints.calls >> 3) % kNanCaseCount);
        const Span bodies = spans.back();
        DryRun("sub_00076F20 (dry: NaNs)", ctx, base, SolveContactsJoints, __imp__sub_00076F20, spans, g_joints_nan,
               [&] { NanRows(base, self, true, bodies.va, bodies.size, k); });
        ReportNan("sub_00076F20", g_joints_nan);
    }
    VerifySpans("sub_00076F20", ctx, base, SolveContactsJoints, __imp__sub_00076F20, spans, g_joints);
}

X86_FUNC(sub_0007AFE0) {
    static std::atomic<uint64_t> calls{0}, bad{0};
    const uint32_t row = ctx.ecx;
    if ((calls & 15) == 0) {  // A and B swapped: B alone moves (or neither does)
        const std::vector<Span> spans = {{ctx.esp - 0x90, 0x90}, {row, 0x100}};
        DryRun("sub_0007AFE0 (dry: A and B swapped)", ctx, base, ContactSetup, __imp__sub_0007AFE0, spans, g_setup_dry,
               [&] {
                   const uint32_t a = X86_MEM32(row + 0x0C);
                   X86_MEM32(row + 0x0C) = X86_MEM32(row + 0x1C);
                   X86_MEM32(row + 0x1C) = a;
               });
    }
    if ((calls & 15) == 7) {  // NaNs in the row and both bodies (pointers at +0x0C, +0x1C, the +0x5C word, flags kept)
        const int k = int((calls >> 4) % kNanCaseCount);
        const uint32_t a = X86_MEM32(row + 0x0C), b = X86_MEM32(row + 0x1C);
        const std::vector<Span> spans = {{ctx.esp - 0x90, 0x90}, {row, 0x100}, {a, 0xB0}, {b, 0xB0}};
        DryRun("sub_0007AFE0 (dry: NaNs)", ctx, base, ContactSetup, __imp__sub_0007AFE0, spans, g_setup_nan,
               [&] { NanSetup(base, row, k); });
        ReportNan("sub_0007AFE0", g_setup_nan);
    }
    const Region regions[] = {{ctx.esp - 0x90, 0x90}, {row, 0x100}};
    Verify("sub_0007AFE0", ctx, base, ContactSetup, __imp__sub_0007AFE0, regions, calls, bad);
    const uint64_t n = calls.load();
    if ((n & (n - 1)) == 0 && n >= 1024)
        std::fprintf(stderr,
                     "verify sub_0007AFE0 paths (dry runs included): both move %llu, A alone %llu, B alone %llu\n",
                     (unsigned long long)g_setup_paths[0], (unsigned long long)g_setup_paths[1],
                     (unsigned long long)g_setup_paths[2]);
}

#endif
