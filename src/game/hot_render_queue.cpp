// Native sorting of the render queue using the guest CRT partition order.
// Keep key comparisons, equal-key ordering, guest scratch and register results.
#include "game/hot_ordered.h"
using namespace hot;
X86_EXTERN_FUNC(__imp__sub_000A2080);
X86_EXTERN_FUNC(__imp__sub_000A20F0);
namespace {
bool Overlap(uint32_t a, uint64_t n, uint32_t b, uint64_t m) {
    return uint64_t(a) < uint64_t(b) + m && uint64_t(b) < uint64_t(a) + n;
}
// The comparator subtracts the bytes modulo 256 before sign extension.
// It is deliberately not a conventional lexicographic comparison.
inline void Compare(uint8_t *base, uint32_t a, uint32_t b, uint32_t &eax, uint32_t &ecx, uint32_t &edx) {
    ecx = a;
    edx = b;
    const int8_t d = int8_t(uint8_t(X86_MEM8(a + 6) - X86_MEM8(b + 6)));
    if (d)
        eax = uint32_t(int32_t(d));
    else {
        ecx = X86_MEM16(b);
        eax = uint32_t(X86_MEM16(a)) - ecx;
    }
}
static void SmallSort(X86Context &ctx, uint8_t *base) {
    const uint32_t e = ctx.esp, low = X86_MEM32(e + 4), size = X86_MEM32(e + 8), high = ctx.eax;
    if (size != 8 || X86_MEM32(e + 12) != 0x156D80 || high < low || ((high - low) & 7) ||
        Overlap(low, uint64_t(high) - low + 8, e - 28, 44)) {
        __imp__sub_000A2080(ctx, base);
        return;
    }
    uint32_t eax = high, ecx = low, edx = ctx.edx, ebx = ctx.ebx, esi = ctx.esi, edi = high;
    X86_MEM32(e - 4) = ctx.ebp;
    X86_MEM32(e - 8) = ctx.edi;
    if (edi > ecx) {
        X86_MEM32(e - 12) = ebx;
        X86_MEM32(e - 16) = esi;
        do {
            esi = ecx + 8;
            ebx = ecx;
            if (esi <= edi) {
                do {
                    X86_MEM32(e - 20) = ebx;
                    X86_MEM32(e - 24) = esi;
                    X86_MEM32(e - 28) = 0xA20A6;
                    ctx.last_icall = 0x156D80;
                    Compare(base, esi, ebx, eax, ecx, edx);
                    if (int32_t(eax) > 0)
                        ebx = esi;
                    esi += 8;
                } while (esi <= edi);
                ecx = X86_MEM32(e + 4);
            }
            eax = edi;
            if (ebx != edi) {
                ecx = ebx - edi;
                // The arrays cannot overlap the frame; swap all eight bytes together.
                uint64_t a, b;
                std::memcpy(&a, base + edi, 8);
                std::memcpy(&b, base + ebx, 8);
                std::memcpy(base + ebx, &a, 8);
                std::memcpy(base + edi, &b, 8);
                ebx = (ebx & 0xffffff00u) | uint8_t(a >> 56);
                edx = (edx & 0xffffff00u) | uint8_t(b >> 56);
                eax += 8;
                esi = 0;
                ecx = X86_MEM32(e + 4);
            }
            edi -= 8;
        } while (edi > ecx);
        ctx.esi = X86_MEM32(e - 16);
        ctx.ebx = X86_MEM32(e - 12);
    }
    ctx.eax = eax;
    ctx.ecx = ecx;
    ctx.edx = edx;
    ctx.edi = X86_MEM32(e - 8);
    ctx.ebp = X86_MEM32(e - 4);
    ctx.esp = e + 4;
}
// Same median-of-three partitions and selection-sort cutoff as the guest CRT.
// Specialize only BLACK's eight-byte render keys; other qsort users stay original.
static void Sort(X86Context &ctx, uint8_t *base) {
    const uint32_t e = ctx.esp, count = X86_MEM32(e + 8), low = X86_MEM32(e + 4);
    const uint64_t bytes = uint64_t(count) * 8;
    if (X86_MEM32(e + 12) != 8 || X86_MEM32(e + 16) != 0x156D80 || uint64_t(low) + bytes > 0xffffffffu ||
        Overlap(low, bytes, e - 0x150, 0x164)) {
        __imp__sub_000A20F0(ctx, base);
        return;
    }
    uint32_t eax = count, ecx = ctx.ecx, edx = ctx.edx;
    uint32_t ebx = ctx.ebx, esi = ctx.esi, edi = ctx.edi;
    const uint32_t s = e - 0x110;
    if (count < 2) {
        ctx.eax = count;
        ctx.esp = e + 4;
        return;
    }
    X86_MEM32(e - 0x104) = ctx.ebp;
    X86_MEM32(e - 0x108) = ebx;
    X86_MEM32(e - 0x10c) = esi;
    X86_MEM32(s) = edi;
    ebx = low;
    esi = low + (count - 1) * 8;
    X86_MEM32(s + 0x18) = 0;
    X86_MEM32(s + 0x10) = ebx;
    X86_MEM32(s + 0x14) = esi;
    auto compare = [&](uint32_t a, uint32_t b, uint32_t ret) {
        X86_MEM32(s - 4) = b;
        X86_MEM32(s - 8) = a;
        X86_MEM32(s - 12) = ret;
        ctx.last_icall = 0x156D80;
        Compare(base, a, b, eax, ecx, edx);
    };
    auto swap = [&](uint32_t a, uint32_t b) {
        uint64_t av, bv;
        std::memcpy(&av, base + a, 8);
        std::memcpy(&bv, base + b, 8);
        std::memcpy(base + a, &bv, 8);
        std::memcpy(base + b, &av, 8);
        ecx = a - b;
        eax = b + 8;
        esi = 0;
        ebx = (ebx & 0xffffff00u) | uint8_t(bv >> 56);
        edx = (edx & 0xffffff00u) | uint8_t(av >> 56);
    };
partition:
    eax = (esi - ebx) / 8 + 1;
    edx = 0;
    if (eax <= 8) {
        X86_MEM32(s - 4) = 0x156D80;
        X86_MEM32(s - 8) = 8;
        X86_MEM32(s - 12) = ebx;
        X86_MEM32(s - 16) = 0xA2154;
        ctx.esp = s - 16;
        ctx.eax = esi;
        ctx.ecx = ecx;
        ctx.edx = edx;
        ctx.ebx = ebx;
        ctx.esi = esi;
        ctx.edi = edi;
        ctx.ebp = 8;
        SmallSort(ctx, base);
        eax = ctx.eax;
        ecx = ctx.ecx;
        edx = ctx.edx;
        ebx = ctx.ebx;
        esi = ctx.esi;
        edi = ctx.edi;
        goto pop;
    }
    edi = ebx + (eax / 2) * 8;
    compare(ebx, edi, 0xA2191);
    if (int32_t(eax) > 0 && ebx != edi) {
        swap(ebx, edi);
        ebx = X86_MEM32(s + 0x10);
        esi = X86_MEM32(s + 0x14);
    }
    compare(ebx, esi, 0xA21C3);
    if (int32_t(eax) > 0 && ebx != esi) {
        swap(ebx, esi);
        ebx = X86_MEM32(s + 0x10);
        esi = X86_MEM32(s + 0x14);
    }
    compare(edi, esi, 0xA21F5);
    if (int32_t(eax) > 0 && edi != esi) {
        swap(edi, esi);
        ebx = X86_MEM32(s + 0x10);
    }
    esi = X86_MEM32(s + 0x14);
scan:
    if (edi > ebx) {
    left_before_pivot:
        ebx += 8;
        if (ebx < edi) {
            compare(ebx, edi, 0xA2233);
            if (int32_t(eax) <= 0)
                goto left_before_pivot;
            if (edi > ebx)
                goto right;
        }
    }
left_after_pivot:
    eax = X86_MEM32(s + 0x14);
    ebx += 8;
    if (ebx <= eax) {
        compare(ebx, edi, 0xA2253);
        if (int32_t(eax) <= 0)
            goto left_after_pivot;
    }
right:
    esi -= 8;
    if (esi > edi) {
        compare(esi, edi, 0xA226F);
        if (int32_t(eax) > 0)
            goto right;
    }
    if (ebx > esi)
        goto split;
    eax = esi;
    if (ebx != esi) {
        uint64_t av, bv;
        std::memcpy(&av, base + ebx, 8);
        std::memcpy(&bv, base + esi, 8);
        std::memcpy(base + ebx, &bv, 8);
        std::memcpy(base + esi, &av, 8);
        X86_MEM32(s + 0x1c) = 0;
        ecx = 0;
        edx = (edx & 0xffffff00u) | uint8_t(bv >> 56);
        eax += 8;
    }
    if (edi == esi)
        edi = ebx;
    goto scan;
split:
    esi += 8;
    if (edi < esi) {
    strip_before_pivot:
        esi -= 8;
        if (esi > edi) {
            compare(esi, edi, 0xA22D1);
            if (!eax)
                goto strip_before_pivot;
            if (edi < esi)
                goto choose;
        }
    }
strip_after_pivot:
    eax = X86_MEM32(s + 0x10);
    esi -= 8;
    if (esi > eax) {
        compare(esi, edi, 0xA22F3);
        if (!eax)
            goto strip_after_pivot;
    }
choose:
    eax = X86_MEM32(s + 0x10);
    edx = X86_MEM32(s + 0x14);
    ecx = edx - ebx;
    edi = esi - eax;
    if (int32_t(edi) >= int32_t(ecx)) {
        if (eax < esi) {
            ecx = X86_MEM32(s + 0x18);
            X86_MEM32(s + 0x20 + ecx * 4) = eax;
            X86_MEM32(s + 0x98 + ecx * 4) = esi;
            X86_MEM32(s + 0x18) = ++ecx;
        }
        if (ebx < edx) {
            esi = X86_MEM32(s + 0x14);
            X86_MEM32(s + 0x10) = ebx;
            goto partition;
        }
    } else {
        if (ebx < edx) {
            ecx = X86_MEM32(s + 0x18);
            X86_MEM32(s + 0x20 + ecx * 4) = ebx;
            X86_MEM32(s + 0x98 + ecx * 4) = edx;
            X86_MEM32(s + 0x18) = ++ecx;
        }
        if (eax < esi) {
            ebx = X86_MEM32(s + 0x10);
            X86_MEM32(s + 0x14) = esi;
            goto partition;
        }
    }
pop:
    eax = X86_MEM32(s + 0x18) - 1;
    X86_MEM32(s + 0x18) = eax;
    if (int32_t(eax) >= 0) {
        edx = X86_MEM32(s + 0x20 + eax * 4);
        eax = X86_MEM32(s + 0x98 + eax * 4);
        X86_MEM32(s + 0x10) = edx;
        X86_MEM32(s + 0x14) = eax;
        ebx = edx;
        esi = eax;
        goto partition;
    }
    ctx.eax = eax;
    ctx.ecx = ecx;
    ctx.edx = edx;
    ctx.edi = X86_MEM32(s);
    ctx.esi = X86_MEM32(e - 0x10c);
    ctx.ebx = X86_MEM32(e - 0x108);
    ctx.ebp = X86_MEM32(e - 0x104);
    ctx.esp = e + 4;
}

} // namespace

X86_FUNC(sub_000A2080) { SmallSort(ctx, base); }

X86_FUNC(sub_000A20F0) { Sort(ctx, base); }
