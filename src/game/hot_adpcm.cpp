// Native replacement of the game's 4-bit audio block decoder (sub_00140960): 32 samples from 19 bytes at ecx (two
// 12-bit seeds, each with a nibble picking the predictor and the step, then three runs of 10 nibbles), each sample
// nibble << 28 times the step plus the two previous samples times the predictor pair, then scaled, clamped and
// truncated to int16 at edi. Called on the game thread for streamed sounds.
//
// A transliteration with the x86 registers in locals: the original's stores (the samples and its scratch slots on
// the stack, the two registers it pushes), the same operand order (mulss, addss, maxss, minss as the generated code
// takes them), and the registers and xmm it leaves. Build with BLACK_VERIFY_HOT to run the original after every call
// and count the calls where anything differs.
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#ifdef __aarch64__
#include <arm_neon.h>
#endif

#include "game/hot_ordered.h"
#include "game/hot_synth.h"
#include "game/hot_verify.h"

using namespace hot;

X86_EXTERN_FUNC(__imp__sub_00140960);

namespace {

#ifdef BLACK_VERIFY_HOT
PathCounts g_adpcm_paths{{}, {"native", "operands-in-frame"}};
#endif

inline bool Overlap(uint32_t a, uint64_t an, uint32_t b, uint64_t bn) {
    return uint64_t(a) < uint64_t(b) + bn && uint64_t(b) < uint64_t(a) + an;
}

// sub_00140960 (bytes in ecx, out in edi). Frame: sub esp, 0x90 (esp = f); push ebx and push esi around the runs.
// The samples at [f+0x10 + 4 * n]; [f], [f+4], [f+8], [f+0x0C] scratch of the output loop.
static void DecodeBlock(X86Context& __restrict ctx, uint8_t* base) {
    const uint32_t e = ctx.esp, f = e - 0x90, in = ctx.ecx, out = ctx.edi;
    if (Overlap(in, 20, f - 8, e - (f - 8)) || Overlap(out, 64, f - 8, e - (f - 8)) || Overlap(out, 64, in, 20)) {
        HOT_PATH(g_adpcm_paths, 1);
        __imp__sub_00140960(ctx, base);
        return;
    }
    HOT_PATH(g_adpcm_paths, 0);
    const uint8_t* p = base + in;
    f4 x3;
    std::memcpy(&x3, &ctx.xmm[3], 16);  // cvtsi2ss, mulss, addss keep its upper lanes
    const float scale = F32(base, 0x295DA0u);
    uint32_t eax = p[0];
    uint32_t edx = eax & 0x0F;
    const float c0 = F32(base, 0x289F44u + edx * 8), c1 = F32(base, 0x289F48u + edx * 8);
    edx = uint32_t(int32_t(int8_t(p[1]))) << 8;
    eax &= 0xF0;
    edx += eax;
    eax = p[2];
    const float seed0 = SMul(float(int32_t(edx)), scale);
    X86_MEMF32(f + 0x10) = seed0;
    edx = eax & 0x0F;
    const float step = F32(base, 0x289F64u + edx * 4);
    edx = uint32_t(int32_t(int8_t(p[3]))) << 8;
    eax &= 0xF0;
    edx += eax;
    X86_MEM32(f - 4) = ctx.ebx;
    x3[0] = float(int32_t(edx));
    X86_MEM32(f - 8) = ctx.esi;
    x3[0] = SMul(x3[0], scale);
    X86_MEMF32(f + 0x14) = x3[0];
    // the runs: s[n] = (nibble << 28) * step + s[n-2] * c1 + s[n-1] * c0
    float prev2 = seed0, prev1 = x3[0], x4 = 0;
    uint32_t at = f + 0x18;
    const uint8_t* q = p + 4;
    for (int run = 0; run < 3; run++, q += 5) {
        for (int b = 0; b < 5; b++) {
            edx = q[b];
            for (int half = 0; half < 2; half++) {
                const uint32_t v = half ? edx << 28 : (edx >> 4) << 28;
                const float t = SMul(prev2, c1);
                float s = SMul(float(int32_t(v)), step);
                s = SAdd(s, t);
                x4 = SMul(prev1, c0);
                s = SAdd(s, x4);
                X86_MEMF32(at) = s;
                at += 4;
                prev2 = prev1;
                prev1 = s;
            }
            edx <<= 28;
        }
    }
    x3[0] = prev1;
    // scale, clamp to [lo, hi] (maxss then minss) and truncate each sample
    const float hi = F32(base, 0x295DD4u), lo = F32(base, 0x295DD0u);
    X86_MEMF32(f + 4) = hi;
    X86_MEMF32(f + 0xC) = lo;
    float x0 = 0;
    uint32_t ecx = 0;
#ifdef __aarch64__
    if (hi == 32767.0f && lo == -32768.0f) {
        const float32x4_t upper = vdupq_n_f32(hi), lower = vdupq_n_f32(lo);
        for (uint32_t i = 0; i < 32; i += 4) {
            const float32x4_t scaled = VMul(Load4(base, f + 0x10 + 4 * i), Splat(hi));
            // SSE max/min select their second operand for NaNs and ties.
            float32x4_t clipped = vbslq_f32(vcgtq_f32(scaled, lower), scaled, lower);
            clipped = vbslq_f32(vcltq_f32(clipped, upper), clipped, upper);
            const int32x4_t samples = vcvtq_s32_f32(clipped);
            vst1_s16(reinterpret_cast<int16_t*>(base + out + 2 * i), vmovn_s32(samples));
            if (i == 28) {
                X86_MEMF32(f) = vgetq_lane_f32(scaled, 3);
                x0 = vgetq_lane_f32(clipped, 3);
                X86_MEMF32(f + 8) = x0;
                ecx = uint32_t(vgetq_lane_s32(samples, 3));
            }
        }
    } else
#endif
    {
        for (uint32_t i = 0; i < 32; i++) {
            x0 = SMul(F32(base, f + 0x10 + 4 * i), hi);
            X86_MEMF32(f) = x0;
            x0 = sse_max(x0, F32(base, f + 0xC));
            x0 = sse_min(x0, F32(base, f + 4));
            X86_MEMF32(f + 8) = x0;
            ecx = uint32_t(sse_cvtt(x0));
            X86_MEM16(out + 2 * i) = uint16_t(ecx);
        }
    }
    ctx.eax = 32;
    ctx.ecx = ecx;
    ctx.edx = edx;
    PutXmm(ctx.xmm[0], Scalar(x0));
    PutXmm(ctx.xmm[1], Scalar(hi));
    PutXmm(ctx.xmm[2], Scalar(step));
    PutXmm(ctx.xmm[3], x3);
    PutXmm(ctx.xmm[4], Scalar(x4));
    ctx.esp = e + 4;
}

}  // namespace

#ifndef BLACK_VERIFY_HOT

X86_FUNC(sub_00140960) {
    DecodeBlock(ctx, base);
}

#else

namespace {
std::atomic<uint64_t> g_adpcm_calls{0}, g_adpcm_bad{0};
std::atomic<bool> g_synthetic{false};

// once, synthetic blocks in the dead stack 0x800 below esp: every predictor and step nibble, extreme seeds and
// nibbles (the clamp at both ends), pseudo-random bytes
void Synthetic(X86Context& live, uint8_t* base) {
    const uint32_t t = (live.esp - 0x800) & ~15u;
    uint32_t seed = 0x12345678u;
    for (int n = 0; n < 48; n++) {
        // the bytes first, to name blocks whose predictor or step entries are not finite (indices the game does not
        // use): their NaNs meet in mulss/addss, where the generated reference is not NaN-exact
        uint8_t bytes[20];
        for (uint32_t i = 0; i < 20; i++) {
            seed = seed * 1664525u + 1013904223u;
            uint8_t b = uint8_t(seed >> 24);
            if (n < 16 && (i == 0 || i == 2)) b = uint8_t((b & 0xF0) | n);   // each predictor and step
            if (n >= 16 && n < 24) b = (n & 1) ? 0x77 : 0x88;                  // the largest nibbles
            if (n >= 24 && n < 28 && (i == 1 || i == 3)) b = (n & 1) ? 0x7F : 0x80;  // extreme seeds
            bytes[i] = b;
        }
        const bool finite = std::isfinite(F32(base, 0x289F44u + (bytes[0] & 15) * 8)) &&
                            std::isfinite(F32(base, 0x289F48u + (bytes[0] & 15) * 8)) &&
                            std::isfinite(F32(base, 0x289F64u + (bytes[2] & 15) * 4));
        SyntheticCall("sub_00140960 block " + std::to_string(n) +
                          (finite ? "" : " [table entries not finite: reference not NaN-exact, a difference is expected]"),
                      live, base, DecodeBlock, __imp__sub_00140960, t, {}, [&](X86Context& c) {
            for (uint32_t i = 0; i < 20; i++) X86_MEM8(t + i) = bytes[i];
            c.ecx = t;
            c.edi = t + 0x40;
            c.esp = t - 0x40;
            X86_MEM32(c.esp) = 0;
        });
    }
}
}  // namespace

X86_FUNC(sub_00140960) {
    if (!g_synthetic.exchange(true)) Synthetic(ctx, base);
    const uint32_t e = ctx.esp;
    Ranges r;
    r.Add(e - 0x98, 0x98);
    r.Add(ctx.edi, 64);
    VerifyAll("sub_00140960", ctx, base, DecodeBlock, __imp__sub_00140960, r, g_adpcm_calls, g_adpcm_bad, &g_adpcm_paths);
}

#endif
