#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(__SWITCH__) || defined(BLACK_TEST_NEWLIB)
extern "C" int __ieee754_rem_pio2(double, double *);
extern "C" double __kernel_sin(double, double, int);
extern "C" double __kernel_cos(double, double);
#endif

namespace hot {
struct SinCos {
    double sine, cosine;
};
// The Switch libm sincos wrapper calls sin and cos separately. Use the same
// kernels and reduction once, retaining their double results for the guest x87.
inline SinCos ParticleSinCos(double angle) {
#if defined(__SWITCH__) || defined(BLACK_TEST_NEWLIB)
    uint64_t bits;
    std::memcpy(&bits, &angle, sizeof bits);
    const uint32_t high = uint32_t(bits >> 32) & 0x7fffffffu;
    if (high <= 0x3fe921fbu)
        return {__kernel_sin(angle, 0.0, 0), __kernel_cos(angle, 0.0)};
    if (high >= 0x7ff00000u) {
        const double invalid = angle - angle;
        return {invalid, invalid};
    }
    double reduced[2];
    const int quadrant = __ieee754_rem_pio2(angle, reduced) & 3;
    const double sine = __kernel_sin(reduced[0], reduced[1], 1);
    const double cosine = __kernel_cos(reduced[0], reduced[1]);
    switch (quadrant) {
    case 0:
        return {sine, cosine};
    case 1:
        return {cosine, -sine};
    case 2:
        return {-sine, -cosine};
    default:
        return {-cosine, sine};
    }
#else
    return {std::sin(angle), std::cos(angle)};
#endif
}
} // namespace hot
