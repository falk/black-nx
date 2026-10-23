// Native replacements of the particle renderer's recompiled loop and its rotating quad builder
//
// Each one leaves the guest exactly as the original does: the same registers (x87 included), the same stack bytes and
// the same vertices. Build with BLACK_VERIFY_HOT to check them against the originals (see the end of the file).
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

#include "game/hot_colour.h"
#include "game/hot_trig.h"

using namespace hot;

// NaN results: when both operands of an addps, addss, mulps or mulss are NaN, the x86 result is the first operand's
// (quieted). The ordered helpers of hot_ordered.h (VAdd, VMul, SAdd, SMul, AddPair) keep the original's operand order
// wherever both operands can carry a NaN from guest memory; plain + and * remain where one operand is a constant of
// .rdata ([0x295C74] 1.0, [0x295CB4] 255.0: never NaN) or a copy of the other.

namespace {

inline f4 GetXmm(const X86Xmm& x) {
    f4 v;
    std::memcpy(&v, &x, 16);
    return v;
}
inline f4 Zero() { return f4{0, 0, 0, 0}; }

// mulps without the microcode assist a subnormal operand costs (about 100 ns on the host; the view-projection rows
// carry subnormal w lanes): the product of two floats is exact in double (48 bits, exponents in range), so rounding it
// once to float gives the same result as the float multiply, subnormal, infinite and overflowing results included.
// NaNs: widening keeps the payload (quieted, as mulps quiets), mulpd returns its first operand's NaN when both are, and
// narrowing drops only the zero bits widening added. mulpd is written as asm: the compiler cannot fold it back into a
// float multiply nor swap its operands.
// AArch64 multiplies subnormals in hardware at full speed (FPCR.FZ clear), so the float multiply is used there.
inline f4 MulExact(f4 a, f4 b) {
#if defined(__aarch64__)
    return VMul(a, b);
#else
    typedef double d2 __attribute__((vector_size(16)));
    typedef float f2 __attribute__((vector_size(8)));
    d2 lo = __builtin_convertvector((f2{a[0], a[1]}), d2), hi = __builtin_convertvector((f2{a[2], a[3]}), d2);
    const d2 blo = __builtin_convertvector((f2{b[0], b[1]}), d2), bhi = __builtin_convertvector((f2{b[2], b[3]}), d2);
    __asm__("mulpd %1, %0" : "+x"(lo) : "x"(blo));
    __asm__("mulpd %1, %0" : "+x"(hi) : "x"(bhi));
    const f2 l = __builtin_convertvector(lo, f2), h = __builtin_convertvector(hi, f2);
    return f4{l[0], l[1], h[0], h[1]};
#endif
}

constexpr uint32_t kOne = 0x295C74;        // 1.0f
constexpr uint32_t kVertices = 0x53CAE0;   // the particle vertex buffer: 0x20 bytes a vertex, 4 a particle
constexpr uint32_t kBuilders = 0x289DF0;   // quad builder of each particle type, indexed by emitter[0x64]
constexpr uint32_t kRotatingQuad = 0x15BF30;
constexpr uint32_t kBillboard = 0x15D320;

// paths of sub_0015B380, counted in the verify build
enum Path {
    kBatch, kEmpty, kChunk, kAgedOut, kDepthCulled, kSideCulled, kTooSmall, kFaint, kClamped, kEarlyKey, kLateKey,
    kBuilt, kBuiltRotating, kFlush, kPaths
};
#ifdef BLACK_VERIFY_HOT
std::atomic<uint64_t> g_paths[kPaths];
inline void Count(Path p) { ++g_paths[p]; }
#else
inline void Count(Path) {}
#endif

}  // namespace

#ifdef BLACK_VERIFY_HOT
X86_EXTERN_FUNC(sub_0015BF30);  // the verify wrapper below
#endif
X86_EXTERN_FUNC(sub_0002AC00);
X86_EXTERN_FUNC(sub_0015D320);

// sub_0015BF30: quad builder of particle type 6, a square turned about the view axis
//   stack  view, particle, vertex index, age, &colour, &screen position, &size (the caller pops them)
//   angle  (v[0x9C] lo^2 + v[0x94] hi + v[0x90] lo + v[0xA0]) * particle[0x1C] + [0x2F81B0], lo and hi the min and
//          max of age and v[0x98]; its sine and cosine from x87 fsin and fcos (the recompiler's std::sin and std::cos
//          of the float widened to double; the cosine stays in the popped x87 slot)
//   out    4 vertices at 0x53CAE0 + index * 0x20: x, y, z = screen position + size * a corner turned by the angle,
//          [0x295C74], then the colour as a8r8g8b8 (truncated) of colour * lerp(v[0x10], v[0x20], particle[0x20] +
//          particle[0x24] * (v[0x30] * corner) . v[0x00]) * [0x295CB4]. The 12 bytes after each are left alone.
// The corners are built in one 16-byte slot whose w is never written: it is read from there.
static void RotatingQuad(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp, ebp = e - 4;
    const uint32_t s = (ebp & ~15u) - 0xD4 - 12;  // esp after the alignment, sub esp, 0xD4 and three pushes
    auto mf = [&](uint32_t off) -> x86_f32u& { return X86_MEMF32(s + off); };
    auto m32 = [&](uint32_t off) -> x86_u32u& { return X86_MEM32(s + off); };
    auto ld = [&](uint32_t off) { return Load4(base, s + off); };
    auto st = [&](uint32_t off, f4 v) { Store4(base, s + off, v); };

    X86_MEM32(ebp) = ctx.ebp;
    const uint32_t view = X86_MEM32(ebp + 8), particle = X86_MEM32(ebp + 0xC);
    f4 x0 = Scalar(F32(base, view + 0x98));
    f4 x2 = Scalar(F32(base, ebp + 0x14));  // age
    f4 x1 = Scalar(F32(base, particle + 0x1C));
    m32(8) = ctx.ebx;
    m32(4) = ctx.esi;
    m32(0) = ctx.edi;
    mf(0x1C) = x0[0];
    mf(0x3C) = x2[0];
    mf(0x18) = sse_min(x2[0], x0[0]);  // lo
    m32(0x3C) = X86_MEM32(view + 0x98);
    mf(0x1C) = x2[0];
    mf(0x14) = sse_max(x2[0], F32(base, view + 0x98));  // hi
    x0 = Scalar(mf(0x18));
    x2 = Scalar(F32(base, view + 0x9C));
    f4 x3 = Scalar(SMul(F32(base, view + 0x94), mf(0x14)));
    x2[0] = SMul(x2[0], x0[0]);
    x2[0] = SMul(x2[0], x0[0]);
    x2[0] = SAdd(x2[0], x3[0]);
    x3 = Scalar(SMul(F32(base, view + 0x90), x0[0]));
    x2[0] = SAdd(x2[0], x3[0]);
    x2[0] = SAdd(x2[0], F32(base, view + 0xA0));
    x2[0] = SMul(x2[0], x1[0]);
    x2[0] = SAdd(x2[0], F32(base, 0x2F81B0));  // the angle
    mf(0x1C) = x2[0];

    // fld, fsin, fstp, fld, fcos, fstp: the slot below the top holds the cosine afterwards, C2 cleared
    const double angle = double(mf(0x1C));
    const SinCos rotation = ParticleSinCos(angle);
    mf(0x14) = float(rotation.sine);
    const double cosine = rotation.cosine;
    mf(0x18) = float(cosine);
    ctx.st[(ctx.top - 1) & 7] = cosine;
    ctx.fsw &= ~0x400;

    x1 = Scalar(mf(0x18));  // cos
    x2 = Zero();
    f4 x5 = Scalar(mf(0x14));  // sin
    x3 = Scalar(F32(base, kOne));
    const uint32_t size = X86_MEM32(ebp + 0x20);
    mf(0x24) = x1[0];
    mf(0x28) = x3[0];
    x0 = Lane0(x2, x2[0] - x5[0]);  // -sin
    mf(0x20) = x0[0];
    f4 x4 = ld(0x20);  // (-sin, cos, 1, w)
    mf(0x20) = x1[0];
    mf(0x24) = x5[0];
    mf(0x28) = x3[0];
    st(0xA0, x4);
    x4 = ld(0x20);  // (cos, sin, 1, w)
    st(0xB0, x4);
    mf(0x20) = x5[0];
    mf(0x28) = x3[0];
    x4 = Lane0(x2, x2[0] - x1[0]);  // -cos
    mf(0x24) = x4[0];
    f4 x6 = ld(0x20);  // (sin, -cos, 1, w)
    mf(0x24) = x0[0];
    mf(0x20) = x4[0];
    mf(0x28) = x3[0];
    x4 = ld(0x20);  // (-cos, -sin, 1, w)
    mf(0x20) = x0[0];
    x0 = Load4(base, size);
    mf(0x24) = x1[0];
    mf(0x28) = x2[0];
    st(0xD0, x4);
    x4 = ld(0x20);  // (-sin, cos, 0, w)
    st(0xC0, x6);
    x6 = x0;
    mf(0x20) = x1[0];
    mf(0x24) = x5[0];
    mf(0x28) = x2[0];
    x1 = ld(0x20);  // (cos, sin, 0, w)
    x6 = VMul(x6, x4);
    x0 = VMul(x0, x1);
    st(0x40, x6);  // corner 0 = size * (-sin, cos, 0, w)
    st(0x50, x0);  // corner 1 = size * (cos, sin, 0, w)
    // corners 2 and 3: the first two negated (xorps with the sign bit)
    const f4 sign = Splat(-0.0f);
    typedef uint32_t u4 __attribute__((vector_size(16)));
    x0 = f4(u4(sign) ^ u4(ld(0x40)));
    st(0x20, x0);
    st(0x60, x0);
    x0 = f4(u4(sign) ^ u4(ld(0x50)));
    st(0x20, x0);
    st(0x70, x0);
    // the slots at 0x14 and 0x18 end with the colour scale and the second end of the colour lerp
    m32(0x14) = X86_MEM32(0x295CB4);
    m32(0x18) = view + 0x20;
    m32(0x3C) = view + 0x10;
    x0 = Scalar(F32(base, 0x295CB4));
    x5 = Splat(x0[0]);

    uint32_t eax = view, ecx = particle, edx = (X86_MEM32(ebp + 0x10) << 5) + kVertices + 8;
    for (uint32_t i = 0; i < 0x40; i += 0x10) {
        if (i) {
            eax = X86_MEM32(ebp + 8);
            ecx = X86_MEM32(ebp + 0xC);
        }
        x1 = VAdd(ld(0x40 + i), Load4(base, X86_MEM32(ebp + 0x1C)));  // + the screen position
        x0 = Load4(base, eax + 0x30);
        st(0x40 + i, x1);
        x1 = VMul(ld(0xA0 + i), x0);
        st(0xA0 + i, x1);
        x1 = Load4(base, eax);
        x0 = VMul(ld(0xA0 + i), x1);
        x1 = Shuf<0x39>(x0, x0);
        x0[0] = AddPair(x0);  // addss xmm0, xmm1: lane 0 + lane 1
        x4 = Shuf<0x39>(x1, x1);
        x0[0] = SAdd(x0[0], x4[0]);
        mf(0x1C) = x2[0];
        mf(0x1C) = x0[0];
        x0 = Scalar(SMul(F32(base, ecx + 0x24), mf(0x1C)));
        x0[0] = SAdd(x0[0], F32(base, ecx + 0x20));
        x4 = Load4(base, X86_MEM32(ebp + 0x18));  // the colour
        mf(0x14) = x0[0];
        const uint32_t lo = m32(0x3C), hi = m32(0x18);
        x0 = Load4(base, hi) - Load4(base, lo);
        x1 = Splat(mf(0x14));
        x0 = VMul(x0, x1);
        x0 = VAdd(x0, Load4(base, lo));
        st(0x90, x0);
        x4 = VMul(x4, x0);
        x4 = x4 * x5;  // x5: [0x295CB4] in every lane
        st(0x20, x4);
        m32(0x84) = m32(0x2C);
        m32(0x80) = m32(0x20);
        m32(0x88) = m32(0x24);
        m32(0x8C) = m32(0x28);
        x0 = Scalar(mf(0x28));
        X86_MEM32(edx + 8) = PackColour(x4);
        X86_MEM32(edx - 8) = m32(0x40 + i);
        X86_MEM32(edx - 4) = m32(0x44 + i);
        X86_MEM32(edx) = m32(0x48 + i);
        X86_MEMF32(edx + 4) = x3[0];
        eax = m32(0x44 + i);
        ecx = m32(0x48 + i);
        edx += 0x20;
    }

    // ebx, esi, edi and ebp come back as they were
    ctx.eax = eax;
    ctx.ecx = ecx;
    ctx.edx = edx;
    PutXmm(ctx.xmm[0], x0);
    PutXmm(ctx.xmm[1], x1);
    PutXmm(ctx.xmm[2], x2);
    PutXmm(ctx.xmm[3], x3);
    PutXmm(ctx.xmm[4], x4);
    PutXmm(ctx.xmm[5], x5);
    PutXmm(ctx.xmm[6], x6);
    ctx.esp = e + 4;
}

// sub_0015B380(batch, emitter, view): draws one batch of an emitter's particles
//   batch     +0x08 particle count, +0x0C the particles: 0x30 bytes each, p0 at +0x00, velocity at +0x10, birth time at
//             +0x0C, size and alpha scales at +0x28 and +0x2C
//   emitter   +0x00/+0x10/+0x20 colour keys, +0xA4/+0xA8/+0xAC size keys, +0x64 particle type, +0x70 lifetime, +0x74
//             the middle key's time (of 1), +0x78 gravity, +0x7C..+0x8C the growth curve, +0xD0 least alpha
//   view      +0x140 the time, +0xF0 the view-projection rows, +0x40/+0x44 near and far, +0x48/+0x4C fades,
//             +0x50/+0x54 key rates, +0x60/+0x64 the frustum's x and y slopes, +0x68/+0x6C most size, +0x70 least
//             size, +0x80 the size vector
// Per particle, age = time - birth, kept when 0 < age < lifetime:
//   lo, hi    min and max of age and emitter[0x84]; growth = lo^2 e[0x88] + e[0x80] hi + e[0x7C] lo - e[0x8C]
//   position  p0 + velocity * growth + (0, gravity, 0, w) * age^2, w read from a slot nobody writes; then times the
//             view-projection, refused outside near..far and the frustum slopes
//   keys      the pair either side of age / lifetime, t from view[0x50] or view[0x54]
//   size      view[0x80] * size key lerp * particle[0x28] / w; refused below view[0x70], clamped to view[0x68..0x6C]
//   colour    colour key lerp (sub_00011DD0, inlined with its frame); alpha = its w * min(fades, 1) * particle[0x2C],
//             refused at or below emitter[0xD0]
// then the type's builder (table 0x289DF0) writes 4 vertices, and the vertices go to sub_0002AC00(8, count,
// 0x53CAE0, 0x20) at the end of each run of at most 0x400 particles. A batch over 0x400 never ends (the second run
// compares its start with its length), as in the original.
static void ParticleBatch(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp, ebp = e - 4;
    const uint32_t s = (ebp & ~15u) - 0x114 - 12;  // esp after the alignment, sub esp, 0x114 and three pushes
    auto mf = [&](uint32_t off) -> x86_f32u& { return X86_MEMF32(s + off); };
    auto m32 = [&](uint32_t off) -> x86_u32u& { return X86_MEM32(s + off); };
    auto ld = [&](uint32_t off) { return Load4(base, s + off); };
    auto st = [&](uint32_t off, f4 v) { Store4(base, s + off, v); };
    auto f = [&](uint32_t va) { return X86_MEMF32(va); };

    uint32_t eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx, ebx, esi, edi;
    f4 x0 = GetXmm(ctx.xmm[0]), x1 = GetXmm(ctx.xmm[1]), x2 = GetXmm(ctx.xmm[2]), x3, x4 = GetXmm(ctx.xmm[4]),
       x5 = GetXmm(ctx.xmm[5]), x6 = GetXmm(ctx.xmm[6]), x7;

    // registers to the guest for a call, and back
    auto out = [&](uint32_t esp) {
        ctx.eax = eax, ctx.ecx = ecx, ctx.edx = edx, ctx.ebx = ebx, ctx.esi = esi, ctx.edi = edi;
        ctx.ebp = ebp, ctx.esp = esp;
        PutXmm(ctx.xmm[0], x0), PutXmm(ctx.xmm[1], x1), PutXmm(ctx.xmm[2], x2), PutXmm(ctx.xmm[3], x3);
        PutXmm(ctx.xmm[4], x4), PutXmm(ctx.xmm[5], x5), PutXmm(ctx.xmm[6], x6), PutXmm(ctx.xmm[7], x7);
    };
    auto in = [&]() {
        eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx, ebx = ctx.ebx, esi = ctx.esi, edi = ctx.edi;
        x0 = GetXmm(ctx.xmm[0]), x1 = GetXmm(ctx.xmm[1]), x2 = GetXmm(ctx.xmm[2]), x3 = GetXmm(ctx.xmm[3]);
        x4 = GetXmm(ctx.xmm[4]), x5 = GetXmm(ctx.xmm[5]), x6 = GetXmm(ctx.xmm[6]), x7 = GetXmm(ctx.xmm[7]);
    };

    X86_MEM32(ebp) = ctx.ebp;
    edx = X86_MEM32(ebp + 8);
    x7 = Scalar(f(kOne));
    x3 = Zero();
    m32(8) = ctx.ebx;
    ebx = X86_MEM32(ebp + 0x10);
    m32(4) = ctx.esi;
    esi = X86_MEM32(ebp + 0xC);
    eax = X86_MEM32(esi + 0x64);
    x0 = Scalar(f(esi + 0x78));
    ecx = X86_MEM32(eax * 4 + kBuilders);
    eax = X86_MEM32(edx + 8);
    m32(0) = ctx.edi;
    mf(0x104) = x0[0];  // (0, gravity, 0, w) at 0x100
    edi = 0;
    x0 = Lane0(x7, x7[0] / f(esi + 0x70));
    mf(0x100) = x3[0];
    mf(0x108) = x3[0];
    mf(0xC8) = x0[0];  // 1 / lifetime
    m32(0xB0) = ecx;   // the builder
    m32(0x28) = 0;     // vertices so far
    Count(kBatch);
    if (eax == 0) Count(kEmpty);
    else {
        m32(0xEC) = 0x400;
        for (bool first = true;; first = false) {
            if (!first) x3 = Zero();
            eax -= edi;
            m32(0x2C) = eax;
            X86_MEM32(s - 4) = eax;  // push eax, pop eax
            m32(0xC0) = m32(0x2C) > m32(0xEC) ? m32(0xEC) : m32(0x2C);
            eax = m32(0xC0);
            if (edi >= eax) Count(kChunk);
            else {
                ecx = edi * 0x30;
                eax -= edi;
                edi += eax;
                m32(0x2C) = eax;  // particles left in this run
                m32(0xB8) = edi;
                for (bool firstp = true;; firstp = false) {
                    if (!firstp) x3 = Zero();
                    int exit = 0;  // 0: on to the next particle, 1: reload esi, 2: reload xmm7, ecx and esi
                    do {
                        eax = X86_MEM32(X86_MEM32(ebp + 8) + 0xC);
                        x1 = Scalar(f(ebx + 0x140));
                        x1[0] = x1[0] - f(eax + ecx + 0xC);  // age
                        eax += ecx;
                        ecx += 0x30;
                        const float life = f(esi + 0x70);
                        m32(0x98) = eax;
                        m32(0x6C) = ecx;
                        mf(0xA8) = x1[0];
                        if (Jnb(x1[0], life) || Jnb(x3[0], x1[0])) {
                            Count(kAgedOut);
                            break;
                        }
                        // kept in registers from here to the frustum test: the slots are only stored
                        const f4 p0 = Load4(base, eax);
                        st(0x10, p0);
                        const float e84 = f(esi + 0x84);
                        x2 = x1;
                        x2[0] = SMul(x2[0], mf(0xC8));  // age / lifetime
                        const float lo = sse_min(x1[0], e84), hi = sse_max(x1[0], e84);
                        mf(0xD0) = e84;
                        mf(0xD8) = x1[0];
                        mf(0xCC) = lo;
                        mf(0xE0) = e84;
                        mf(0x9C) = x1[0];
                        mf(0xA4) = hi;
                        x0 = Scalar(lo);
                        x5 = Scalar(SMul(f(esi + 0x80), hi));
                        x4 = x0;
                        x4[0] = x4[0] * x0[0];
                        x4[0] = SMul(x4[0], f(esi + 0x88));
                        x4[0] = SAdd(x4[0], x5[0]);
                        x5 = Scalar(SMul(f(esi + 0x7C), x0[0]));
                        const float age2 = x1[0] * x1[0];
                        x1 = ld(0x100);
                        mf(0xE4) = age2;
                        x0 = Splat(age2);
                        x4[0] = SAdd(x4[0], x5[0]);
                        x4[0] = x4[0] - f(esi + 0x8C);  // growth
                        x1 = MulExact(x1, x0);
                        mf(0xAC) = x4[0];
                        x4 = MulExact(Load4(base, eax + 0x10), Splat(x4[0]));
                        x0 = VAdd(p0, x4);
                        x0 = VAdd(x0, x1);  // the position
                        m32(0xD4) = s + 0x10;
                        m32(0xB4) = ebx + 0xF0;
                        esi = ebx + 0xF0;
                        edi = s + 0x10;
                        const f4 pos = x0;
                        x0 = MulExact(Splat(pos[0]), Load4(base, esi));
                        x0 = VAdd(x0, Load4(base, esi + 0x30));
                        x1 = MulExact(Splat(pos[1]), Load4(base, esi + 0x10));
                        x0 = VAdd(x0, x1);
                        x1 = MulExact(Splat(pos[2]), Load4(base, esi + 0x20));
                        x0 = VAdd(x0, x1);
                        const f4 clip = x0;
                        st(0xF0, clip);
                        st(0x10, clip);
                        edx = Bits(clip[3]);
                        x0 = Scalar(f(ebx + 0x40));
                        x5 = Scalar(clip[2]);  // w
                        exit = 1;
                        if (Jnb(x0[0], x5[0]) || Jnb(x5[0], f(ebx + 0x44))) {
                            Count(kDepthCulled);
                            break;
                        }
                        x0 = Scalar(f(ebx + 0x60));
                        x4 = Scalar(clip[0]);
                        x0[0] = SMul(x0[0], x5[0]);
                        x1 = Lane0(x3, x3[0] - x0[0]);
                        if (Jnb(x1[0], x4[0])) {
                            Count(kSideCulled);
                            break;
                        }
                        x0 = Scalar(f(ebx + 0x64));
                        x1 = x3;
                        x3 = Scalar(clip[1]);
                        x0[0] = SMul(x0[0], x5[0]);
                        x1[0] = x1[0] - x0[0];
                        if (Jnb(x1[0], x3[0])) {
                            Count(kSideCulled);
                            break;
                        }
                        x0 = Scalar(f(ebx + 0x60));
                        x0[0] = x0[0] + x7[0];  // x7: [0x295C74]
                        x0[0] = SMul(x0[0], x5[0]);
                        if (Jnb(x4[0], x0[0])) {
                            Count(kSideCulled);
                            break;
                        }
                        x0 = Scalar(f(ebx + 0x64));
                        x0[0] = x0[0] + x7[0];  // x7: [0x295C74]
                        x0[0] = SMul(x0[0], x5[0]);
                        if (Jnb(x3[0], x0[0])) {
                            Count(kSideCulled);
                            break;
                        }

                        // the colour keys either side of the age
                        esi = X86_MEM32(ebp + 0xC);
                        x0 = Scalar(f(esi + 0x74));
                        if (Jnbe(x0[0], x2[0])) {
                            Count(kEarlyKey);
                            x1 = Load4(base, esi);
                            x6 = Scalar(f(ebx + 0x50));
                            x0 = Scalar(f(esi + 0xA8));
                            x6[0] = SMul(x6[0], x2[0]);
                            x2 = Scalar(f(esi + 0xA4));
                            st(0x80, x1);
                            x1 = Load4(base, esi + 0x10);
                        } else {
                            Count(kLateKey);
                            x2[0] = x2[0] - f(esi + 0x74);
                            x2[0] = SMul(x2[0], f(ebx + 0x54));
                            x1 = Load4(base, esi + 0x10);
                            x0 = Scalar(f(esi + 0xAC));
                            x6 = x2;
                            x2 = Scalar(f(esi + 0xA8));
                            st(0x80, x1);
                            x1 = Load4(base, esi + 0x20);
                        }
                        st(0x70, x1);
                        x0[0] = x0[0] - x2[0];
                        x0[0] = SMul(x0[0], x6[0]);
                        x1 = Lane0(x7, x7[0] / x5[0]);  // 1 / w
                        x0[0] = SAdd(x0[0], x2[0]);
                        x4[0] = SMul(x4[0], x1[0]);
                        x3[0] = SMul(x3[0], x1[0]);
                        mf(0x10) = x4[0];
                        mf(0x14) = x3[0];
                        x0[0] = SMul(x0[0], f(eax + 0x28));
                        x0[0] = SMul(x0[0], x1[0]);
                        mf(0x94) = x0[0];
                        eax = s + 0x94;
                        mf(0xA0) = x1[0];
                        x1 = VMul(Load4(base, ebx + 0x80), Splat(mf(0x94)));
                        x0 = Scalar(f(ebx + 0x70));
                        st(0x30, x1);  // the size
                        mf(0x4C) = x6[0];
                        if (Jnbe(x0[0], mf(0x30))) {
                            Count(kTooSmall);
                            break;
                        }

                        // sub_00011DD0(&key0, &key1, t) into the slot at 0x110, its frame below this one
                        ecx = m32(0x4C);
                        X86_MEM32(s - 4) = ecx;
                        X86_MEM32(s - 8) = s + 0x70;
                        X86_MEM32(s - 12) = s + 0x80;
                        X86_MEM32(s - 16) = 0x15B7B8;
                        X86_MEM32(s - 20) = ebp;
                        X86_MEM32(s - 60) = esi;
                        X86_MEM32(s - 64) = edi;
                        x0 = ld(0x70) - ld(0x80);
                        x1 = Splat(F32(base, s - 4));
                        x0 = VMul(x0, x1);
                        x0 = VAdd(x0, ld(0x80));
                        Store4(base, s - 48, x0);
                        st(0x110, x0);
                        edx = X86_MEM32(s - 36);
                        ecx = s + 0x110;

                        x1 = Scalar(f(ebx + 0x44));
                        st(0x50, x0);  // the colour
                        mf(0xBC) = f(kOne);
                        x0 = Scalar(mf(0x18));
                        x1[0] = x1[0] - x0[0];
                        x0[0] = x0[0] - f(ebx + 0x40);
                        x1[0] = SMul(x1[0], f(ebx + 0x4C));
                        x0[0] = SMul(x0[0], f(ebx + 0x48));
                        mf(0xDC) = x1[0];
                        mf(0xC4) = x0[0];
                        x0[0] = sse_min(sse_min(mf(0xC4), mf(0xDC)), mf(0xBC));
                        mf(0xE8) = x0[0];
                        eax = m32(0x98);
                        x0 = Scalar(SMul(mf(0x5C), mf(0xE8)));  // the colour's w
                        x0[0] = SMul(x0[0], f(eax + 0x2C));  // alpha
                        x1 = Scalar(f(esi + 0xD0));
                        exit = 2;
                        if (Jnb(x1[0], x0[0])) {
                            Count(kFaint);
                            break;
                        }
                        x1 = Scalar(mf(0x30));
                        if (Jnbe(x1[0], f(ebx + 0x68))) {
                            Count(kClamped);
                            m32(0x30) = X86_MEM32(ebx + 0x68);
                            m32(0x34) = X86_MEM32(ebx + 0x6C);
                            x1 = Zero();
                            mf(0x38) = 0;
                        }

                        // builder(view, particle, vertex index, age, &colour, &screen position, &size)
                        esi = m32(0x28);
                        x1 = Scalar(mf(0xA0));
                        ecx = s + 0x50;
                        edx = m32(0xA8);
                        X86_MEM32(s - 4) = s + 0x30;
                        X86_MEM32(s - 8) = s + 0x10;
                        X86_MEM32(s - 12) = ecx;
                        X86_MEM32(s - 16) = edx;
                        X86_MEM32(s - 20) = esi;
                        X86_MEM32(s - 24) = eax;
                        X86_MEM32(s - 28) = ebx;
                        mf(0x18) = x1[0];
                        mf(0x5C) = x0[0];
                        const uint32_t target = m32(0xB0);
                        X86_MEM32(s - 32) = 0x15B8B5;
#ifdef BLACK_VERIFY_HOT
                        out(s - 32);
#else
                        if (target == kRotatingQuad || target == kBillboard) {
                            // These leaf builders consume the guest arguments and
                            // saved GPRs, not incoming XMM values. Keep the live
                            // vectors in locals instead of spilling all eight.
                            ctx.ebx = ebx; ctx.esi = esi; ctx.edi = edi;
                            ctx.ebp = ebp; ctx.esp = s - 32;
                        } else {
                            out(s - 32);
                        }
#endif
                        Count(kBuilt);
                        if (target == kRotatingQuad) {
                            Count(kBuiltRotating);
#ifdef BLACK_VERIFY_HOT
                            sub_0015BF30(ctx, base);
#else
                            RotatingQuad(ctx, base);
#endif
#ifdef BLACK_VERIFY_HOT
                            in();
#else
                            eax = ctx.eax; ecx = ctx.ecx; edx = ctx.edx;
                            x0 = GetXmm(ctx.xmm[0]); x1 = GetXmm(ctx.xmm[1]);
                            x2 = GetXmm(ctx.xmm[2]); x3 = GetXmm(ctx.xmm[3]);
                            x4 = GetXmm(ctx.xmm[4]); x5 = GetXmm(ctx.xmm[5]);
                            x6 = GetXmm(ctx.xmm[6]);
#endif
                        } else if (target == kBillboard) {
                            // This builder preserves EBX/ESI/EDI and XMM4..7 in
                            // both the native and generated paths. Keep those
                            // locals live instead of round-tripping all state.
                            ctx.last_icall = target;
                            sub_0015D320(ctx, base);
                            eax = ctx.eax; ecx = ctx.ecx; edx = ctx.edx;
                            x0 = GetXmm(ctx.xmm[0]); x1 = GetXmm(ctx.xmm[1]);
                            x2 = GetXmm(ctx.xmm[2]); x3 = GetXmm(ctx.xmm[3]);
                        } else {
                            x86_icall_fast(ctx, base, target);
                            in();
                        }
                        esi += 4;
                        m32(0x28) = esi;
                    } while (false);
                    if (exit >= 2) {
                        x7 = Scalar(f(kOne));
                        ecx = m32(0x6C);
                    }
                    if (exit >= 1) esi = X86_MEM32(ebp + 0xC);
                    if (--m32(0x2C) == 0) break;
                }
                edi = m32(0xB8);
            }

            eax = m32(0x28);
            if (eax != 0) {
                Count(kFlush);
                X86_MEM32(s - 4) = 0x20;
                X86_MEM32(s - 8) = kVertices;
                X86_MEM32(s - 12) = eax;
                X86_MEM32(s - 16) = 8;
                X86_MEM32(s - 20) = 0x15B8F6;
                out(s - 20);
                sub_0002AC00(ctx, base);
                in();
                x7 = Scalar(f(kOne));
                m32(0x28) = 0;
            }
            eax = X86_MEM32(X86_MEM32(ebp + 8) + 8);
            if (edi >= eax) break;
        }
    }

    ctx.eax = eax;
    ctx.ecx = ecx;
    ctx.edx = edx;
    ctx.edi = m32(0);
    ctx.esi = m32(4);
    ctx.ebx = m32(8);
    ctx.ebp = X86_MEM32(ebp);
    PutXmm(ctx.xmm[0], x0);
    PutXmm(ctx.xmm[1], x1);
    PutXmm(ctx.xmm[2], x2);
    PutXmm(ctx.xmm[3], x3);
    PutXmm(ctx.xmm[4], x4);
    PutXmm(ctx.xmm[5], x5);
    PutXmm(ctx.xmm[6], x6);
    PutXmm(ctx.xmm[7], x7);
    ctx.esp = e + 0x10;  // ret 0x0C
}

#ifndef BLACK_VERIFY_HOT

X86_FUNC(sub_0015B380) { ParticleBatch(ctx, base); }
X86_FUNC(sub_0015BF30) { RotatingQuad(ctx, base); }

#else

// sub_0015BF30 runs twice under Verify (it only writes its frame and its 4 vertices), x87 compared as well.
//
// sub_0015B380 calls guest code with effects beyond what can be restored (the builders' vertices, the draw in
// sub_0002AC00), so it is checked by record and replay instead: the native one runs for real with every guest call it
// makes recorded (the registers, x87 and the stack window before the call, and after it the registers, the window and
// a builder's 4 vertices). Then the window and registers are put back and the original runs with its guest calls
// replayed: each must come in the same order to the same target with the same registers and the same stack window
// (which holds the arguments and the colour, position and size records they point to), and gets the recorded result
// instead of running again. At the end the registers and the window must match. The window runs from 0x400 below the
// frame (the callees' frames) to the arguments. sub_00011DD0 is inlined in the native one and runs for real in the
// original; its frame is inside the window.

#include "game/hot_verify.h"

X86_EXTERN_FUNC(__imp__sub_0015B380);
X86_EXTERN_FUNC(__imp__sub_0015BF30);

namespace {

bool SameRegs(const X86Context& a, const X86Context& b) {
    return std::memcmp(&a, &b, offsetof(X86Context, cf)) == 0 && std::memcmp(&a.xmm, &b.xmm, sizeof a.xmm) == 0 &&
           a.top == b.top && a.fcw == b.fcw && a.fsw == b.fsw && std::memcmp(a.st, b.st, sizeof a.st) == 0;
}

void Report(const char* name, uint64_t n, uint64_t bad, const char* extra = "") {
    if ((n & (n - 1)) == 0 && n >= 1024)
        std::fprintf(stderr, "verify %s: %llu calls, %llu mismatches%s\n", name, (unsigned long long)n,
                     (unsigned long long)bad, extra);
}

std::atomic<uint64_t> g_quad_calls{0}, g_quad_bad{0}, g_quad_x87_bad{0};
thread_local X86Context t_quad_mine;

void RotatingQuadKept(X86Context& ctx, uint8_t* base) {
    RotatingQuad(ctx, base);
    t_quad_mine = ctx;
}

void VerifyRotatingQuad(X86Context& ctx, uint8_t* base) {
    const uint32_t e = ctx.esp;
    const Region regions[] = {{e - 0x100, 0x100}, {kVertices + X86_MEM32(e + 0xC) * 0x20, 0x80}};
    Verify("sub_0015BF30", ctx, base, RotatingQuadKept, __imp__sub_0015BF30, regions, g_quad_calls, g_quad_bad);
    if (!SameRegs(t_quad_mine, ctx) && ++g_quad_x87_bad <= 8)
        std::fprintf(stderr, "verify sub_0015BF30: x87 mismatch at call %llu\n",
                     (unsigned long long)g_quad_calls.load());
}

constexpr uint32_t kWindow = 0x600;  // 0x400 below the frame, the frame (at most 0x133) and the arguments

struct GuestCallRecord {
    uint32_t target, vertices;
    X86Context pre, post;
    uint8_t pre_window[kWindow], post_window[kWindow], vertex_bytes[0x80];
};

struct Trace {
    enum { kOff, kRecord, kReplay } mode = kOff;
    uint32_t lo = 0, size = 0;
    std::vector<GuestCallRecord> calls;
    size_t count = 0, next = 0;
    const char* fail = nullptr;
};
thread_local Trace t_trace;

// every guest call sub_0015B380 makes goes through here: run and record, or check and replay
void GuestCall(X86Context& ctx, uint8_t* base, uint32_t target, void (*fn)(X86Context&, uint8_t*), bool builder) {
    Trace& t = t_trace;
    uint8_t* window = X86_PTR(uint8_t, t.lo);
    if (t.mode == Trace::kOff) {
        fn(ctx, base);
    } else if (t.mode == Trace::kRecord) {
        if (t.count == t.calls.size()) t.calls.resize(t.count + 64);
        GuestCallRecord& c = t.calls[t.count++];
        c.target = target;
        c.vertices = builder ? kVertices + X86_MEM32(ctx.esp + 0xC) * 0x20 : 0;
        c.pre = ctx;
        std::memcpy(c.pre_window, window, t.size);
        fn(ctx, base);
        c.post = ctx;
        std::memcpy(c.post_window, window, t.size);
        if (builder) std::memcpy(c.vertex_bytes, X86_PTR(uint8_t, c.vertices), 0x80);
    } else {
        if (t.next == t.count) {
            if (!t.fail) t.fail = "a call more than the native one made";
            ctx.esp += 4;
            return;
        }
        const GuestCallRecord& c = t.calls[t.next++];
        if (t.fail) {
        } else if (c.target != target) {
            t.fail = "a call to another target";
        } else if (!SameRegs(c.pre, ctx)) {
            t.fail = "registers at a call";
        } else if (std::memcmp(c.pre_window, window, t.size) != 0) {
            t.fail = "the stack at a call";
        }
        std::memcpy(&ctx, &c.post, offsetof(X86Context, thread_handle));
        std::memcpy(window, c.post_window, t.size);
        if (c.vertices) std::memcpy(X86_PTR(uint8_t, c.vertices), c.vertex_bytes, 0x80);
    }
}

std::atomic<uint64_t> g_batch_calls{0}, g_batch_bad{0}, g_batch_guest_calls{0};

}  // namespace

X86_FUNC(sub_0015BF30) { GuestCall(ctx, base, kRotatingQuad, VerifyRotatingQuad, true); }

// the other builders of the table at 0x289DF0, and the draw
#define BUILDER(addr)                                                                            \
    X86_EXTERN_FUNC(__imp__sub_##addr);                                                          \
    X86_FUNC(sub_##addr) { GuestCall(ctx, base, 0x##addr, __imp__sub_##addr, true); }
void VerifyBillboardBuilder(X86Context& ctx, uint8_t* base);
X86_FUNC(sub_0015D320) { GuestCall(ctx, base, 0x15D320, VerifyBillboardBuilder, true); }
void VerifyQuadBuilder(X86Context& ctx, uint8_t* base);  // hot_quad.cpp: its native rewrite against the original
X86_FUNC(sub_0015CFF0) { GuestCall(ctx, base, 0x15CFF0, VerifyQuadBuilder, true); }
BUILDER(0015CD10)
BUILDER(0015CA40)
void VerifySpinBuilder(X86Context& ctx, uint8_t* base);  // hot_quad.cpp: its native rewrite against the original
X86_FUNC(sub_0015C620) { GuestCall(ctx, base, 0x15C620, VerifySpinBuilder, true); }
BUILDER(0015C320)
BUILDER(0015BC20)
#undef BUILDER
X86_EXTERN_FUNC(__imp__sub_0002AC00);
X86_FUNC(sub_0002AC00) { GuestCall(ctx, base, 0x2AC00, __imp__sub_0002AC00, false); }

namespace {

// one record-and-replay check of sub_0015B380 from the current state; the name of what differed, or null
const char* CheckBatch(X86Context& ctx, uint8_t* base) {
    Trace& t = t_trace;
    const uint32_t e = ctx.esp, s = ((e - 4) & ~15u) - 0x120;
    t.lo = s - 0x400;
    t.size = e + 0x10 - t.lo;
    uint8_t* window = X86_PTR(uint8_t, t.lo);
    static thread_local uint8_t before[kWindow], after[kWindow];
    std::memcpy(before, window, t.size);
    const X86Context start = ctx;

    t.mode = Trace::kRecord;
    t.count = 0;
    ParticleBatch(ctx, base);
    const X86Context mine = ctx;
    std::memcpy(after, window, t.size);

    std::memcpy(window, before, t.size);
    ctx = start;
    t.mode = Trace::kReplay;
    t.next = 0;
    t.fail = nullptr;
    __imp__sub_0015B380(ctx, base);
    t.mode = Trace::kOff;

    if (!t.fail && t.next != t.count) t.fail = "fewer calls than the native one made";
    if (!t.fail && !SameRegs(mine, ctx)) t.fail = "registers at the end";
    if (!t.fail && std::memcmp(after, window, t.size) != 0) t.fail = "the stack at the end";
    g_batch_guest_calls += t.count;
    return t.fail;
}

std::atomic<uint64_t> g_synthetic{0};

// sub_0015BF30 on synthetic inputs, once per NaN case of hot_verify.h, in the dead stack 0x800 below the live esp:
// view at t (+0x00, +0x10, +0x20, +0x30 vectors, +0x90..+0xA0 scalars), particle at t + 0x100, colour, screen position
// and size at t + 0x140, 0x150, 0x160, vertex index 0; the corners' w slot (never written by the original) gets a NaN
// too. The stack range and the 4 vertices are compared, then put back.
void RotatingQuadNanCases(X86Context& live, uint8_t* base) {
    const uint32_t t = (live.esp - 0x800) & ~15u, e = t - 0x40, lo = t - 0x200, size = 0x400;
    const uint32_t s = ((e - 4) & ~15u) - 0xE0;
    std::vector<uint8_t> keep(X86_PTR(uint8_t, lo), X86_PTR(uint8_t, lo) + size);
    uint8_t vertices[0x80];
    std::memcpy(vertices, X86_PTR(uint8_t, kVertices), 0x80);
    for (int k = 0; k < kNanCaseCount; k++) {
        X86Context c = live;
        const bool nan = kNanCases[k].lanes != 0;
        PutNanVector(base, t, 0.5f, 0.25f, 0.125f, 1, 0, k);
        PutNanVector(base, t + 0x10, 0.2f, 0.4f, 0.6f, 0.8f, 1, k);
        PutNanVector(base, t + 0x20, 0.9f, 0.7f, 0.5f, 0.3f, 2, k);
        PutNanVector(base, t + 0x30, 1.5f, -0.5f, 2, 1, 3, k);
        PutNanVector(base, t + 0x90, 0.01f, 0.02f, 0.5f, 0.03f, 4, k);  // +0x90, +0x94, +0x98, +0x9C
        PutNanVector(base, t + 0xA0, 0.04f, 0, 0, 0, 5, k);
        PutNanVector(base, t + 0x110, 0.75f, 0.3f, 0.6f, 0, 6, k);  // particle +0x10..: +0x1C, +0x20, +0x24
        PutNanVector(base, t + 0x140, 0.9f, 0.8f, 0.7f, 0.6f, 7, k);
        PutNanVector(base, t + 0x150, 100, 200, 0.5f, 1, 8, k);
        PutNanVector(base, t + 0x160, 8, 8, 0, 0, 9, k);
        X86_MEM32(s + 0x2C) = nan ? NanBits(10, 3, k, kNanCases[k].signaling) : 0;
        X86_MEM32(e) = 0;
        X86_MEM32(e + 4) = t;
        X86_MEM32(e + 8) = t + 0x100;
        X86_MEM32(e + 0xC) = 0;
        X86_MEMF32(e + 0x10) = 0.25f;
        X86_MEM32(e + 0x14) = t + 0x140;
        X86_MEM32(e + 0x18) = t + 0x150;
        X86_MEM32(e + 0x1C) = t + 0x160;
        c.esp = e;
        Ranges r;
        r.Add(lo, size);
        r.Add(kVertices, 0x80);
        std::atomic<uint64_t> calls{0}, bad{0};
        const bool same = VerifyAll("sub_0015BF30", c, base, RotatingQuad, __imp__sub_0015BF30, r, calls, bad, nullptr);
        std::fprintf(stderr, "verify sub_0015BF30 %s: synthetic case %s\n", kNanCases[k].what,
                     same ? "same" : "DIFFERENT");
        std::memcpy(X86_PTR(uint8_t, lo), keep.data(), size);
        std::memcpy(X86_PTR(uint8_t, kVertices), vertices, 0x80);
    }
}

// a range of guest memory kept to be put back
struct Saved {
    uint32_t va;
    std::vector<uint8_t> bytes;
};
Saved Save(uint8_t* base, uint32_t va, uint32_t size) {
    return {va, std::vector<uint8_t>(X86_PTR(uint8_t, va), X86_PTR(uint8_t, va) + size)};
}
void Restore(uint8_t* base, const Saved& s) { std::memcpy(X86_PTR(uint8_t, s.va), s.bytes.data(), s.bytes.size()); }

}  // namespace

X86_FUNC(sub_0015B380) {
    if (t_trace.mode != Trace::kOff) {  // not reentered in practice; run it plainly
        ParticleBatch(ctx, base);
        return;
    }
    static std::atomic<bool> quad_cases{false};
    if (!quad_cases.exchange(true)) RotatingQuadNanCases(ctx, base);
    const char* fail = nullptr;
    // every 0x1000th batch also runs from the same state with every particle too faint (emitter[0xD0] at +inf) and
    // as an empty batch (count 0): paths play does not reach. No guest calls there; inputs and stack put back.
    static thread_local uint64_t seen = 0;
    if ((seen++ & 0xFFF) == 0) {
        const uint32_t e = ctx.esp, lo = (((e - 4) & ~15u) - 0x120) - 0x400, size = e + 0x10 - lo;
        const uint32_t batch = X86_MEM32(e + 4), emitter = X86_MEM32(e + 8);
        static thread_local uint8_t stack[kWindow];
        std::memcpy(stack, X86_PTR(uint8_t, lo), size);
        const X86Context start = ctx;
        const uint32_t least = X86_MEM32(emitter + 0xD0), count = X86_MEM32(batch + 8);
        X86_MEM32(emitter + 0xD0) = 0x7F800000;  // +inf
        fail = CheckBatch(ctx, base);
        X86_MEM32(emitter + 0xD0) = least;
        ctx = start;
        X86_MEM32(batch + 8) = 0;
        if (!fail) fail = CheckBatch(ctx, base);
        X86_MEM32(batch + 8) = count;
        ctx = start;
        std::memcpy(X86_PTR(uint8_t, lo), stack, size);

        // NaNs of distinct bits on both sides of the ordered adds and multiplies. First w lanes alone (they never reach
        // a test, so every particle takes its usual way): each particle's velocity w (+0x1C, also the turn rate the
        // type 6 builder reads; the start's w at +0x0C is the birth time), the view-projection rows and the gravity
        // vector's w (a frame slot the original never writes). Then the growth curve
        // (e[0x7C], e[0x80], e[0x88]), every lane of the colour keys and view[0x80]: NaN positions, colours and sizes
        // pass every test (comiss on unordered), so these particles are built from NaNs.
        const uint32_t view = X86_MEM32(e + 0xC), parts = X86_MEM32(batch + 0xC);
        const uint32_t n = count < 0x400 ? count : 0x400;
        const Saved kept[] = {Save(base, parts, n * 0x30), Save(base, emitter, 0x90), Save(base, view + 0x80, 0x10),
                              Save(base, view + 0xF0, 0x40)};
        for (uint32_t i = 0; i < n; i++) X86_MEM32(parts + i * 0x30 + 0x1C) = NanBits(i, 1, 1, i & 1);
        for (uint32_t r = 0; r < 4; r++) X86_MEM32(view + 0xF0 + r * 0x10 + 0xC) = NanBits(0x40 + r, 3, 1, r == 2);
        X86_MEM32((((e - 4) & ~15u) - 0x120) + 0x10C) = NanBits(0x50, 3, 1, false);
        if (!fail) fail = CheckBatch(ctx, base);
        ctx = start;
        std::memcpy(X86_PTR(uint8_t, lo), stack, size);
        for (uint32_t i = 0; i < 3; i++) X86_MEM32(emitter + 0x7C + 4 * i) = NanBits(0x60 + i, 0, 2, i == 1);
        X86_MEM32(emitter + 0x88) = NanBits(0x63, 0, 2, false);
        for (uint32_t key = 0; key < 3; key++)
            for (uint32_t l = 0; l < 4; l++) X86_MEM32(emitter + key * 0x10 + 4 * l) = NanBits(0x70 + key, l, 2, false);
        for (uint32_t l = 0; l < 4; l++) X86_MEM32(view + 0x80 + 4 * l) = NanBits(0x78, l, 2, l == 3);
        if (!fail) fail = CheckBatch(ctx, base);
        for (const Saved& k : kept) Restore(base, k);
        ctx = start;
        std::memcpy(X86_PTR(uint8_t, lo), stack, size);
        ++g_synthetic;
        if (fail && ++g_batch_bad <= 8) std::fprintf(stderr, "verify sub_0015B380: synthetic mismatch: %s\n", fail);
        fail = nullptr;
    }
    fail = CheckBatch(ctx, base);
    const Trace& t = t_trace;

    const uint64_t n = ++g_batch_calls, guest = g_batch_guest_calls.load();
    if (fail && ++g_batch_bad <= 8)
        std::fprintf(stderr, "verify sub_0015B380: mismatch at call %llu: %s (guest call %zu of %zu)\n",
                     (unsigned long long)n, fail, t.next, t.count);
    if ((n & (n - 1)) == 0 && n >= 1024) {
        char extra[96];
        std::snprintf(extra, sizeof extra, ", %llu guest calls checked, %llu synthetic", (unsigned long long)guest,
                      (unsigned long long)g_synthetic.load());
        Report("sub_0015B380", n, g_batch_bad.load(), extra);
        static const char* const names[kPaths] = {
            "batches", "empty", "run start past its end", "aged out", "depth culled", "side culled",
            "too small", "faint", "size clamped", "early key", "late key", "built", "built by sub_0015BF30", "flushes"};
        std::fprintf(stderr, "verify sub_0015B380 paths:");
        for (int i = 0; i < kPaths; i++)
            std::fprintf(stderr, " %s %llu%s", names[i], (unsigned long long)g_paths[i].load(),
                         i + 1 < kPaths ? "," : "\n");
        char x87[48];
        std::snprintf(x87, sizeof x87, ", %llu x87 mismatches", (unsigned long long)g_quad_x87_bad.load());
        std::fprintf(stderr, "verify sub_0015BF30: %llu calls, %llu mismatches%s\n",
                     (unsigned long long)g_quad_calls.load(), (unsigned long long)g_quad_bad.load(), x87);
    }
}

#endif
