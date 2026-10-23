// SSE-compatible particle colour conversion, using four NEON lanes on AArch64.
#pragma once
#include "game/hot_ordered.h"
#ifdef __aarch64__
#include <arm_neon.h>
#endif

namespace hot {
inline uint32_t PackColour(f4 rgba) {
#ifdef __aarch64__
    const float32x4_t v = rgba;
    // FCVTZS saturates instead of SSE's integer-indefinite result. Invalid
    // lanes must contribute zero low bytes, including NaNs and +2^31.
    const uint32x4_t valid =
        vandq_u32(vcgeq_f32(v, vdupq_n_f32(-2147483648.0f)), vcltq_f32(v, vdupq_n_f32(2147483648.0f)));
    const uint32x4_t words = vandq_u32(vreinterpretq_u32_s32(vcvtq_s32_f32(v)), valid);
    const uint8x16_t order = {8, 4, 0, 12, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    return vgetq_lane_u32(vreinterpretq_u32_u8(vqtbl1q_u8(vreinterpretq_u8_u32(words), order)), 0);
#else
    return (uint32_t(sse_cvtt(rgba[3])) << 24) | ((uint32_t(sse_cvtt(rgba[0])) & 255) << 16) |
           ((uint32_t(sse_cvtt(rgba[1])) & 255) << 8) | (uint32_t(sse_cvtt(rgba[2])) & 255);
#endif
}
} // namespace hot
