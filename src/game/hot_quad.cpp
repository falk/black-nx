// Native replacements of two particle quad builders: four corners of a camera-facing quad, each the particle's centre
// plus a corner offset, written as 0x20-byte vertices (x, y, z, 1.0, colour) at 0x53CAE8 + 0x20 * index - 8. Called
// once per particle drawn. sub_0015CFF0 builds an upright quad, sub_0015C620 one rotated by an angle that grows with
// the particle's age (sin and cos on the x87 stack, in double precision as the generated code takes fsin and fcos).
//
// The rewrite is a transliteration with the x86 registers in locals: every store the original makes (its frame, the
// statics at 0x565670 it sets up once, the vertices), the same operand order, the same call to sub_00011C10 (through
// the dispatcher symbol, so its own native rewrite runs) with the stack and registers as the original makes the call,
// and the registers and xmm it leaves. Build with BLACK_VERIFY_HOT to run the original after every call and count the
// calls where anything differs.
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "game/hot_colour.h"
#include "game/hot_synth.h"
#include "game/hot_verify.h"

using namespace hot;

X86_EXTERN_FUNC(__imp__sub_0015CFF0);
X86_EXTERN_FUNC(__imp__sub_0015C620);
X86_EXTERN_FUNC(sub_00011C10);

namespace {

#ifdef BLACK_VERIFY_HOT
PathCounts g_quad_paths{{}, {"native", "first call", "operands-in-frame"}};
PathCounts g_spin_paths{{}, {"native", "first call", "operands-in-frame"}};
#endif

inline bool Overlap(uint32_t a, uint64_t an, uint32_t b, uint64_t bn) {
    return uint64_t(a) < uint64_t(b) + bn && uint64_t(b) < uint64_t(a) + an;
}
inline f4 Xor(f4 a, f4 b) {
    uint32_t x[4], y[4];
    std::memcpy(x, &a, 16);
    std::memcpy(y, &b, 16);
    for (int i = 0; i < 4; i++) x[i] ^= y[i];
    f4 r;
    std::memcpy(&r, x, 16);
    return r;
}
inline f4 SplatBits(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return Splat(f);
}
inline f4 GetXmm(const X86Xmm& x) {
    f4 v;
    std::memcpy(&v, &x, 16);
    return v;
}

// sub_0015CFF0 (cdecl: object, emitter, index, unused, centre, corner, uv)
// Frame: push ebp, the alignment, sub esp, 0x84, push ebx, push esi, push edi (esp = s from then on)
static void ParticleQuad(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp;
    const uint32_t A = (e - 4) & ~15u, s = A - 0x90;
    const uint32_t obj = X86_MEM32(e + 4), emitter = X86_MEM32(e + 8), index = X86_MEM32(e + 0xC);
    const uint32_t up = X86_MEM32(e + 0x14), centre = X86_MEM32(e + 0x18), uv = X86_MEM32(e + 0x1C);
    const uint32_t out = 0x53CAE8u + (index << 5);
    // operands the original reads after it writes its frame (or the callee's, 0x40 below it), or vertices that
    // land on what it reads later
    const uint32_t lo = s - 0x40, n = e - lo;
    if (Overlap(obj, 0x140, lo, n) || Overlap(emitter + 0x20, 8, lo, n) || Overlap(up, 16, lo, n) ||
        Overlap(centre, 16, lo, n) || Overlap(uv, 8, lo, n) || Overlap(out - 8, 0x80, lo, n) ||
        Overlap(out - 8, 0x80, centre, 16) || Overlap(out - 8, 0x80, 0x565670, 0x14)) {
        HOT_PATH(g_quad_paths, 2);
        __imp__sub_0015CFF0(ctx, base);
        return;
    }
    uint32_t eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx, ebx = ctx.ebx, esi = ctx.esi, edi = ctx.edi;
    const uint32_t ebx0 = ebx, esi0 = esi, edi0 = edi;
    f4 x0 = GetXmm(ctx.xmm[0]), x1 = GetXmm(ctx.xmm[1]), x2 = GetXmm(ctx.xmm[2]), x3 = GetXmm(ctx.xmm[3]);
    X86_MEM32(e - 4) = ctx.ebp;
    const uint32_t ebp = e - 4;
    ecx = (ecx & 0xFFFFFF00u) | X86_MEM8(0x565680u);
    X86_MEM32(s + 8) = ebx;
    eax = 1;
    const bool done = (uint8_t(eax) & uint8_t(ecx)) != 0;
    X86_MEM32(s + 4) = esi;
    X86_MEM32(s) = edi;
    if (!done) {
        HOT_PATH(g_quad_paths, 1);
        ecx = X86_MEM32(0x565680u);
        const uint32_t c = X86_MEM32(0x295C8Cu);
        x0 = Scalar(F32(base, 0x295C8Cu));
        ecx |= eax;
        X86_MEM32(0x565670u) = c;
        X86_MEM32(0x565674u) = c;
        x0 = Splat(0.0f);
        X86_MEM32(0x565680u) = ecx;
        X86_MEM32(0x565678u) = 0;
    } else {
        HOT_PATH(g_quad_paths, 0);
    }
    x0 = Scalar(F32(base, 0x295C70u));
    ebx = centre;
    x2 = Load4(base, ebx);
    edi = obj;
    x3 = Load4(base, 0x565670u);
    x1 = Load4(base, edi + 0x130);
    X86_MEM32(s + 0x14) = X86_MEM32(0x295C70u);
    x0 = Splat(F32(base, s + 0x14));
    x3 = VAdd(x3, x2);
    x3 = VMul(x3, x0);
    x0 = Scalar(F32(base, 0x295C74u));
    x3 = VMul(x3, x1);
    Store4(base, s + 0x20, x3);
    eax = s + 0x20;
    X86_MEM32(s + 0x28) = X86_MEM32(0x295C74u);
    // call sub_00011C10 with the original's registers and return address
    ctx.eax = eax;
    ctx.ecx = ecx;
    ctx.edx = edx;
    ctx.ebx = ebx;
    ctx.esi = esi;
    ctx.edi = edi;
    ctx.ebp = ebp;
    PutXmm(ctx.xmm[0], x0);
    PutXmm(ctx.xmm[1], x1);
    PutXmm(ctx.xmm[2], x2);
    PutXmm(ctx.xmm[3], x3);
    ctx.esp = s - 4;
    X86_MEM32(s - 4) = 0x15D095u;
    sub_00011C10(ctx, base);
    eax = ctx.eax;
    edx = ctx.edx;
    // the unit vector at [esp+0x20], negated
    ecx = s + 0x20;
    X86_MEM32(s + 0x18) = ecx;
    ecx = 0x80000000u;
    X86_MEM32(s + 0x14) = ecx;
    esi = s + 0x20;
    x0 = Xor(SplatBits(ecx), Load4(base, esi));
    Store4(base, s + 0x30, x0);
    eax = X86_MEM32(s + 0x34);
    x1 = Load4(base, edi);
    edx = X86_MEM32(s + 0x30);
    x0 = Load4(base, s + 0x30);
    x2 = Splat(0.0f);
    x0 = VMul(x0, x1);
    x1 = Sh<0x39>(x0, x0);
    X86_MEM32(s + 0x24) = eax;
    eax = X86_MEM32(s + 0x3C);
    X86_MEM32(s + 0x20) = edx;
    edx = X86_MEM32(s + 0x38);
    x0[0] = SAdd(x0[0], x1[0]);
    X86_MEM32(s + 0x2C) = eax;
    eax = up;
    x3 = Sh<0x39>(x1, x1);
    x0[0] = SAdd(x0[0], x3[0]);
    x3 = Load4(base, eax);
    eax = emitter;
    X86_MEM32(s + 0x28) = edx;
    X86_MEM32(s + 0x14) = 0;
    X86_MEMF32(s + 0x14) = x0[0];
    x0 = Scalar(F32(base, eax + 0x24));
    x0[0] = SMul(x0[0], F32(base, s + 0x14));
    x0[0] = SAdd(x0[0], F32(base, eax + 0x20));
    edx = edi + 0x20;
    edi = edi + 0x10;
    X86_MEMF32(s + 0x18) = x0[0];
    X86_MEM32(s + 0x14) = edx;
    X86_MEM32(s + 0x1C) = edi;
    esi = edi;
    edi = edx;
    // lerp of the object's two vectors at +0x10 and +0x20 by the clamped facing
    x0 = VSub(Load4(base, edi), Load4(base, esi));
    x1 = Splat(F32(base, s + 0x18));
    x0 = VMul(x0, x1);
    x0 = VAdd(x0, Load4(base, esi));
    Store4(base, s + 0x40, x0);
    x0 = Scalar(F32(base, 0x295CB4u));
    x1 = Load4(base, s + 0x40);
    X86_MEM32(s + 0x1C) = X86_MEM32(0x295CB4u);
    eax = s + 0x1C;
    x0 = Splat(F32(base, s + 0x1C));
    eax = uv;
    x3 = VMul(x3, x1);
    x3 = VMul(x3, x0);
    x0 = x2;
    x0[0] = x0[0] - F32(base, eax);
    X86_MEMF32(s + 0x50) = x0[0];
    x1 = x2;
    x1[0] = x1[0] - F32(base, eax + 4);
    X86_MEMF32(s + 0x60) = x0[0];
    x0 = Scalar(F32(base, eax + 4));
    edx = s + 0x50;
    Store4(base, s + 0x40, x3);
    X86_MEMF32(s + 0x54) = x1[0];
    X86_MEMF32(s + 0x58) = x2[0];
    X86_MEMF32(s + 0x64) = x0[0];
    X86_MEMF32(s + 0x68) = x2[0];
    X86_MEM32(s + 0x18) = edx;
    X86_MEM32(s + 0x1C) = ecx;
    // the two corners negated into [esp+0x70] and [esp+0x80]
    esi = edx;
    x0 = Xor(SplatBits(ecx), Load4(base, esi));
    Store4(base, s + 0x30, x0);
    eax = X86_MEM32(s + 0x30);
    edx = X86_MEM32(s + 0x34);
    X86_MEM32(s + 0x70) = eax;
    eax = X86_MEM32(s + 0x38);
    X86_MEM32(s + 0x74) = edx;
    edx = X86_MEM32(s + 0x3C);
    X86_MEM32(s + 0x78) = eax;
    eax = s + 0x60;
    X86_MEM32(s + 0x7C) = edx;
    X86_MEM32(s + 0x18) = eax;
    X86_MEM32(s + 0x1C) = ecx;
    esi = eax;
    x0 = Xor(SplatBits(ecx), Load4(base, esi));
    Store4(base, s + 0x30, x0);
    ecx = X86_MEM32(s + 0x30);
    edx = X86_MEM32(s + 0x34);
    eax = X86_MEM32(s + 0x38);
    x0 = Scalar(F32(base, s + 0x4C));
    X86_MEM32(s + 0x80) = ecx;
    ecx = X86_MEM32(s + 0x3C);
    X86_MEM32(s + 0x84) = edx;
    X86_MEM32(s + 0x88) = eax;
    X86_MEM32(s + 0x8C) = ecx;
    // the colour: four truncated lanes of [esp+0x40] (a, r, g, b) packed as bytes
    // All four channels are independent; keep SSE conversion semantics in NEON.
    ecx = PackColour(Load4(base, s + 0x40));
    X86_MEM32(s + 0x1C) = X86_MEM32(s + 0x48);
    eax = out;
    edx = s + 0x50;
    const float one = F32(base, 0x295C74u);
    const f4 origin = Load4(base, ebx);
    auto emit = [&](uint32_t i) {
        const uint32_t corner = s + 0x50 + i * 16, vertex = out + i * 32;
        x1 = VAdd(Load4(base, corner), origin);
        Store4(base, corner, x1);
        X86_MEM32(vertex + 8) = ecx;
        Store4(base, vertex - 8, f4{x1[0], x1[1], x1[2], one});
    };
    emit(0); emit(1); emit(2); emit(3);
    x0 = Scalar(one);
    eax += 0x80;
    edx += 0x40;
    ctx.eax = eax;
    ctx.ecx = ecx;
    ctx.edx = edx;
    ctx.ebx = ebx0;
    ctx.esi = esi0;
    ctx.edi = edi0;
    PutXmm(ctx.xmm[0], x0);
    PutXmm(ctx.xmm[1], x1);
    PutXmm(ctx.xmm[2], x2);
    PutXmm(ctx.xmm[3], x3);
    ctx.ebp = X86_MEM32(e - 4);
    ctx.esp = e + 4;
}

// sub_0015C620 (cdecl: object, emitter, index, age, up, centre, uv)
// Frame: push ebp, the alignment, sub esp, 0xA4, push ebx, push esi, push edi (esp = s from then on)
static void SpinQuad(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp;
    const uint32_t A = (e - 4) & ~15u, s = A - 0xB0;
    const uint32_t obj = X86_MEM32(e + 4), emitter = X86_MEM32(e + 8), index = X86_MEM32(e + 0xC);
    const uint32_t up = X86_MEM32(e + 0x14), centre = X86_MEM32(e + 0x18), uv = X86_MEM32(e + 0x1C);
    const uint32_t out = 0x53CAE8u + (index << 5);
    const uint32_t lo = s - 0x40, n = e - lo;
    if (Overlap(obj, 0x140, lo, n) || Overlap(emitter, 0x28, lo, n) || Overlap(up, 16, lo, n) ||
        Overlap(centre, 16, lo, n) || Overlap(uv, 16, lo, n) || Overlap(out - 8, 0x80, lo, n) ||
        Overlap(out - 8, 0x80, centre, 16) || Overlap(out - 8, 0x80, 0x565650, 0x14)) {
        HOT_PATH(g_spin_paths, 2);
        __imp__sub_0015C620(ctx, base);
        return;
    }
    uint32_t eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx, ebx = ctx.ebx, esi = ctx.esi, edi = ctx.edi;
    const uint32_t ebx0 = ebx, esi0 = esi, edi0 = edi;
    f4 x0 = GetXmm(ctx.xmm[0]), x1 = GetXmm(ctx.xmm[1]), x2 = GetXmm(ctx.xmm[2]), x3 = GetXmm(ctx.xmm[3]);
    X86_MEM32(e - 4) = ctx.ebp;
    const uint32_t ebp = e - 4;
    ecx = (ecx & 0xFFFFFF00u) | X86_MEM8(0x565660u);
    X86_MEM32(s + 8) = ebx;
    eax = 1;
    const bool done = (uint8_t(eax) & uint8_t(ecx)) != 0;
    X86_MEM32(s + 4) = esi;
    X86_MEM32(s) = edi;
    if (!done) {
        HOT_PATH(g_spin_paths, 1);
        ecx = X86_MEM32(0x565660u);
        const uint32_t c = X86_MEM32(0x295C8Cu);
        ecx |= eax;
        X86_MEM32(0x565650u) = c;
        X86_MEM32(0x565654u) = c;
        x0 = Splat(0.0f);
        X86_MEM32(0x565660u) = ecx;
        X86_MEM32(0x565658u) = 0;
    } else {
        HOT_PATH(g_spin_paths, 0);
    }
    edi = obj;
    ebx = emitter;
    // the angle: age clamped to the object's [0x98] both ways, through a quadratic of the object, times the
    // emitter's rate, plus the global time
    x0 = Scalar(F32(base, edi + 0x98));
    x2 = Scalar(F32(base, e + 0x10));
    x1 = Scalar(F32(base, ebx + 0x1C));
    X86_MEMF32(s + 0x1C) = x0[0];
    X86_MEMF32(s + 0x58) = x2[0];
    x0 = Scalar(F32(base, s + 0x58));
    x0[0] = sse_min(x0[0], F32(base, s + 0x1C));
    X86_MEMF32(s + 0x5C) = x0[0];
    x0 = Scalar(F32(base, edi + 0x98));
    X86_MEMF32(s + 0x58) = x0[0];
    X86_MEMF32(s + 0x1C) = x2[0];
    x0 = Scalar(F32(base, s + 0x1C));
    x0[0] = sse_max(x0[0], F32(base, s + 0x58));
    X86_MEMF32(s + 0x18) = x0[0];
    x0 = Scalar(F32(base, s + 0x5C));
    x2 = Scalar(F32(base, edi + 0x9C));
    x3 = Scalar(F32(base, edi + 0x94));
    x3[0] = SMul(x3[0], F32(base, s + 0x18));
    x2[0] = SMul(x2[0], x0[0]);
    x2[0] = SMul(x2[0], x0[0]);
    x2[0] = SAdd(x2[0], x3[0]);
    x3 = Scalar(F32(base, edi + 0x90));
    x3[0] = SMul(x3[0], x0[0]);
    x2[0] = SAdd(x2[0], x3[0]);
    x2[0] = SAdd(x2[0], F32(base, edi + 0xA0));
    x2[0] = SMul(x2[0], x1[0]);
    x2[0] = SAdd(x2[0], F32(base, 0x2F81B0u));
    X86_MEMF32(s + 0x1C) = x2[0];
    // fld, fsin, fstp; fld, fcos, fstp: the slot below the top keeps the cosine
    const uint32_t t = ctx.top;
    const double angle = double(F32(base, s + 0x1C));
    X86_MEMF32(s + 0x3C) = float(std::sin(angle));
    const double cosine = std::cos(angle);
    X86_MEMF32(s + 0x14) = float(cosine);
    ctx.st[(t - 1) & 7] = cosine;
    ctx.fsw &= ~0x400;
    x0 = Scalar(F32(base, 0x295C70u));
    ecx = centre;
    x2 = Load4(base, ecx);
    x3 = Load4(base, 0x565650u);
    x1 = Load4(base, edi + 0x130);
    X86_MEM32(s + 0x18) = X86_MEM32(0x295C70u);
    x0 = Splat(F32(base, s + 0x18));
    x3 = VAdd(x3, x2);
    x3 = VMul(x3, x0);
    x0 = Scalar(F32(base, 0x295C74u));
    x3 = VMul(x3, x1);
    Store4(base, s + 0x20, x3);
    eax = s + 0x20;
    X86_MEM32(s + 0x28) = X86_MEM32(0x295C74u);
    ctx.eax = eax;
    ctx.ecx = ecx;
    ctx.edx = edx;
    ctx.ebx = ebx;
    ctx.esi = esi;
    ctx.edi = edi;
    ctx.ebp = ebp;
    PutXmm(ctx.xmm[0], x0);
    PutXmm(ctx.xmm[1], x1);
    PutXmm(ctx.xmm[2], x2);
    PutXmm(ctx.xmm[3], x3);
    ctx.esp = s - 4;
    X86_MEM32(s - 4) = 0x15C784u;
    sub_00011C10(ctx, base);
    edx = s + 0x20;
    eax = 0x80000000u;
    X86_MEM32(s + 0x5C) = edx;
    X86_MEM32(s + 0x18) = eax;
    esi = edx;
    x0 = Xor(SplatBits(eax), Load4(base, esi));
    Store4(base, s + 0x40, x0);
    ecx = X86_MEM32(s + 0x40);
    x1 = Load4(base, edi);
    x0 = Load4(base, s + 0x40);
    edx = X86_MEM32(s + 0x44);
    x3 = Splat(0.0f);
    x0 = VMul(x0, x1);
    x1 = Sh<0x39>(x0, x0);
    X86_MEM32(s + 0x20) = ecx;
    ecx = X86_MEM32(s + 0x48);
    x0[0] = SAdd(x0[0], x1[0]);
    X86_MEM32(s + 0x28) = ecx;
    X86_MEM32(s + 0x24) = edx;
    edx = X86_MEM32(s + 0x4C);
    x2 = Sh<0x39>(x1, x1);
    x0[0] = SAdd(x0[0], x2[0]);
    X86_MEM32(s + 0x1C) = 0;
    X86_MEMF32(s + 0x1C) = x0[0];
    x0 = Scalar(F32(base, ebx + 0x24));
    x0[0] = SMul(x0[0], F32(base, s + 0x1C));
    x0[0] = SAdd(x0[0], F32(base, ebx + 0x20));
    ecx = edi + 0x20;
    X86_MEM32(s + 0x2C) = edx;
    edx = up;
    x2 = Load4(base, edx);
    edi += 0x10;
    X86_MEMF32(s + 0x18) = x0[0];
    X86_MEM32(s + 0x5C) = ecx;
    X86_MEM32(s + 0x58) = edi;
    esi = edi;
    edi = ecx;
    x0 = VSub(Load4(base, edi), Load4(base, esi));
    x1 = Splat(F32(base, s + 0x18));
    x0 = VMul(x0, x1);
    x0 = VAdd(x0, Load4(base, esi));
    Store4(base, s + 0xA0, x0);
    x0 = Scalar(F32(base, 0x295CB4u));
    x1 = Load4(base, s + 0xA0);
    ecx = uv;
    X86_MEM32(s + 0x18) = X86_MEM32(0x295CB4u);
    edx = s + 0x18;
    x0 = Splat(F32(base, s + 0x18));
    x2 = VMul(x2, x1);
    x1 = Scalar(F32(base, s + 0x3C));
    x2 = VMul(x2, x0);
    x0 = x3;
    x0[0] = x0[0] - x1[0];
    X86_MEMF32(s + 0x40) = x0[0];
    x0 = Load4(base, ecx);
    Store4(base, s + 0xA0, x2);
    x2 = Scalar(F32(base, s + 0x14));
    X86_MEMF32(s + 0x44) = x2[0];
    X86_MEMF32(s + 0x48) = x3[0];
    const f4 x4 = Load4(base, s + 0x40);
    f4 x5 = x0;
    X86_MEMF32(s + 0x40) = x2[0];
    X86_MEMF32(s + 0x44) = x1[0];
    X86_MEMF32(s + 0x48) = x3[0];
    x1 = Load4(base, s + 0x40);
    edx = s + 0x60;
    x5 = VMul(x5, x4);
    x0 = VMul(x0, x1);
    Store4(base, s + 0x60, x5);
    Store4(base, s + 0x70, x0);
    X86_MEM32(s + 0x3C) = edx;
    X86_MEM32(s + 0x14) = eax;
    // the two corners negated into [esp+0x80] and [esp+0x90]
    esi = edx;
    x0 = Xor(SplatBits(X86_MEM32(s + 0x14)), Load4(base, esi));
    Store4(base, s + 0x40, x0);
    ecx = X86_MEM32(s + 0x40);
    edx = X86_MEM32(s + 0x44);
    X86_MEM32(s + 0x80) = ecx;
    ecx = X86_MEM32(s + 0x48);
    X86_MEM32(s + 0x84) = edx;
    edx = X86_MEM32(s + 0x4C);
    X86_MEM32(s + 0x88) = ecx;
    ecx = s + 0x70;
    X86_MEM32(s + 0x8C) = edx;
    X86_MEM32(s + 0x3C) = ecx;
    X86_MEM32(s + 0x14) = eax;
    esi = ecx;
    x0 = Xor(SplatBits(X86_MEM32(s + 0x14)), Load4(base, esi));
    Store4(base, s + 0x40, x0);
    edx = X86_MEM32(s + 0x40);
    eax = X86_MEM32(s + 0x44);
    ecx = X86_MEM32(s + 0x48);
    x0 = Scalar(F32(base, s + 0xAC));
    X86_MEM32(s + 0x90) = edx;
    edx = X86_MEM32(s + 0x4C);
    X86_MEM32(s + 0x94) = eax;
    X86_MEM32(s + 0x98) = ecx;
    X86_MEM32(s + 0x9C) = edx;
    // All four channels are independent; keep SSE conversion semantics in NEON.
    ecx = PackColour(Load4(base, s + 0xA0));
    X86_MEM32(s + 0x14) = X86_MEM32(s + 0xA8);
    eax = out;
    edx = s + 0x60;
    const float one = F32(base, 0x295C74u);
    const f4 origin = Load4(base, centre);
    auto emit = [&](uint32_t i) {
        const uint32_t corner = s + 0x60 + i * 16, vertex = out + i * 32;
        x1 = VAdd(Load4(base, corner), origin);
        Store4(base, corner, x1);
        X86_MEM32(vertex + 8) = ecx;
        Store4(base, vertex - 8, f4{x1[0], x1[1], x1[2], one});
    };
    emit(0); emit(1); emit(2); emit(3);
    x0 = Scalar(one);
    eax += 0x80;
    edx += 0x40;
    ctx.eax = eax;
    ctx.ecx = ecx;
    ctx.edx = edx;
    ctx.ebx = ebx0;
    ctx.esi = esi0;
    ctx.edi = edi0;
    PutXmm(ctx.xmm[0], x0);
    PutXmm(ctx.xmm[1], x1);
    PutXmm(ctx.xmm[2], x2);
    PutXmm(ctx.xmm[3], x3);
    PutXmm(ctx.xmm[4], x4);
    PutXmm(ctx.xmm[5], x5);
    ctx.ebp = X86_MEM32(e - 4);
    ctx.esp = e + 4;
}

}  // namespace

#ifndef BLACK_VERIFY_HOT

X86_FUNC(sub_0015CFF0) {
    ParticleQuad(ctx, base);
}
X86_FUNC(sub_0015C620) {
    SpinQuad(ctx, base);
}

#else

namespace {

std::atomic<uint64_t> g_quad_calls{0}, g_quad_bad{0}, g_spin_calls{0}, g_spin_bad{0};

// once per builder, synthetic calls in the dead stack 0x800 below esp: an object (its vectors at +0, +0x10, +0x20,
// +0x130, its spin terms at +0x90..+0xA0), an emitter, up, centre and uv with NaNs of distinct bits in the lanes each
// NaN case names, both with the statics set up and not (the first call). Vertex slot 0 and the statics are put back.
void SyntheticBuilder(const char* fn, X86Context& live, uint8_t* base, void (*native)(X86Context&, uint8_t*),
                      void (*original)(X86Context&, uint8_t*), uint32_t statics, uint32_t flag) {
    const uint32_t t = (live.esp - 0x800) & ~15u;
    for (int k = 0; k < kNanCaseCount; k++)
        for (int first = 0; first < 2; first++)
            SyntheticCall(std::string(fn) + " " + kNanCases[k].what + (first ? ", first call" : "") +
                              (k ? " [reference not NaN-exact, a difference is expected]" : ""),
                          live, base, native,
                          original, t, {{0x53CAE0u, 0x80}, {statics, 0x14}}, [&](X86Context& c) {
                const uint32_t obj = t, em = t + 0x140, up = t + 0x180, centre = t + 0x190, uv = t + 0x1A0;
                for (uint32_t i = 0; i < 0x140; i += 4) X86_MEMF32(obj + i) = 0.125f * float(i % 37) - 1.0f;
                PutNanVector(base, obj, 0.25f, 0.5f, -0.75f, 1.0f, 0, k);
                PutNanVector(base, obj + 0x10, 200.0f, 100.0f, 50.0f, 255.0f, 1, k);
                PutNanVector(base, obj + 0x20, 10.0f, 250.0f, 300.0f, -20.0f, 2, k);
                PutNanVector(base, obj + 0x130, 1.5f, 1.5f, 0.5f, 1.0f, 3, k);
                X86_MEMF32(obj + 0x98) = 2.0f;
                for (uint32_t i = 0; i < 0x30; i += 4) X86_MEMF32(em + i) = 0.5f + 0.25f * float(i / 4);
                PutNanVector(base, up, 0.0f, 1.0f, 0.0f, 0.0f, 4, k);
                PutNanVector(base, centre, 5.0f, -3.0f, 12.0f, 1.0f, 5, k);
                PutNanVector(base, uv, 0.5f, 0.75f, 0.0f, 0.0f, 6, k);
                if (first) X86_MEM8(flag) = 0;
                c.esp = t - 0x40;
                X86_MEM32(c.esp) = 0;
                X86_MEM32(c.esp + 4) = obj;
                X86_MEM32(c.esp + 8) = em;
                X86_MEM32(c.esp + 0xC) = 0;  // vertex slot 0
                X86_MEMF32(c.esp + 0x10) = 1.25f;
                X86_MEM32(c.esp + 0x14) = up;
                X86_MEM32(c.esp + 0x18) = centre;
                X86_MEM32(c.esp + 0x1C) = uv;
            });
}
std::atomic<int> g_synthetic{0};

}  // namespace

// hot_particles.cpp's record-and-replay check of the particle batch calls the builders through sub_0015CFF0 itself;
// it runs this one for the call
void VerifyQuadBuilder(X86Context& ctx, uint8_t* base) {
    if (!(g_synthetic.fetch_or(1) & 1))
        SyntheticBuilder("sub_0015CFF0", ctx, base, ParticleQuad, __imp__sub_0015CFF0, 0x565670u, 0x565680u);
    const uint32_t e = ctx.esp;
    const uint32_t out = 0x53CAE8u + (X86_MEM32(e + 0xC) << 5);
    Ranges r;
    r.Add(e - 0x100, 0x100);
    r.Add(out - 8, 0x80);
    r.Add(0x565670u, 0x14);
    VerifyAll("sub_0015CFF0", ctx, base, ParticleQuad, __imp__sub_0015CFF0, r, g_quad_calls, g_quad_bad, &g_quad_paths);
}

void VerifySpinBuilder(X86Context& ctx, uint8_t* base) {
    if (!(g_synthetic.fetch_or(2) & 2))
        SyntheticBuilder("sub_0015C620", ctx, base, SpinQuad, __imp__sub_0015C620, 0x565650u, 0x565660u);
    const uint32_t e = ctx.esp;
    const uint32_t out = 0x53CAE8u + (X86_MEM32(e + 0xC) << 5);
    Ranges r;
    r.Add(e - 0x100, 0x100);
    r.Add(out - 8, 0x80);
    r.Add(0x565650u, 0x14);
    VerifyAll("sub_0015C620", ctx, base, SpinQuad, __imp__sub_0015C620, r, g_spin_calls, g_spin_bad, &g_spin_paths);
}

#endif
