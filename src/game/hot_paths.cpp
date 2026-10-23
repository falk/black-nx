// Native replacements of the game loop's hottest recompiled functions
//
// Each one leaves the guest exactly as the original does: the same result, the same registers and the same stack
// bytes, since other code can read stack slots a function leaves behind. Build with BLACK_VERIFY_HOT to run the
// original after every call and count the calls where anything differs.
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "game/hot_ordered.h"

using namespace hot;
X86_EXTERN_FUNC(__imp__sub_000205A0);
X86_EXTERN_FUNC(__imp__sub_000D1D10);
X86_EXTERN_FUNC(sub_000CD040);

// NaN results: when both operands of an addps, addss, mulps or mulss are NaN, the x86 result is the first operand's
// (quieted). The rewrites below use the ordered helpers of hot_ordered.h (VMul, SAdd, AddPair, ...) wherever both
// operands can carry a NaN from guest memory, in the original's operand order; plain + and * remain where one operand
// is a constant of .rdata (0x28B534, 0x295C74, 0x295C88, 0x295D80, 0x295D84, 0x295D8C, 0x295E90: never NaN), a value
// that cannot be NaN (an integer converted, a t already compared to be in 0..1), or a copy of the other operand.
// The guest stack frame is written through a host pointer to its lowest slot, so each slot is an immediate offset.

namespace {

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
inline void StF(uint8_t* p, float f) { std::memcpy(p, &f, 4); }
inline void St32(uint8_t* p, uint32_t u) { std::memcpy(p, &u, 4); }
inline f4 Ld4(const uint8_t* p) {
    f4 v;
    std::memcpy(&v, p, 16);
    return v;
}
inline void St4(uint8_t* p, f4 v) { std::memcpy(p, &v, 16); }

}  // namespace

// sub_000205A0: a vertex position decoded from its quantized form
//   ecx  mesh: the vertices at +0x04 (u16 x, y, z each), the offset at +0x0A (s8 x, y, z)
//   edx  vertex index
//   eax  out: 16 bytes, (u16 xyz / [0x295D80]) * [0x295D8C] + s8 xyz * [0x295D84]
// w comes from a stack slot the original never writes, so it is read from there too. On x86 a subnormal there can trigger
// microcode assists, so that backend keeps w out of the vector operations and handles it separately.
// AArch64 uses ordinary four-lane NEON arithmetic. The x86 w paths are:
// - zero or denormal, with the scales at 65536, 500 and 1000: w is m units of 2^-149, and each step is integer
//   arithmetic on units, rounded to float as SSE rounds (to nearest, ties to even);
// - else in double, rounded to float after each step (with 53 bits against 24, a double rounding cannot change a
//   single +, *, / from the float result).
// [0x295D80] is 65536: dividing by a power of two is multiplying by its reciprocal, the same exact value rounded once.
// NaN: the scales are constants of .rdata and the integers converted are never NaN, so only w can be NaN, and then
// every step of lane w carries w's NaN on both sides: the operand order cannot change a result here.

// a float from a whole number of units of 2^-149, rounded to 24 significant bits, ties to even; below 2^24 units the
// float's bits are the number itself (denormals, then the first normal binade)
static uint32_t UnitsToFloatBits(uint64_t u, uint64_t* value) {
    if (u < (1u << 24)) {
        *value = u;
        return uint32_t(u);
    }
    int shift = 64 - __builtin_clzll(u) - 24;
    uint64_t q = u >> shift, rest = u & ((uint64_t(1) << shift) - 1), half = uint64_t(1) << (shift - 1);
    if (rest > half || (rest == half && (q & 1))) q++;
    if (q == (1u << 24)) {
        q >>= 1;
        shift++;
    }
    *value = q << shift;
    return (uint32_t(shift + 1) << 23) | uint32_t(q & 0x7FFFFF);
}

// lane w of the decode for a zero or denormal w (bits): w * 500, then (w / 65536) * 1000 + that
static void DecodeTinyW(uint32_t bits, float* scaled, float* out) {
    const uint32_t sign = bits & 0x80000000u, m = bits & 0x7FFFFF;
    uint32_t k = m >> 16, rest = m & 0xFFFF;  // w / 65536 in units
    if (rest > 0x8000 || (rest == 0x8000 && (k & 1))) k++;
    uint64_t c = 0;
    const uint32_t cbits = UnitsToFloatBits(uint64_t(m) * 500, &c);
    uint64_t sum = 0;
    const uint32_t obits = UnitsToFloatBits(uint64_t(k) * 1000 + c, &sum);
    const uint32_t sb = sign | cbits, ob = sign | obits;
    std::memcpy(scaled, &sb, 4);
    std::memcpy(out, &ob, 4);
}

static void DecodePosition(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t frame = (ctx.esp - 4) & ~15u;  // after push ebp and the alignment
    const uint32_t s = frame - 0x20;              // esp after sub esp, 0x1C and push esi
    uint8_t* const fp = base + s;
    const uint32_t mesh = ctx.ecx;
    const uint32_t v = X86_MEM32(mesh + 0x4) + ctx.edx * 6;
    auto in_frame = [&](uint32_t p, uint32_t n) {
        return uint64_t(p) < ctx.esp && uint64_t(s) < uint64_t(p) + n;
    };
    if (in_frame(mesh, 13) || in_frame(v, 6) || in_frame(ctx.eax, 16)) {
        __imp__sub_000205A0(ctx, base);
        return;
    }

    const float w = LdF(fp + 0x1C);
#ifdef __aarch64__
    // Cortex-A57 handles subnormal arithmetic in hardware. Include w in the
    // same NEON operations as xyz instead of emulating it with integer units.
    const float lane_w = w;
#else
    const float lane_w = 0;
#endif
    const f4 offset3 = {float(int8_t(X86_MEM8(mesh + 0xA))), float(int8_t(X86_MEM8(mesh + 0xB))),
                        float(int8_t(X86_MEM8(mesh + 0xC))), lane_w};
    const f4 quant3 = {float(X86_MEM16(v)), float(X86_MEM16(v + 2)), float(X86_MEM16(v + 4)), lane_w};
    const float s84 = F32(base, 0x295D84), s80 = F32(base, 0x295D80), s8c = F32(base, 0x295D8C);

    f4 scaled = offset3 * Splat(s84);
    f4 out = (s80 == 65536.0f ? quant3 * Splat(1.0f / 65536.0f) : quant3 / Splat(s80)) * Splat(s8c) + scaled;
#ifndef __aarch64__
    const uint32_t wbits = Ld32(fp + 0x1C);
    if ((wbits & 0x7F800000u) == 0 && s80 == 65536.0f && s84 == 500.0f && s8c == 1000.0f) {
        float sw, ow;
        DecodeTinyW(wbits, &sw, &ow);
        scaled[3] = sw;
        out[3] = ow;
    } else {
        const double dw = w;
        scaled[3] = float(dw * s84);
        out[3] = float(double(float(double(float(dw / s80)) * s8c)) + double(scaled[3]));
    }
#endif
    const f4 quant = {quant3[0], quant3[1], quant3[2], w};

    // the stack as the original leaves it: saved ebp and esi, its last locals
    X86_MEM32(ctx.esp - 4) = ctx.ebp;
    St32(fp, ctx.esi);
    StF(fp + 0x8, s8c);
    StF(fp + 0xC, s80);
    StF(fp + 0x10, quant[0]);
    StF(fp + 0x14, quant[1]);
    StF(fp + 0x18, quant[2]);
    std::memcpy(X86_PTR(uint8_t, ctx.eax), &out, 16);

    // registers: ecx and edx point at its locals, xmm0 to xmm3 hold what it computed with
    ctx.ecx = s + 0xC;
    ctx.edx = s + 0x8;
    PutXmm(ctx.xmm[0], Splat(s8c));
    PutXmm(ctx.xmm[1], scaled);
    PutXmm(ctx.xmm[2], Splat(s80));
    PutXmm(ctx.xmm[3], out);
    ctx.esp += 4;
}

// sub_000CCB30: a ray against a triangle, Moller-Trumbore, the triangle's back culled
//   ecx, edx  corners 0 and 1 (16 bytes each); eax the ray, its start then its end
//   stack     corner 2, then where t (along the ray), u and v (across the triangle) go; the caller pops them
//   al        1 on a hit; t, u and v written only then
// The tolerances: det above [0x28B534], u, v, u + v and t between det * [0x295C88] and det * [0x295E90].
// The two dot products are sub_00011D40's, with its frame below this one.
// A NaN det, u, v or t passes every test (comiss sets CF and ZF on unordered: jnb and jnbe are not taken), so NaNs
// from the corners or the ray reach t, u and v.
static void RayTriangle(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp, ebp = e - 4;
    const uint32_t s = ((e - 4) & ~15u) - 0x50;  // esp after the alignment and sub esp, 0x50
    uint8_t* const fp = base + s;
    uint8_t* const bp = base + ebp;
    St32(bp, ctx.ebp);
    uint32_t eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx;
    f4 x0, x1, x2, x3, x4, x5, x6, x7;
    bool hit = false;

    // sub_00011D40 called with the return address r: dot(*ecx, *eax) in lane 0 of xmm0
    auto dot = [&](uint32_t r) {
        St32(fp - 4, r);
        St32(fp - 8, ebp);
        x0 = f4{0, 0, 0, 0};
        x1 = Load4(base, eax);
        StF(fp - 0x14, x0[0]);
        x0 = Load4(base, ecx);
        x0 = VMul(x0, x1);
        x1 = Shuf<0x39>(x0, x0);
        x2 = x1;
        x0 = Lane0(x0, AddPair(x0));  // addss xmm0, xmm1: lane 0 + lane 1
        x2 = Shuf<0x39>(x2, x1);
        x0 = Lane0(x0, SAdd(x0[0], x2[0]));
        edx = s - 0x14;
        StF(fp - 0x14, x0[0]);
        x0 = Scalar(LdF(fp - 0x14));
    };

    do {
        x5 = Load4(base, ecx);
        ecx = Ld32(bp + 8);
        x1 = Load4(base, ecx);
        x3 = Load4(base, eax);
        x0 = Load4(base, eax + 0x10);
        x4 = Load4(base, edx);
        x7 = f4{0, 0, 0, 0};
        x1 = x1 - x5;  // e2
        St4(fp + 0x40, x1);
        x1 = Shuf<0xD2>(x1, x1);
        x0 = x0 - x3;  // the ray's direction
        x2 = Shuf<0xD2>(x1, x1);
        St4(fp + 0x30, x0);
        x0 = Shuf<0xC9>(x0, x0);
        x6 = Shuf<0xC9>(x0, x0);
        x6 = VMul(x6, x2);
        x0 = VMul(x0, x1);
        x0 = x0 - x6;  // pvec = dir x e2
        x4 = x4 - x5;  // e1
        x1 = VMul(x4, x0);
        x2 = Shuf<0x39>(x1, x1);
        x6 = Shuf<0x39>(x2, x2);
        x1 = Lane0(x1, AddPair(x1));
        x1 = Lane0(x1, SAdd(x1[0], x6[0]));  // det = e1 . pvec
        StF(fp + 0x10, x7[0]);
        edx = s + 0x10;
        StF(fp + 0x10, x1[0]);
        x1 = Scalar(F32(base, 0x28B534));
        x6 = Scalar(LdF(fp + 0x10));
        if (Jnb(x1[0], x6[0])) break;

        x3 = x3 - x5;  // tvec
        x1 = VMul(x3, x0);
        x0 = Shuf<0x39>(x1, x1);
        x5 = Shuf<0x39>(x0, x0);
        x1 = Lane0(x1, AddPair(x1));
        StF(fp + 0x0C, x7[0]);
        eax = s + 0x0C;
        x2 = Lane0(x6, x6[0] * F32(base, 0x295C88));
        x6 = Lane0(x6, x6[0] * F32(base, 0x295E90));
        x1 = Lane0(x1, SAdd(x1[0], x5[0]));  // u = tvec . pvec
        StF(fp + 0x0C, x1[0]);
        x0 = Scalar(LdF(fp + 0x0C));
        StF(fp + 0x14, x2[0]);
        StF(fp + 0x18, x6[0]);
        if (Jnb(x2[0], x0[0]) || Jnbe(x0[0], x6[0])) break;

        x0 = Shuf<0xC9>(x3, x3);
        x1 = Shuf<0xD2>(x4, x4);
        x3 = Shuf<0xC9>(x0, x0);
        x2 = Shuf<0xD2>(x1, x1);
        x3 = VMul(x3, x2);
        x0 = VMul(x0, x1);
        x0 = x0 - x3;  // qvec = tvec x e1
        eax = s + 0x20;
        ecx = s + 0x30;
        St4(fp + 0x20, x0);
        dot(0xCCC5D);  // v = dir . qvec
        x1 = Scalar(LdF(fp + 0x14));
        StF(fp + 0x1C, x0[0]);
        if (Jnb(x1[0], x0[0])) break;
        x1 = Scalar(LdF(fp + 0x0C));
        x1 = Lane0(x1, SAdd(x1[0], x0[0]));
        if (Jnbe(x1[0], LdF(fp + 0x18))) break;

        eax = s + 0x20;
        ecx = s + 0x40;
        dot(0xCCC8C);  // t = e2 . qvec
        x1 = Scalar(LdF(fp + 0x14));
        if (Jnb(x1[0], x0[0]) || Jnbe(x0[0], LdF(fp + 0x18))) break;

        x1 = Scalar(F32(base, 0x295C74));
        x1 = Lane0(x1, x1[0] / LdF(fp + 0x10));  // 1 / det
        ecx = Ld32(bp + 0x0C);
        edx = Ld32(bp + 0x10);
        eax = Ld32(bp + 0x14);
        x2 = Lane0(x1, SMul(x1[0], x0[0]));
        x0 = Scalar(LdF(fp + 0x0C));
        X86_MEMF32(ecx) = x2[0];
        x0 = Lane0(x0, SMul(x0[0], x1[0]));
        x1 = Lane0(x1, SMul(x1[0], LdF(fp + 0x1C)));
        X86_MEMF32(edx) = x0[0];
        X86_MEMF32(eax) = x1[0];
        hit = true;
    } while (false);

    ctx.eax = (eax & ~0xFFu) | (hit ? 1u : 0u);
    ctx.ecx = ecx;
    ctx.edx = edx;
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

// sub_000CC6C0: a sphere against a line segment
//   eax  the sphere: its centre, the radius in w
//   ecx  the segment: p0, then p1 at +0x10
//   eax  2 when both ends are inside, 1 when the segment reaches the sphere, else 0; the closest point is p0 + seg * t,
//        t = (c - p0) . seg / |seg|^2, refused outside 0 to [0x295C74]
// A NaN t is refused (jb is taken on unordered), so seg * t has at most one NaN operand; p0 + seg * t can have two
// (the w lanes: p0's and p1's).
static void SphereSegment(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp, ebp = e - 4;
    const uint32_t s = ((e - 4) & ~15u) - 0x20;
    uint8_t* const fp = base + s;
    St32(base + ebp, ctx.ebp);
    uint32_t eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx;
    f4 x0, x1, x2, x3, x4, x5, x6, x7;  // all eight set before the first way out
    uint32_t result = 0;
    // comiss a, b then jb: taken when a < b or either is NaN
    auto jb = [](float a, float b) { return !(a >= b); };

    do {
        x2 = Load4(base, eax);  // the centre
        x3 = Load4(base, ecx + 0x10);
        x6 = Scalar(F32(base, eax + 0x0C));
        x4 = f4{0, 0, 0, 0};
        x0 = x2 - x3;
        x0 = x0 * x0;
        x1 = Shuf<0x39>(x0, x0);
        x0 = Lane0(x0, AddPair(x0));
        x5 = Shuf<0x39>(x1, x1);
        x0 = Lane0(x0, SAdd(x0[0], x5[0]));  // |c - p1|^2
        x5 = Load4(base, ecx);
        x1 = x2;
        StF(fp + 0x08, x4[0]);
        eax = s + 0x08;
        StF(fp + 0x08, x0[0]);
        x1 = x1 - x5;  // c - p0
        St4(fp + 0x10, x2);
        x0 = x1 * x1;
        x2 = Shuf<0x39>(x0, x0);
        x6 = Lane0(x6, x6[0] * x6[0]);  // r^2
        const bool far1 = jb(x6[0], LdF(fp + 0x08));
        x7 = Shuf<0x39>(x2, x2);
        x0 = Lane0(x0, AddPair(x0));
        ecx = s + 0x04;
        StF(fp + 0x04, x4[0]);
        x0 = Lane0(x0, SAdd(x0[0], x7[0]));  // |c - p0|^2
        StF(fp + 0x0C, x6[0]);
        StF(fp + 0x04, x0[0]);
        if (!far1) {
            result = jb(x6[0], LdF(fp + 0x04)) ? 1 : 2;
            break;
        }
        if (!jb(x6[0], LdF(fp + 0x04))) {
            result = 1;
            break;
        }

        x3 = x3 - x5;  // seg = p1 - p0
        x2 = x3;
        x1 = VMul(x1, x2);
        x0 = Shuf<0x39>(x1, x1);
        x1 = Lane0(x1, AddPair(x1));
        x3 = Shuf<0x39>(x0, x0);
        x1 = Lane0(x1, SAdd(x1[0], x3[0]));  // (c - p0) . seg
        StF(fp + 0x08, x4[0]);
        edx = s + 0x08;
        StF(fp + 0x08, x1[0]);
        x0 = x2 * x2;
        x1 = Shuf<0x39>(x0, x0);
        x0 = Lane0(x0, AddPair(x0));
        x3 = x1;
        StF(fp + 0x04, x4[0]);
        x3 = Shuf<0x39>(x3, x1);
        x0 = Lane0(x0, SAdd(x0[0], x3[0]));  // |seg|^2
        eax = s + 0x04;
        StF(fp + 0x04, x0[0]);
        x0 = Scalar(LdF(fp + 0x08));
        x0 = Lane0(x0, x0[0] / LdF(fp + 0x04));  // t
        if (jb(x0[0], x4[0])) break;
        x1 = Scalar(F32(base, 0x295C74));
        if (jb(x1[0], x0[0])) break;
        StF(fp + 0x08, x0[0]);
        ecx = s + 0x08;
        x0 = Scalar(LdF(fp + 0x08));
        x0 = Shuf<0x00>(x0, x0);
        x2 = x2 * x0;
        eax = s + 0x10;
        x5 = VAdd(x5, x2);
        x5 = x5 - Ld4(fp + 0x10);  // the closest point - c
        ecx = eax;
        St4(fp + 0x10, x5);
        // sub_00011D40: |closest - c|^2 (eax and ecx both s + 0x10: a square)
        St32(fp - 4, 0xCC80F);
        St32(fp - 8, ebp);
        x0 = f4{0, 0, 0, 0};
        x1 = Ld4(fp + 0x10);
        StF(fp - 0x14, x0[0]);
        x0 = Ld4(fp + 0x10);
        x0 = x0 * x1;
        x1 = Shuf<0x39>(x0, x0);
        x2 = x1;
        x0 = Lane0(x0, AddPair(x0));
        x2 = Shuf<0x39>(x2, x1);
        x0 = Lane0(x0, SAdd(x0[0], x2[0]));
        edx = s - 0x14;
        StF(fp - 0x14, x0[0]);
        x0 = Scalar(LdF(fp - 0x14));
        x1 = Scalar(LdF(fp + 0x0C));
        result = jb(x1[0], x0[0]) ? 0 : 1;
    } while (false);

    ctx.eax = result;
    ctx.ecx = ecx;
    ctx.edx = edx;
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

// Intrusive collision-tree traversal. Inline the sphere test so its register
// results can feed the traversal without a second guest call boundary. Guest
// stack operations remain ordered around the leaf callback and recursion.
namespace {
inline void Push(X86Context& ctx, uint8_t* base, uint32_t value) {
    ctx.esp -= 4;
    X86_MEM32(ctx.esp) = value;
}
inline uint32_t Pop(X86Context& ctx, uint8_t* base) {
    uint32_t value = X86_MEM32(ctx.esp);
    ctx.esp += 4;
    return value;
}
static __attribute__((flatten)) void Walk(X86Context& __restrict ctx, uint8_t* base) {
    Push(ctx, base, ctx.esi);
    ctx.esi = X86_MEM32(ctx.esp + 8);
    Push(ctx, base, ctx.edi);
    ctx.edi = ctx.ecx;
    ctx.ecx = X86_MEM32(ctx.edi + 0x4c);
    ctx.eax = ctx.esi;
    Push(ctx, base, 0xCFE52);
    SphereSegment(ctx, base);
    if (!ctx.eax) {
        ctx.edi = Pop(ctx, base);
        ctx.eax = (ctx.eax & 0xffffff00u) | 1;
        ctx.esi = Pop(ctx, base);
        ctx.esp += 8;
        return;
    }
    ctx.ecx = X86_MEM32(ctx.esi + 0x24);
    if (!ctx.ecx) {
        ctx.ecx = X86_MEM32(ctx.edi + 0x64);
        Push(ctx, base, ctx.ecx);
        Push(ctx, base, ctx.eax);
        Push(ctx, base, ctx.esi);
        const uint32_t target = X86_MEM32(ctx.edi + 0x58);
        Push(ctx, base, 0xCFE6D);
        x86_icall_fast(ctx, base, target);
        ctx.esp += 12;
        ctx.edi = Pop(ctx, base);
        ctx.esi = Pop(ctx, base);
        ctx.esp += 8;
        return;
    }
    Push(ctx, base, ctx.ebx);
    ctx.ebx = ctx.esi + 0x28;
    ctx.esi = X86_MEM32(ctx.ebx);
    while (ctx.esi != ctx.ebx) {
        ctx.eax = ctx.esi - 0x14;
        ctx.esi = X86_MEM32(ctx.esi);
        Push(ctx, base, ctx.eax);
        ctx.ecx = ctx.edi;
        Push(ctx, base, 0xCFE8D);
        Walk(ctx, base);
        if (!(ctx.eax & 0xff)) {
            ctx.ebx = Pop(ctx, base);
            ctx.edi = Pop(ctx, base);
            ctx.eax &= 0xffffff00u;
            ctx.esi = Pop(ctx, base);
            ctx.esp += 8;
            return;
        }
    }
    ctx.ebx = Pop(ctx, base);
    ctx.edi = Pop(ctx, base);
    ctx.eax = (ctx.eax & 0xffffff00u) | 1;
    ctx.esi = Pop(ctx, base);
    ctx.esp += 8;
}
} // namespace
X86_FUNC(sub_000CFE40) { Walk(ctx, base); }

namespace {
// Nearest ray hit over a mesh's triangle/quad index records. Keep decoded
// vertices and the ray test in one native call instead of crossing guest call
// boundaries for every corner. Scratch slots and return addresses remain visible.
static inline f4 GetXmm(const X86Xmm& x) {
    f4 v;
    std::memcpy(&v, &x, 16);
    return v;
}
static __attribute__((flatten)) void MeshRay(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp, s = ((e - 4) & ~15u) - 0xE0;
    const uint32_t mesh = ctx.eax, ray = X86_MEM32(e + 4), result = X86_MEM32(e + 8);
    auto overlaps = [&](uint32_t p, uint32_t n) {
        return uint64_t(p) < uint64_t(e) + 12 && uint64_t(s - 0x100) < uint64_t(p) + n;
    };
    if (overlaps(mesh, 16) || overlaps(ray, 32) || overlaps(result, 0x6c) ||
        overlaps(X86_MEM32(mesh), uint32_t(X86_MEM8(mesh + 13)) * X86_MEM8(mesh + 14) + 4) ||
        overlaps(X86_MEM32(mesh + 4), 256 * 6)) {
        __imp__sub_000D1D10(ctx, base);
        return;
    }
    Push(ctx, base, ctx.ebp);
    ctx.ebp = ctx.esp;
    ctx.esp = s + 12;
    Push(ctx, base, ctx.ebx);
    ctx.ebx = X86_MEM32(ctx.ebp + 12);
    PutXmm(ctx.xmm[0], Scalar(F32(base, ctx.ebx + 0x50)));
    Push(ctx, base, ctx.esi);
    Push(ctx, base, ctx.edi);
    ctx.edi = ctx.eax;
    ctx.eax = X86_MEM32(ctx.edi + 4);
    ctx.ecx = X86_MEM8(ctx.edi + 13);
    ctx.esi = X86_MEM32(ctx.edi);
    X86_MEM32(s + 0x2c) = ctx.eax;
    ctx.eax = X86_MEM8(ctx.edi + 14) * ctx.ecx + ctx.esi;
    ctx.edx = 0;
    X86_MEM32(s + 0x1c) = 0;
    X86_MEM32(s + 0x28) = 0;
    X86_MEM32(s + 0x24) = ctx.eax;
    X86_MEM32(s + 0x14) = 0;
    StF(base + s + 0x18, ctx.xmm[0].f[0]);
    if (ctx.esi < ctx.eax) {
        do {
            ctx.eax = X86_MEM32(s + 0x14);
            ctx.ecx = ctx.edi;
            if (!ctx.eax) {
                ctx.edx = X86_MEM8(ctx.esi);
                ctx.eax = s + 0x40;
                Push(ctx, base, 0xD1D76);
                DecodePosition(ctx, base);
                PutXmm(ctx.xmm[0], Load4(base, ctx.eax));
                ctx.edx = X86_MEM8(ctx.esi + 1);
                ctx.eax = s + 0xD0;
                ctx.ecx = ctx.edi;
                Store4(base, s + 0x50, GetXmm(ctx.xmm[0]));
                Push(ctx, base, 0xD1D90);
                DecodePosition(ctx, base);
                PutXmm(ctx.xmm[0], Load4(base, ctx.eax));
                ctx.edx = X86_MEM8(ctx.esi + 2);
                Store4(base, s + 0x60, GetXmm(ctx.xmm[0]));
                ctx.eax = s + 0xC0;
                ctx.ecx = ctx.edi;
            } else {
                PutXmm(ctx.xmm[0], Load4(base, s + 0x70));
                ctx.edx = X86_MEM8(ctx.esi + 3);
                Store4(base, s + 0x50, GetXmm(ctx.xmm[0]));
                ctx.eax = s + 0xB0;
            }
            Push(ctx, base, 0xD1DC1);
            DecodePosition(ctx, base);
            PutXmm(ctx.xmm[0], Load4(base, ctx.eax));
            ctx.ecx = s + 0x20;
            Push(ctx, base, ctx.ecx);
            ctx.edx = s + 0x38;
            Push(ctx, base, ctx.edx);
            ctx.eax = s + 0x30;
            Push(ctx, base, ctx.eax);
            ctx.eax = X86_MEM32(ctx.ebp + 8);
            ctx.ecx = s + 0x70;
            Push(ctx, base, ctx.ecx);
            ctx.edx = s + 0x60;
            ctx.ecx = s + 0x50;
            Store4(base, s + 0x70, GetXmm(ctx.xmm[0]));
            Push(ctx, base, 0xD1DF0);
            RayTriangle(ctx, base);
            ctx.esp += 16;
            if (ctx.eax & 0xff) {
                PutXmm(ctx.xmm[0], Scalar(F32(base, s + 0x30)));
                PutXmm(ctx.xmm[1], Scalar(F32(base, s + 0x18)));
                if (ctx.xmm[1].f[0] > ctx.xmm[0].f[0]) {
                    PutXmm(ctx.xmm[1], Load4(base, s + 0x50));
                    ctx.edx = X86_MEM32(s + 0x14);
                    Store4(base, s + 0x80, GetXmm(ctx.xmm[1]));
                    PutXmm(ctx.xmm[1], Load4(base, s + 0x60));
                    StF(base + s + 0x18, ctx.xmm[0].f[0]);
                    PutXmm(ctx.xmm[0], Scalar(F32(base, s + 0x38)));
                    Store4(base, s + 0x90, GetXmm(ctx.xmm[1]));
                    PutXmm(ctx.xmm[1], Load4(base, s + 0x70));
                    StF(base + s + 0x34, ctx.xmm[0].f[0]);
                    PutXmm(ctx.xmm[0], Scalar(F32(base, s + 0x20)));
                    Store4(base, s + 0xA0, GetXmm(ctx.xmm[1]));
                    X86_MEM32(s + 0x1c) = ctx.esi;
                    X86_MEM32(s + 0x28) = ctx.edx;
                    StF(base + s + 0x3c, ctx.xmm[0].f[0]);
                }
            }
            if (X86_MEM8(ctx.esi + 3) < 255 && !X86_MEM32(s + 0x14)) {
                X86_MEM32(s + 0x14) = 1;
            } else {
                ctx.eax = X86_MEM8(ctx.edi + 13);
                ctx.esi += ctx.eax;
                X86_MEM32(s + 0x14) = 0;
            }
        } while (ctx.esi < X86_MEM32(s + 0x24));
        ctx.eax = X86_MEM32(s + 0x1c);
    }
    if (!X86_MEM32(s + 0x1c)) {
        ctx.eax = 0;
        ctx.edi = Pop(ctx, base);
        ctx.esi = Pop(ctx, base);
        ctx.ebx = Pop(ctx, base);
        ctx.esp = ctx.ebp;
        ctx.ebp = Pop(ctx, base);
        ctx.esp += 4;
        return;
    }
    ctx.eax = X86_MEM32(ctx.ebp + 8);
    PutXmm(ctx.xmm[0], Scalar(F32(base, s + 0x18)));
    ctx.eax += 16;
    StF(base + s + 0x24, ctx.xmm[0].f[0]);
    X86_MEM32(s + 0x20) = ctx.eax;
    ctx.esi = X86_MEM32(ctx.ebp + 8);
    ctx.edi = X86_MEM32(s + 0x20);
    f4 x0 = Load4(base, ctx.edi) - Load4(base, ctx.esi);
    PutXmm(ctx.xmm[1], Splat(F32(base, s + 0x24)));
    x0 = VMul(x0, GetXmm(ctx.xmm[1]));
    x0 = VAdd(x0, Load4(base, ctx.esi));
    PutXmm(ctx.xmm[0], x0);
    Store4(base, s + 0x40, x0);
    ctx.edx = X86_MEM32(s + 0x40);
    ctx.eax = X86_MEM32(s + 0x44);
    ctx.ecx = ctx.ebx;
    X86_MEM32(ctx.ecx) = ctx.edx;
    ctx.edx = X86_MEM32(s + 0x48);
    X86_MEM32(ctx.ecx + 4) = ctx.eax;
    ctx.eax = X86_MEM32(s + 0x4c);
    X86_MEM32(ctx.ecx + 8) = ctx.edx;
    X86_MEM32(ctx.ecx + 12) = ctx.eax;
    ctx.ecx = s + 0xA0;
    Push(ctx, base, ctx.ecx);
    ctx.ecx = s + 0x90;
    ctx.eax = s + 0x80;
    ctx.esi = s + 0xB0;
    Push(ctx, base, 0xD1F11);
    sub_000CD040(ctx, base);
    PutXmm(ctx.xmm[0], Load4(base, ctx.eax));
    ctx.eax = X86_MEM32(s + 0x2c);
    ctx.edx = X86_MEM32(s + 0x1c);
    ctx.ecx = X86_MEM32(s + 0x28);
    Store4(base, ctx.ebx + 0x10, GetXmm(ctx.xmm[0]));
    PutXmm(ctx.xmm[0], Scalar(F32(base, s + 0x18)));
    StF(base + ctx.ebx + 0x50, ctx.xmm[0].f[0]);
    PutXmm(ctx.xmm[0], Scalar(F32(base, s + 0x34)));
    StF(base + ctx.ebx + 0x54, ctx.xmm[0].f[0]);
    PutXmm(ctx.xmm[0], Scalar(F32(base, s + 0x3c)));
    StF(base + ctx.ebx + 0x58, ctx.xmm[0].f[0]);
    PutXmm(ctx.xmm[0], Load4(base, s + 0x80));
    X86_MEM32(ctx.ebx + 0x68) = ctx.eax;
    X86_MEM32(ctx.ebx + 0x60) = ctx.edx;
    X86_MEM32(ctx.ebx + 0x64) = ctx.ecx;
    Store4(base, ctx.ebx + 0x20, GetXmm(ctx.xmm[0]));
    PutXmm(ctx.xmm[0], Load4(base, s + 0x90));
    ctx.esp += 4;
    Store4(base, ctx.ebx + 0x30, GetXmm(ctx.xmm[0]));
    PutXmm(ctx.xmm[0], Load4(base, s + 0xA0));
    ctx.edi = Pop(ctx, base);
    ctx.esi = Pop(ctx, base);
    Store4(base, ctx.ebx + 0x40, GetXmm(ctx.xmm[0]));
    ctx.eax = 1;
    ctx.ebx = Pop(ctx, base);
    ctx.esp = ctx.ebp;
    ctx.ebp = Pop(ctx, base);
    ctx.esp += 4;
}

} // namespace
X86_FUNC(sub_000D1D10) { MeshRay(ctx, base); }

#ifndef BLACK_VERIFY_HOT

X86_FUNC(sub_000205A0) { DecodePosition(ctx, base); }
X86_FUNC(sub_000CCB30) { RayTriangle(ctx, base); }
X86_FUNC(sub_000CC6C0) { SphereSegment(ctx, base); }

#else

#include "game/hot_verify.h"

X86_EXTERN_FUNC(__imp__sub_000205A0);
X86_EXTERN_FUNC(__imp__sub_000CCB30);
X86_EXTERN_FUNC(__imp__sub_000CC6C0);

namespace {

std::atomic<uint64_t> g_decode_calls{0}, g_decode_bad{0};
std::atomic<uint64_t> g_ray_calls{0}, g_ray_bad{0};
std::atomic<uint64_t> g_sphere_calls{0}, g_sphere_bad{0};
std::atomic<int> g_synthetic{0};

// Synthetic calls, once per function on its first call, in the dead stack 0x800 below the live esp: the inputs at t,
// the call's esp at t - 0x40 (a zero return address there), all of [t - 0x200, t + 0x200) compared and put back.
void Synthetic(int which, const char* name, X86Context& ctx, uint8_t* base, void (*native)(X86Context&, uint8_t*),
               void (*original)(X86Context&, uint8_t*)) {
    if (g_synthetic.fetch_or(1 << which) & (1 << which)) return;
    const uint32_t t = (ctx.esp - 0x800) & ~15u, e = t - 0x40;
    char what[96];
    if (which == 0) {
        // sub_000205A0: mesh at t (vertices at t + 0x20, offsets 3, -7, 100), vertex 1 = (1000, 2, 65535) at t + 0x26,
        // out at t + 0x40; w is the frame slot at ((e - 4) & ~15) - 4
        static const uint32_t ws[] = {0x7FC12345u, 0xFFC54321u, 0x7F812345u, 0xFF800001u, 0x00000003u, 0x3F800000u};
        for (uint32_t w : ws) {
            std::snprintf(what, sizeof what, "%s w %08X", name, w);
            VerifyCase(what, ctx, base, native, original, t - 0x200, 0x400, [&](X86Context& c) {
                X86_MEM32(t + 4) = t + 0x20;
                X86_MEM8(t + 0xA) = 3;
                X86_MEM8(t + 0xB) = uint8_t(-7);
                X86_MEM8(t + 0xC) = 100;
                X86_MEM16(t + 0x26) = 1000;
                X86_MEM16(t + 0x28) = 2;
                X86_MEM16(t + 0x2A) = 65535;
                X86_MEM32(((e - 4) & ~15u) - 4) = w;
                X86_MEM32(e) = 0;
                c.ecx = t;
                c.edx = 1;
                c.eax = t + 0x40;
                c.esp = e;
            });
        }
        return;
    }
    for (int k = 0; k < kNanCaseCount; k++) {
        std::snprintf(what, sizeof what, "%s %s", name, kNanCases[k].what);
        VerifyCase(what, ctx, base, native, original, t - 0x200, 0x400, [&](X86Context& c) {
            X86_MEM32(e) = 0;
            c.esp = e;
            if (which == 1) {
                // sub_000CCB30: corners (0,0,0), (1,0,0), (0,1,0) at t, t + 0x10, t + 0x20; the ray (0.25, 0.25, 1) to
                // (0.25, 0.25, -1) at t + 0x30: a hit without NaNs; t, u, v to t + 0x50, 0x54, 0x58
                PutNanVector(base, t, 0, 0, 0, 1, 0, k);
                PutNanVector(base, t + 0x10, 1, 0, 0, 1, 1, k);
                PutNanVector(base, t + 0x20, 0, 1, 0, 1, 2, k);
                PutNanVector(base, t + 0x30, 0.25f, 0.25f, 1, 1, 3, k);
                PutNanVector(base, t + 0x40, 0.25f, 0.25f, -1, 1, 4, k);
                X86_MEM32(e + 4) = t + 0x20;
                X86_MEM32(e + 8) = t + 0x50;
                X86_MEM32(e + 0xC) = t + 0x54;
                X86_MEM32(e + 0x10) = t + 0x58;
                c.ecx = t;
                c.edx = t + 0x10;
                c.eax = t + 0x30;
            } else {
                // sub_000CC6C0: the unit sphere at the origin (t), the segment (-5, 0.5, 0) to (5, 0.5, 0) at t + 0x10:
                // both ends outside, the closest point inside; the radius a NaN only when every lane is
                PutNanVector(base, t, 0, 0, 0, 1, 0, k);
                if (kNanCases[k].lanes != 15) X86_MEMF32(t + 0xC) = 1;
                PutNanVector(base, t + 0x10, -5, 0.5f, 0, 1, 1, k);
                PutNanVector(base, t + 0x20, 5, 0.5f, 0, 1, 2, k);
                c.eax = t;
                c.ecx = t + 0x10;
            }
        });
    }
}

}  // namespace

X86_FUNC(sub_000205A0) {
    Synthetic(0, "sub_000205A0", ctx, base, DecodePosition, __imp__sub_000205A0);
    const Region regions[] = {{ctx.esp - 0x40, 0x40}, {ctx.eax, 16}};
    Verify("sub_000205A0", ctx, base, DecodePosition, __imp__sub_000205A0, regions, g_decode_calls, g_decode_bad);
}

X86_FUNC(sub_000CCB30) {
    Synthetic(1, "sub_000CCB30", ctx, base, RayTriangle, __imp__sub_000CCB30);
    const uint32_t e = ctx.esp;
    const Region regions[] = {{e - 0xA0, 0xA0}, {X86_MEM32(e + 8), 4}, {X86_MEM32(e + 0xC), 4}, {X86_MEM32(e + 0x10), 4}};
    Verify("sub_000CCB30", ctx, base, RayTriangle, __imp__sub_000CCB30, regions, g_ray_calls, g_ray_bad);
    static std::atomic<uint64_t> hits{0};
    if ((ctx.eax & 0xFF) && ((++hits & (hits - 1)) == 0) && hits >= 1024)
        std::fprintf(stderr, "verify sub_000CCB30: %llu hits\n", (unsigned long long)hits.load());
}

X86_FUNC(sub_000CC6C0) {
    Synthetic(2, "sub_000CC6C0", ctx, base, SphereSegment, __imp__sub_000CC6C0);
    const uint32_t e = ctx.esp;
    const Region regions[] = {{e - 0x60, 0x60}};
    Verify("sub_000CC6C0", ctx, base, SphereSegment, __imp__sub_000CC6C0, regions, g_sphere_calls, g_sphere_bad);
    static std::atomic<uint64_t> seen[3];
    uint32_t r = ctx.eax < 3 ? ctx.eax : 0;
    uint64_t n = ++seen[r];
    if ((n & (n - 1)) == 0 && n >= 1024) std::fprintf(stderr, "verify sub_000CC6C0: %llu results of %u\n", (unsigned long long)n, r);
}

#endif
