// Native replacement of the XMV film decoder's YUV 4:2:0 -> ARGB converter (sub_002403D1) and its 16x16 macroblock
// kernel (sub_002401D2). The menus' backgrounds and the loading screens are looping films decoded on the game thread,
// and in generated code this converter, MMX emulated lane by lane, is most of the decode time.
//
// After portingpete/Black-2006-Recomp, src/black_xmv_yuv.inc (MIT, (c) its authors): the same arithmetic, here with
// NEON on AArch64 and the converter's exact register and stack effects. Per pixel, as the kernel computes it:
//   Y' = (Y - 16) * 38139 (pmaddwd of 0x54FB and 0x4000), a float; u = U - 128, v = V - 128 as floats
//   B = cvt(u * 66126 + Y'), R = cvt(v * 52298 + Y'), G = cvt((Y' + v * -26640) + u * -12812)
//   each >> 15 (psrad), saturated to int16 (packssdw) then to 0..255 (packuswb); alpha 0xFF.
// The products are exact (integers below 2^24); the sums round, so they keep the guest's order. cvt is cvtps2pi in
// the thread's rounding mode (MXCSR.RC; the generated code rounds to nearest whatever the mode, which is the mode the
// game runs in). The walk follows sub_002403D1: 16x16 blocks, a partial last column or row converted into the frame's
// scratch block and copied. The final registers (eax, ecx, edx, mm0-mm7, xmm0-xmm7) and the stack bytes both
// functions write (locals, argument slots, the kernel's frames) are left as the original leaves them.
//
// BLACK_XMV_YUV=0 keeps the generated converter; BLACK_XMV_YUV=verify runs both on the first 400 calls and compares
// registers, the frame, the stack and the output (the generated converter is the oracle).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/memory.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

X86_EXTERN_FUNC(__imp__sub_002403D1);

namespace {

constexpr uint32_t kCb = 0x47812700, kCgv = 0xC6D02000, kCgu = 0xC6483000, kCr = 0x474C4A00;
constexpr uint32_t kRetFull = 0x24056B, kRetPartial = 0x2404F2;
constexpr uint64_t kMm5 = 0x400054FB400054FBull, kMm6 = 0xFFFFFF80FFFFFF80ull, kMm7 = 0xFFF0FFF0FFF0FFF0ull;

// Where a pixel row is written. The decoder writes its frames through the write combined view; where that view is
// pages of its own (Horizon without svcMapProcessMemory) the frame goes to the contiguous view, which the renderer
// reads, at the same physical address.
inline uint32_t Out(uint32_t va) {
    static const bool separate = !mem::WriteCombineAliased();
    if (separate && va - mem::kWriteCombineBase < mem::kPhysicalSize)
        return va - mem::kWriteCombineBase + mem::kContiguousBase;
    return va;
}

// a guest dword as a plain value (GCC for AArch64 fails on the aligned(1) type of X86_MEM32 in a variadic call)
inline uint32_t Arg(uint8_t* base, uint32_t va) {
    uint32_t v;
    std::memcpy(&v, base + va, 4);
    return v;
}

inline float F(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

inline int32_t Cvt(float v, int rc) {
    switch (rc) {
        case 0: return int32_t(std::nearbyint(v));  // the host rounds to nearest
        case 1: return int32_t(std::floor(v));
        case 2: return int32_t(std::ceil(v));
        default: return int32_t(std::trunc(v));
    }
}
// psrad 15, packssdw, packuswb
inline uint32_t Chan(int32_t c) {
    int32_t s = c >> 15;
    s = s < -32768 ? -32768 : s > 32767 ? 32767 : s;
    return uint32_t(s < 0 ? 0 : s > 255 ? 255 : s);
}

struct Px {
    float b, g, r;
};
inline Px Floats(uint8_t y, float u, float v) {
    float yf = float((int32_t(y) - 16) * 38139);
    return {u * F(kCb) + yf, (yf + v * F(kCgv)) + u * F(kCgu), v * F(kCr) + yf};
}
inline uint32_t Pixel(const Px& p, int rc) {
    return 0xFF000000u | Chan(Cvt(p.r, rc)) << 16 | Chan(Cvt(p.g, rc)) << 8 | Chan(Cvt(p.b, rc));
}

// one 16x16 macroblock: Y with stride ys, U and V with stride cs, pixels at dst with pitch ds
#if defined(__aarch64__)
template <int RC>
inline int32x4_t CvtN(float32x4_t v) {
    if constexpr (RC == 0) return vcvtnq_s32_f32(v);
    else if constexpr (RC == 1) return vcvtmq_s32_f32(v);
    else if constexpr (RC == 2) return vcvtpq_s32_f32(v);
    else return vcvtq_s32_f32(v);
}
template <int RC>
inline uint8x8_t ChanN(float32x4_t a, float32x4_t b) {
    int16x8_t s = vcombine_s16(vqmovn_s32(vshrq_n_s32(CvtN<RC>(a), 15)), vqmovn_s32(vshrq_n_s32(CvtN<RC>(b), 15)));
    return vqmovun_s16(s);
}
inline float32x4_t Centred(const uint8_t* p) {
    uint32_t w;
    std::memcpy(&w, p, 4);
    int32x4_t x = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vmovl_u8(vcreate_u8(w)))));
    return vcvtq_f32_s32(vsubq_s32(x, vdupq_n_s32(128)));
}
// 8 pixels of a row, chroma lanes [c0 c0 c1 c1] [c2 c2 c3 c3]
template <int RC>
inline void Row8(const uint8_t* y, float32x4_t u0, float32x4_t u1, float32x4_t v0, float32x4_t v1, uint8_t* dst) {
    int16x8_t yw = vreinterpretq_s16_u16(vsubq_u16(vmovl_u8(vld1_u8(y)), vdupq_n_u16(16)));
    float32x4_t y0 = vcvtq_f32_s32(vmulq_n_s32(vmovl_s16(vget_low_s16(yw)), 38139));
    float32x4_t y1 = vcvtq_f32_s32(vmulq_n_s32(vmovl_s16(vget_high_s16(yw)), 38139));
    const float32x4_t cb = vdupq_n_f32(F(kCb)), cr = vdupq_n_f32(F(kCr));
    const float32x4_t cgv = vdupq_n_f32(F(kCgv)), cgu = vdupq_n_f32(F(kCgu));
    uint8x8x4_t px;
    px.val[0] = ChanN<RC>(vaddq_f32(vmulq_f32(u0, cb), y0), vaddq_f32(vmulq_f32(u1, cb), y1));
    px.val[1] = ChanN<RC>(vaddq_f32(vaddq_f32(y0, vmulq_f32(v0, cgv)), vmulq_f32(u0, cgu)),
                          vaddq_f32(vaddq_f32(y1, vmulq_f32(v1, cgv)), vmulq_f32(u1, cgu)));
    px.val[2] = ChanN<RC>(vaddq_f32(vmulq_f32(v0, cr), y0), vaddq_f32(vmulq_f32(v1, cr), y1));
    px.val[3] = vdup_n_u8(0xFF);
    vst4_u8(dst, px);
}
template <int RC>
void Block16N(const uint8_t* Y, uint32_t ys, const uint8_t* U, const uint8_t* V, uint32_t cs, uint8_t* dst,
              uint32_t ds) {
    for (uint32_t pair = 0; pair < 8; pair++) {
        const uint8_t* y = Y + 2 * pair * ys;
        uint8_t* d = dst + 2 * pair * ds;
        for (uint32_t half = 0; half < 2; half++) {
            float32x4_t u = Centred(U + pair * cs + 4 * half), v = Centred(V + pair * cs + 4 * half);
            float32x4_t u0 = vzip1q_f32(u, u), u1 = vzip2q_f32(u, u), v0 = vzip1q_f32(v, v), v1 = vzip2q_f32(v, v);
            Row8<RC>(y + 8 * half, u0, u1, v0, v1, d + 32 * half);
            Row8<RC>(y + ys + 8 * half, u0, u1, v0, v1, d + ds + 32 * half);
        }
    }
}
#endif

void Block16(const uint8_t* Y, uint32_t ys, const uint8_t* U, const uint8_t* V, uint32_t cs, uint8_t* dst,
             uint32_t ds, int rc) {
#if defined(__aarch64__)
    switch (rc) {
        case 0: return Block16N<0>(Y, ys, U, V, cs, dst, ds);
        case 1: return Block16N<1>(Y, ys, U, V, cs, dst, ds);
        case 2: return Block16N<2>(Y, ys, U, V, cs, dst, ds);
        default: return Block16N<3>(Y, ys, U, V, cs, dst, ds);
    }
#else
    for (uint32_t row = 0; row < 16; row++)
        for (uint32_t x = 0; x < 16; x++) {
            float u = float(int32_t(U[(row / 2) * cs + x / 2]) - 128);
            float v = float(int32_t(V[(row / 2) * cs + x / 2]) - 128);
            uint32_t p = Pixel(Floats(Y[row * ys + x], u, v), rc);
            std::memcpy(dst + row * ds + x * 4, &p, 4);
        }
#endif
}

inline void SetXmm(X86Xmm& x, float a, float b, float c, float d) {
    x.f[0] = a;
    x.f[1] = b;
    x.f[2] = c;
    x.f[3] = d;
}

// The kernel on the block at (y, u, v): its pixels, its stack frame, and its exit state: eax 0, ecx and edx past its
// last chroma reads, mm0-mm3 and xmm0-xmm3 from its last step (rows 14 and 15, pixels 12 to 15).
void Kernel(X86Context& ctx, uint8_t* base, uint32_t esp, uint32_t y, uint32_t u, uint32_t v, uint32_t ys,
            uint32_t cs, uint32_t dst, uint32_t ds, uint32_t ret, int rc) {
    const uint32_t args[7] = {y, u, v, ys, cs, dst, ds};
    for (int i = 0; i < 7; i++) X86_MEM32(esp - 0x1C + 4 * i) = args[i];
    X86_MEM32(esp - 0x20) = ret;
    X86_MEM32(esp - 0x24) = ctx.ebp;
    X86_MEM32(esp - 0x28) = ctx.ebx;
    X86_MEM32(esp - 0x2C) = ctx.esi;
    X86_MEM32(esp - 0x30) = ctx.edi;
    Block16(X86_PTR(uint8_t, y), ys, X86_PTR(uint8_t, u), X86_PTR(uint8_t, v), cs, X86_PTR(uint8_t, Out(dst)), ds, rc);

    const uint32_t cu = u + 7 * cs + 4, cv = v + 7 * cs + 4;
    ctx.eax = 0;
    ctx.ecx = cu;
    ctx.edx = cv;
    const uint8_t* uc = X86_PTR(uint8_t, cu);
    const uint8_t* vc = X86_PTR(uint8_t, cv);
    const int32_t uc2 = int32_t(uc[2]) - 128, uc3 = int32_t(uc[3]) - 128;
    const int32_t vc2 = int32_t(vc[2]) - 128, vc3 = int32_t(vc[3]) - 128;
    const float fu2 = float(uc2), fu3 = float(uc3), fv2 = float(vc2), fv3 = float(vc3);
    const uint8_t* yl = X86_PTR(uint8_t, y + 15 * ys + 12);
    const Px p0 = Floats(yl[0], fu2, fv2), p1 = Floats(yl[1], fu2, fv2);
    const Px p2 = Floats(yl[2], fu3, fv3), p3 = Floats(yl[3], fu3, fv3);
    ctx.mm[0] = uint64_t(Pixel(p0, rc)) | uint64_t(Pixel(p1, rc)) << 32;
    ctx.mm[1] = uint64_t(Pixel(p2, rc)) | uint64_t(Pixel(p3, rc)) << 32;
    ctx.mm[2] = uint64_t(Chan(Cvt(p2.g, rc))) << 8 | uint64_t(Chan(Cvt(p3.g, rc))) << 40;
    ctx.mm[3] = uint64_t(uint32_t(vc2)) | uint64_t(uint32_t(vc3)) << 32;
    SetXmm(ctx.xmm[0], p2.g, p3.g, p2.g, p0.g);
    SetXmm(ctx.xmm[1], fu2, fu2, fu3, fu3);
    SetXmm(ctx.xmm[2], fv2, fv2, fv3, fv3);
    const float g2 = fu2 * F(kCgu), g3 = fu3 * F(kCgu);
    SetXmm(ctx.xmm[3], g2, g2, g3, g3);
}

// sub_002403D1(blocks across, blocks down, Y, U, V, width, height, out, pitch), stdcall
void Convert(X86Context& ctx, uint8_t* base) {
    const uint32_t sp = ctx.esp;
    const uint32_t a0 = X86_MEM32(sp + 4), a1 = X86_MEM32(sp + 8), a5 = X86_MEM32(sp + 0x18),
                   a6 = X86_MEM32(sp + 0x1C), pitch = X86_MEM32(sp + 0x24);
    const int rc = int(ctx.mxcsr >> 13) & 3;
    const uint32_t ebp = sp - 4;  // push ebp; mov ebp, esp
    const uint32_t esp = ebp - 0x474;
    const uint32_t ebx0 = ctx.ebx, esi0 = ctx.esi, edi0 = ctx.edi, ebp0 = ctx.ebp;
    X86_MEM32(ebp) = ebp0;
    X86_MEM32(ebp - 0x46C) = ebx0;
    X86_MEM32(ebp - 0x470) = esi0;
    X86_MEM32(ebp - 0x474) = edi0;
    auto L = [&](int32_t off) -> x86_u32u& { return X86_MEM32(ebp + uint32_t(off)); };

    const uint32_t bottom = a1 * 16 - a6, right = a0 * 16 - a5, ys = a0 << 4, cs = a0 << 3;
    L(0x20) = bottom;
    uint32_t ecx = pitch - (a0 << 6), edx = cs, eax = kCr;
    L(-0x18) = L(-0x14) = 0x400054FB;
    L(-0x10) = L(-0xC) = 0xFFFFFF80;
    L(-0x20) = L(-0x1C) = 0xFFF0FFF0;
    for (int i = 0; i < 4; i++) {
        L(-0x58 + 4 * i) = kCb;
        L(-0x48 + 4 * i) = kCgv;
        L(-0x68 + 4 * i) = kCgu;
        L(-0x38 + 4 * i) = kCr;
    }
    L(-0x28) = right;
    ctx.mm[4] = 0;
    ctx.mm[5] = kMm5;
    ctx.mm[6] = kMm6;
    ctx.mm[7] = kMm7;
    const uint32_t consts[4] = {kCb, kCgv, kCgu, kCr};
    for (int i = 0; i < 4; i++)
        for (int l = 0; l < 4; l++) ctx.xmm[4 + i].u[l] = consts[i];

    // registers the kernel calls see: ebp the frame, ebx the Y stride, esi blocks across, edi the right pad
    ctx.ebp = ebp;
    ctx.ebx = ys;
    ctx.esi = a0;
    ctx.edi = right;
    if (a1) {
        edx = cs * 7;
        L(-0xC) = ys * 15;
        L(-0x14) = edx;
        L(-0x1C) = pitch * 15 + ecx;
        const uint32_t scratch = ebp - 0x468;
        while (true) {
            uint32_t rows_left = --L(0xC);
            L(0x1C) = a0;
            if (a0) {
                while (true) {
                    uint32_t cols_left = --L(0x1C);
                    bool partial = (cols_left == 0 && right) || (rows_left == 0 && bottom);
                    const uint32_t y = L(0x10), u = L(0x14), v = L(0x18), dst = L(0x24);
                    if (!partial) {
                        Kernel(ctx, base, esp, y, u, v, ys, cs, dst, pitch, kRetFull, rc);
                        ecx = ctx.ecx;
                    } else {
                        Kernel(ctx, base, esp, y, u, v, ys, cs, scratch, 0x40, kRetPartial, rc);
                        X86_MEM32(esp - 4) = 0x10;  // push 0x10 / push ecx over the first argument's slot
                        uint32_t cols = cols_left ? 16 : 16 - right;
                        uint32_t rows = rows_left ? 16 : 16 - bottom;
                        eax = cols;
                        ecx = rows;
                        edx = ctx.edx;
                        if (rows) {
                            L(-0x24) = cols * 4;
                            L(-8) = rows;
                            uint32_t d = dst, s = scratch;
                            for (uint32_t r = 0; r < rows; r++, d += pitch, s += 0x40) {
                                std::memmove(X86_PTR(uint8_t, Out(d)), X86_PTR(uint8_t, s), cols * 4);
                                L(-4) = d + pitch;
                            }
                            L(-4) = d;
                            L(-8) = 0;
                            eax = cols * 4;
                            edx = scratch + rows * 0x40;
                        }
                        ecx = 0;
                        ctx.eax = eax;
                        ctx.edx = edx;
                    }
                    L(0x10) += 16;
                    L(0x14) += 8;
                    L(0x18) += 8;
                    L(0x24) += 64;
                    if (cols_left == 0) break;
                }
                edx = L(-0x14);
            }
            L(0x10) += L(-0xC);
            eax = L(-0x1C);
            L(0x14) += edx;
            L(0x18) += edx;
            L(0x24) += eax;
            if (rows_left == 0) break;
        }
    }
    ctx.eax = eax;
    ctx.ecx = ecx;
    ctx.edx = edx;
    ctx.ebx = ebx0;
    ctx.esi = esi0;
    ctx.edi = edi0;
    ctx.ebp = ebp0;
    ctx.esp = sp + 4 + 0x24;
}

int Mode() {
    static const int mode = [] {
        const char* e = std::getenv("BLACK_XMV_YUV");
        int m = e && !std::strcmp(e, "verify") ? 2 : (e && *e == '0') ? 0 : 1;
        LOGI("xmv: YUV converter %s", m == 0 ? "generated" : m == 1 ? "native" : "native, verified");
        return m;
    }();
    return mode;
}

// BLACK_XMV_YUV=verify: the native converter, then the generated one from the same state; registers, the frame and
// stack bytes below it, the argument slots and the output rows must match
std::atomic<uint32_t> g_verified{0}, g_bad{0};
void Verify(X86Context& ctx, uint8_t* base) {
    const uint32_t sp = ctx.esp, a0 = Arg(base, sp + 4), a1 = Arg(base, sp + 8), w = Arg(base, sp + 0x18),
                   h = Arg(base, sp + 0x1C), dst = Arg(base, sp + 0x20), pitch = Arg(base, sp + 0x24);
    struct Range {
        uint32_t va, size;
    };
    // the native converter writes the frame where the renderer reads it (Out), the generated one through the
    // write combined view: each side's output is read where it went
    // the rows the converter writes: the picture's height (the last block row is cut to it)
    const uint32_t rows = std::min(h, a1 * 16);
    const Range ranges[] = {{sp - 0x4B0, 0x4B0 + 0x28}, {Out(dst), pitch * rows}};
    const Range oracle[] = {ranges[0], {dst, pitch * rows}};
    std::vector<uint8_t> before[2], mine[2];
    for (int i = 0; i < 2; i++) before[i].assign(X86_PTR(uint8_t, ranges[i].va), X86_PTR(uint8_t, ranges[i].va) + ranges[i].size);
    const X86Context start = ctx;
    Convert(ctx, base);
    const X86Context native = ctx;
    for (int i = 0; i < 2; i++) mine[i].assign(X86_PTR(uint8_t, ranges[i].va), X86_PTR(uint8_t, ranges[i].va) + ranges[i].size);
    for (int i = 1; i >= 0; i--) std::memcpy(X86_PTR(uint8_t, ranges[i].va), before[i].data(), ranges[i].size);
    ctx = start;
    __imp__sub_002403D1(ctx, base);
    bool regs = std::memcmp(&native, &ctx, offsetof(X86Context, cf)) == 0;
    bool mm = std::memcmp(native.mm, ctx.mm, sizeof ctx.mm) == 0;
    bool xmm = std::memcmp(native.xmm, ctx.xmm, sizeof ctx.xmm) == 0;
    size_t diff[2] = {};
    for (int i = 0; i < 2; i++)
        for (uint32_t k = 0; k < ranges[i].size; k++) diff[i] += mine[i][k] != X86_PTR(uint8_t, oracle[i].va)[k];
    uint32_t n = ++g_verified;
    if (!(regs && mm && xmm && !diff[0] && !diff[1])) {
        uint32_t b = ++g_bad;
        if (b <= 8) {
            LOGE("xmv verify: call %u differs: registers %d mm %d xmm %d, stack %zu bytes, output %zu bytes (%ux%u "
                 "blocks, %ux%u px, pitch %u)",
                 n, regs, mm, xmm, diff[0], diff[1], a0, a1, w, h, pitch);
            const uint32_t* a = &native.eax;
            const uint32_t* o = &ctx.eax;
            for (int i = 0; i < 8; i++)
                if (a[i] != o[i]) LOGE("  gpr %d: %08X, original %08X", i, a[i], o[i]);
            for (int i = 0; i < 8; i++)
                if (native.mm[i] != ctx.mm[i])
                    LOGE("  mm%d: %016llX, original %016llX", i, (unsigned long long)native.mm[i],
                         (unsigned long long)ctx.mm[i]);
            for (int i = 0; i < 8; i++)
                if (std::memcmp(&native.xmm[i], &ctx.xmm[i], 16))
                    LOGE("  xmm%d: %08X %08X %08X %08X, original %08X %08X %08X %08X", i, native.xmm[i].u[0],
                         native.xmm[i].u[1], native.xmm[i].u[2], native.xmm[i].u[3], ctx.xmm[i].u[0], ctx.xmm[i].u[1],
                         ctx.xmm[i].u[2], ctx.xmm[i].u[3]);
        }
    }
    if (n == 1 || n % 100 == 0) LOGI("xmv verify: %u calls, %u differ", n, g_bad.load());
}

// once in verify mode: the same planes as a picture 6 pixels narrower and 10 lower, so the walk takes its partial
// last column and row; the arguments and registers are put back for the real call
void Synthetic(X86Context& ctx, uint8_t* base) {
    static bool done = false;
    if (done) return;
    done = true;
    uint32_t args[9];
    for (int i = 0; i < 9; i++) args[i] = X86_MEM32(ctx.esp + 4 + 4 * i);
    const X86Context saved = ctx;
    X86_MEM32(ctx.esp + 0x18) = args[5] - 6;
    X86_MEM32(ctx.esp + 0x1C) = args[6] - 10;
    Verify(ctx, base);
    ctx = saved;
    for (int i = 0; i < 9; i++) X86_MEM32(ctx.esp + 4 + 4 * i) = args[i];
}

// time per converted frame, logged every 300 frames
struct Timing {
    uint64_t ns = 0, frames = 0;
} g_timing;

}  // namespace

X86_FUNC(sub_002403D1) {
    const int mode = Mode();
    static bool first = true;
    if (first) {
        first = false;
        LOGI("xmv: converter %ux%u blocks, %ux%u px, Y %08X out %08X pitch %u", Arg(base, ctx.esp + 4),
             Arg(base, ctx.esp + 8), Arg(base, ctx.esp + 0x18), Arg(base, ctx.esp + 0x1C), Arg(base, ctx.esp + 0xC),
             Arg(base, ctx.esp + 0x20), Arg(base, ctx.esp + 0x24));
    }
    auto t0 = std::chrono::steady_clock::now();
    // the rows the converter writes: the picture's height (the last block row is cut to it)
    const uint32_t dst = X86_MEM32(ctx.esp + 0x20),
                   rows = std::min(Arg(base, ctx.esp + 0x1C), Arg(base, ctx.esp + 8) * 16),
                   pitch = X86_MEM32(ctx.esp + 0x24);
    if (mode == 2 && g_verified < 400) {
        Synthetic(ctx, base);
        Verify(ctx, base);
    } else if (mode == 0) {
        __imp__sub_002403D1(ctx, base);
    } else {
        Convert(ctx, base);
        goto timed;
    }
    // the generated converter wrote through the write combined view
    if (Out(dst) != dst) std::memcpy(X86_PTR(uint8_t, Out(dst)), X86_PTR(uint8_t, dst), size_t(rows) * pitch);
timed:
    g_timing.ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    if (++g_timing.frames % 300 == 0) {
        LOGI("xmv: YUV conversion %.2f ms per frame (%s)", double(g_timing.ns) / 1e6 / 300.0,
             mode == 0 ? "generated" : "native");
        g_timing.ns = 0;
    }
}

// The decoder's two IDCTs, sub_002420C8 (8x8 to bytes) and sub_00242535 (8x8 to 16-bit rows), MMX leaves without
// data-dependent control flow but their loops. In generated code each MMX instruction is a loop over bytes; here each
// is one operation on a 64-bit value, in the same order, with the same memory writes and the same final registers:
// the bodies (hot_xmv_idct*.inc in the generated code) are translated from the game's default.xbe by tools/mmx2c.py
// at recompile time (switch/recomp.sh), so the repo holds no game code.
// BLACK_XMV_IDCT=0 keeps the generated code; =verify compares the first 20000 calls of each with it.
namespace {
inline uint64_t Q(uint8_t* base, uint32_t va) {
    uint64_t v;
    std::memcpy(&v, base + va, 8);
    return v;
}
inline void PutQ(uint8_t* base, uint32_t va, uint64_t v) { std::memcpy(base + va, &v, 8); }
inline uint32_t Lo(uint64_t v) { return uint32_t(v); }
inline uint32_t Hi(uint64_t v) { return uint32_t(v >> 32); }
inline uint64_t D(uint32_t lo, uint32_t hi) { return uint64_t(lo) | uint64_t(hi) << 32; }
inline uint16_t W(uint64_t v, int i) { return uint16_t(v >> (16 * i)); }
inline uint64_t Ws(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    return uint64_t(a) | uint64_t(b) << 16 | uint64_t(c) << 32 | uint64_t(d) << 48;
}
inline uint64_t AddD(uint64_t a, uint64_t b) { return D(Lo(a) + Lo(b), Hi(a) + Hi(b)); }
inline uint64_t SubD(uint64_t a, uint64_t b) { return D(Lo(a) - Lo(b), Hi(a) - Hi(b)); }
inline uint64_t AddW(uint64_t a, uint64_t b) {
    return Ws(uint16_t(W(a, 0) + W(b, 0)), uint16_t(W(a, 1) + W(b, 1)), uint16_t(W(a, 2) + W(b, 2)),
              uint16_t(W(a, 3) + W(b, 3)));
}
inline uint32_t Madd(uint16_t a0, uint16_t a1, uint16_t b0, uint16_t b1) {
    return uint32_t(int64_t(int16_t(a0)) * int16_t(b0) + int64_t(int16_t(a1)) * int16_t(b1));
}
inline uint64_t MaddWD(uint64_t a, uint64_t b) {
    return D(Madd(W(a, 0), W(a, 1), W(b, 0), W(b, 1)), Madd(W(a, 2), W(a, 3), W(b, 2), W(b, 3)));
}
inline uint64_t UnpackLoDQ(uint64_t a, uint64_t b) { return D(Lo(a), Lo(b)); }
inline uint64_t UnpackHiDQ(uint64_t a, uint64_t b) { return D(Hi(a), Hi(b)); }
inline uint64_t UnpackLoWD(uint64_t a, uint64_t b) { return Ws(W(a, 0), W(b, 0), W(a, 1), W(b, 1)); }
inline uint16_t Sat16(uint32_t v) {
    int32_t s = int32_t(v);
    return uint16_t(s < -32768 ? -32768 : s > 32767 ? 32767 : s);
}
inline uint64_t PackSSDW(uint64_t a, uint64_t b) { return Ws(Sat16(Lo(a)), Sat16(Hi(a)), Sat16(Lo(b)), Sat16(Hi(b))); }
inline uint64_t Satu8(uint16_t v) {
    int16_t s = int16_t(v);
    return uint64_t(s < 0 ? 0 : s > 255 ? 255 : s);
}
inline uint64_t PackUSWB(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 4; i++) r |= Satu8(W(a, i)) << (8 * i) | Satu8(W(b, i)) << (8 * (i + 4));
    return r;
}
inline uint64_t SllD(uint64_t a, int n) { return n > 31 ? 0 : D(Lo(a) << n, Hi(a) << n); }
inline uint64_t SrlD(uint64_t a, int n) { return n > 31 ? 0 : D(Lo(a) >> n, Hi(a) >> n); }
inline uint64_t SraD(uint64_t a, int n) {
    n = n > 31 ? 31 : n;
    return D(uint32_t(int32_t(Lo(a)) >> n), uint32_t(int32_t(Hi(a)) >> n));
}
inline uint64_t SllQ(uint64_t a, int n) { return n > 63 ? 0 : a << n; }
inline uint64_t SrlQ(uint64_t a, int n) { return n > 63 ? 0 : a >> n; }
inline uint64_t SllW(uint64_t a, int n) {
    return n > 15 ? 0 : Ws(uint16_t(W(a, 0) << n), uint16_t(W(a, 1) << n), uint16_t(W(a, 2) << n), uint16_t(W(a, 3) << n));
}
inline uint64_t SrlW(uint64_t a, int n) {
    return n > 15 ? 0 : Ws(uint16_t(W(a, 0) >> n), uint16_t(W(a, 1) >> n), uint16_t(W(a, 2) >> n), uint16_t(W(a, 3) >> n));
}
inline uint64_t SraW(uint64_t a, int n) {
    n = n > 15 ? 15 : n;
    return Ws(uint16_t(int16_t(W(a, 0)) >> n), uint16_t(int16_t(W(a, 1)) >> n), uint16_t(int16_t(W(a, 2)) >> n),
              uint16_t(int16_t(W(a, 3)) >> n));
}

#include "hot_xmv_idct.inc"
#include "hot_xmv_idct16.inc"

int IdctMode() {
    static const int mode = [] {
        const char* e = std::getenv("BLACK_XMV_IDCT");
        int m = e && !std::strcmp(e, "verify") ? 2 : (e && *e == '0') ? 0 : 1;
        LOGI("xmv: IDCTs %s", m == 0 ? "generated" : m == 1 ? "native" : "native, verified");
        return m;
    }();
    return mode;
}

// a leaf run natively, then generated from the same state: registers, mm, xmm and the given ranges must match
struct LeafStats {
    const char* name;
    std::atomic<uint32_t> calls{0}, bad{0};
};
void VerifyLeaf(LeafStats& st, X86Context& ctx, uint8_t* base, void (*native)(X86Context&, uint8_t*),
                void (*original)(X86Context&, uint8_t*), const uint32_t (*ranges)[2], int count) {
    std::vector<std::vector<uint8_t>> before(count), mine(count);
    for (int i = 0; i < count; i++)
        before[i].assign(base + ranges[i][0], base + ranges[i][0] + ranges[i][1]);
    const X86Context start = ctx;
    native(ctx, base);
    const X86Context ours = ctx;
    for (int i = 0; i < count; i++) mine[i].assign(base + ranges[i][0], base + ranges[i][0] + ranges[i][1]);
    for (int i = count - 1; i >= 0; i--) std::memcpy(base + ranges[i][0], before[i].data(), ranges[i][1]);
    ctx = start;
    original(ctx, base);
    bool regs = std::memcmp(&ours, &ctx, offsetof(X86Context, cf)) == 0;
    bool mm = std::memcmp(ours.mm, ctx.mm, sizeof ctx.mm) == 0;
    bool xmm = std::memcmp(ours.xmm, ctx.xmm, sizeof ctx.xmm) == 0;
    size_t diff = 0;
    for (int i = 0; i < count; i++)
        for (uint32_t k = 0; k < ranges[i][1]; k++) diff += mine[i][k] != base[ranges[i][0] + k];
    uint32_t n = ++st.calls;
    if (!(regs && mm && xmm && !diff) && ++st.bad <= 8) {
        LOGE("xmv verify %s: call %u differs: registers %d mm %d xmm %d, memory %zu bytes", st.name, n, regs, mm, xmm,
             diff);
        const uint32_t* a = &ours.eax;
        const uint32_t* o = &ctx.eax;
        for (int i = 0; i < 8; i++)
            if (a[i] != o[i]) LOGE("  gpr %d: %08X, original %08X", i, a[i], o[i]);
        for (int i = 0; i < 8; i++)
            if (ours.mm[i] != ctx.mm[i])
                LOGE("  mm%d: %016llX, original %016llX", i, (unsigned long long)ours.mm[i], (unsigned long long)ctx.mm[i]);
    }
    if (n == 1 || n % 5000 == 0) LOGI("xmv verify %s: %u calls, %u differ", st.name, n, st.bad.load());
}

LeafStats g_idct_stats{"sub_002420C8"}, g_idct16_stats{"sub_00242535"};

}  // namespace

X86_EXTERN_FUNC(__imp__sub_002420C8);
X86_EXTERN_FUNC(__imp__sub_00242535);

// sub_002420C8(dst, stride, coefficients), stdcall: 8 rows of 8 bytes at dst
void Idct8(X86Context& ctx, uint8_t* base) {
    const int mode = IdctMode();
    if (mode == 0) return __imp__sub_002420C8(ctx, base);
    if (mode == 2 && g_idct_stats.calls < 20000) {
        const uint32_t sp = ctx.esp, dst = X86_MEM32(sp + 4), stride = X86_MEM32(sp + 8);
        const uint32_t ranges[2][2] = {{sp - 0x160, 0x160 + 0x10}, {dst, 7 * stride + 8}};
        return VerifyLeaf(g_idct_stats, ctx, base, IdctNative, __imp__sub_002420C8, ranges, 2);
    }
    IdctNative(ctx, base);
}

// sub_00242535(dst, coefficients, row mask), stdcall: 8 rows of 8 words at dst, 16 bytes apart
void Idct16(X86Context& ctx, uint8_t* base) {
    const int mode = IdctMode();
    if (mode == 0) return __imp__sub_00242535(ctx, base);
    if (mode == 2 && g_idct16_stats.calls < 20000) {
        const uint32_t sp = ctx.esp, dst = X86_MEM32(sp + 4);
        const uint32_t ranges[2][2] = {{sp - 0x120, 0x120 + 0x10}, {dst, 8 * 16}};
        return VerifyLeaf(g_idct16_stats, ctx, base, Idct16Native, __imp__sub_00242535, ranges, 2);
    }
    Idct16Native(ctx, base);
}

// BLACK_XMV_PROF=1: the decoder's parts timed (inclusive), as in portingpete/Black-2006-Recomp's black_xmv_prof.inc:
// the two IDCTs (sub_002420C8, sub_00242535), block clears (sub_0023FAD9) and two others
namespace {
enum Part { kIdct, kIdct16, kClear, kA, kB, kParts };
const char* const kPartNames[kParts] = {"idct8", "idct16", "clear", "sub_00240683", "sub_002415BA"};
uint64_t g_part_ns[kParts], g_part_calls[kParts];
bool Profiling() {
    static const bool on = [] {
        const char* e = std::getenv("BLACK_XMV_PROF");
        return e && *e == '1';
    }();
    return on;
}
template <Part P, void (*F)(X86Context&, uint8_t*)>
inline void Timed(X86Context& ctx, uint8_t* base) {
    if (!Profiling()) return F(ctx, base);
    auto t0 = std::chrono::steady_clock::now();
    F(ctx, base);
    g_part_ns[P] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    g_part_calls[P]++;
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_0023FAD9);
X86_EXTERN_FUNC(__imp__sub_00240683);
X86_EXTERN_FUNC(__imp__sub_002415BA);
X86_FUNC(sub_002420C8) { Timed<kIdct, Idct8>(ctx, base); }
X86_FUNC(sub_00242535) { Timed<kIdct16, Idct16>(ctx, base); }
X86_FUNC(sub_0023FAD9) { Timed<kClear, __imp__sub_0023FAD9>(ctx, base); }
X86_FUNC(sub_00240683) { Timed<kA, __imp__sub_00240683>(ctx, base); }
X86_FUNC(sub_002415BA) { Timed<kB, __imp__sub_002415BA>(ctx, base); }

// XMVDecoder_GetNextFrame(decoder, surface, &result, &time): the time of the calls that decoded a frame (result 1,
// XMV_NEWFRAME), logged as ms per frame every 300 frames, with the converter's share above
X86_EXTERN_FUNC(__imp__sub_0023F45C);
X86_FUNC(sub_0023F45C) {
    static uint64_t ns = 0, frames = 0;
    const uint32_t result = X86_MEM32(ctx.esp + 0xC);
    auto t0 = std::chrono::steady_clock::now();
    __imp__sub_0023F45C(ctx, base);
    if (Arg(base, result) != 1) return;
    ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    if (++frames % 300 == 0) {
        LOGI("xmv: decode %.2f ms per frame", double(ns) / 1e6 / 300.0);
        ns = 0;
        if (Profiling()) {
            for (int p = 0; p < kParts; p++)
                LOGI("xmv:   %s %.2f ms per frame, %.0f calls", kPartNames[p], double(g_part_ns[p]) / 1e6 / 300.0,
                     double(g_part_calls[p]) / 300.0);
            std::memset(g_part_ns, 0, sizeof g_part_ns);
            std::memset(g_part_calls, 0, sizeof g_part_calls);
        }
    }
}
