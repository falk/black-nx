// sub_0015D320: four axis-aligned particle corners and packed colour. Keep
// temporary vectors in NEON registers, then leave the same guest scratch bytes.
#include "game/hot_colour.h"
#include "game/hot_verify.h"


using namespace hot;
X86_EXTERN_FUNC(__imp__sub_0015D320);

namespace {
bool Overlap(uint32_t a, uint64_t an, uint32_t b, uint64_t bn) {
    return uint64_t(a) < uint64_t(b) + bn && uint64_t(b) < uint64_t(a) + an;
}
f4 Flip(f4 v) {
    using u4 = uint32_t __attribute__((vector_size(16)));
    u4 u;
    std::memcpy(&u, &v, 16);
    u ^= u4{0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u};
    std::memcpy(&v, &u, 16);
    return v;
}

void Billboard(X86Context &ctx, uint8_t *base) {
    const uint32_t e = ctx.esp, s = ((e - 4) & ~15u) - 0x80;
    const uint32_t emitter = X86_MEM32(e + 8), colour = X86_MEM32(e + 0x14);
    const uint32_t centre = X86_MEM32(e + 0x18), size = X86_MEM32(e + 0x1C);
    const uint32_t out = 0x53CAE8u + (X86_MEM32(e + 0xC) << 5);
    // The original reloads centre after each vertex store. Keep that behaviour
    // for aliasing inputs; also avoid hoisting operands out of the guest frame.
    if (Overlap(emitter + 0x20, 4, s, e + 0x20 - s) || Overlap(colour, 16, s, e + 0x20 - s) ||
        Overlap(centre, 16, s, e + 0x20 - s) || Overlap(size, 8, s, e + 0x20 - s) ||
        Overlap(out - 8, 0x80, s, e + 0x20 - s) || Overlap(out - 8, 0x80, centre, 16)) {
        __imp__sub_0015D320(ctx, base);
        return;
    }
    const float scale = F32(base, emitter + 0x20), one = F32(base, 0x295C74);
    const f4 rgba = VMul(VMul(Load4(base, colour), f4{scale, scale, scale, one}), Splat(F32(base, 0x295CB4)));
    const f4 nx = VSub(Splat(0), Scalar(F32(base, size)));
    const f4 ny = VSub(Splat(0), Scalar(F32(base, size + 4)));
    f4 a = {nx[0], ny[0], 0, F32(base, s + 0x4C)};
    f4 b = {nx[0], F32(base, size + 4), 0, F32(base, s + 0x5C)};
    const f4 c = Flip(a), d = Flip(b), origin = Load4(base, centre);
    const f4 v0 = VAdd(a, origin), v1 = VAdd(b, origin), v2 = VAdd(c, origin), v3 = VAdd(d, origin);
    const uint32_t packed = PackColour(rgba);
    X86_MEM32(e - 4) = ctx.ebp;
    X86_MEM32(s + 8) = ctx.ebx;
    X86_MEM32(s + 4) = ctx.esi;
    X86_MEM32(s) = ctx.edi;
    X86_MEM32(s + 0x18) = s + 0x50;
    X86_MEM32(s + 0x1C) = Bits(rgba[2]);
    Store4(base, s + 0x20, d);
    Store4(base, s + 0x30, rgba);
    // Explicit corners avoid a temporary array and loop spills on Cortex-A57.
    auto emit = [&](unsigned i, f4 v) {
        Store4(base, s + 0x40 + i * 16, v);
        X86_MEM32(out + i * 32 + 8) = packed;
        Store4(base, out + i * 32 - 8, f4{v[0], v[1], v[2], one});
    };
    emit(0, v0);
    emit(1, v1);
    emit(2, v2);
    emit(3, v3);
    ctx.eax = out + 0x80;
    ctx.ecx = packed;
    ctx.edx = s + 0x80;
    PutXmm(ctx.xmm[0], origin);
    PutXmm(ctx.xmm[1], v3);
    PutXmm(ctx.xmm[2], Scalar(one));
    PutXmm(ctx.xmm[3], ny);
    ctx.esp = e + 4;
}
} // namespace

#ifndef BLACK_VERIFY_HOT
X86_FUNC(sub_0015D320) {
    Billboard(ctx, base);
}
#else
// Called through the particle batch's record/replay wrapper.
void VerifyBillboardBuilder(X86Context &ctx, uint8_t *base) {
    const uint32_t e = ctx.esp;
    Ranges ranges;
    ranges.Add(e - 0x100, 0x120);
    ranges.Add(0x53CAE0u + (X86_MEM32(e + 0xC) << 5), 0x80);
    static std::atomic<uint64_t> calls{0}, bad{0};
    VerifyAll("sub_0015D320", ctx, base, Billboard, __imp__sub_0015D320, ranges, calls, bad, nullptr);
}
#endif
