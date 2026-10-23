// Explicit BLACK_BENCH_HOT=1 startup experiment. Private guest memory prevents
// fixtures from touching the live game. Normal launches do not run this benchmark.
#if defined(__SWITCH__) && defined(BLACK_ROI_BUILD)
#include "core/log.h"
#include "game/hot_paths.h"
#include <algorithm>
#include <chrono>
#include <vector>
X86_EXTERN_FUNC(sub_0015D320);
X86_EXTERN_FUNC(__imp__sub_0015D320);

namespace game {
bool BenchHotCandidates() {
    std::vector<uint8_t> memory(1u << 24), before(memory.size());
    uint8_t *base = memory.data();
    constexpr uint32_t e = 0xA00200;
    auto setup = [&]() {
        X86_MEM32(e + 8) = 0x800400;
        X86_MEM32(e + 0xC) = 0;
        X86_MEM32(e + 0x14) = 0x800100;
        X86_MEM32(e + 0x18) = 0x800200;
        X86_MEM32(e + 0x1C) = 0x800300;
        X86_MEMF32(0x295C74) = 1;
        X86_MEMF32(0x295CB4) = 255;
        for (unsigned i = 0; i < 4; ++i) {
            X86_MEMF32(0x800100 + i * 4) = .25f;
            X86_MEMF32(0x800200 + i * 4) = i == 3 ? 0 : float(i);
            X86_MEMF32(0x800300 + i * 4) = .5f;
        }
        X86_MEMF32(0x800420) = .75f;
        const uint32_t s = ((e - 4) & ~15u) - 0x80;
        X86_MEM32(s + 0x4C) = 0;
        X86_MEM32(s + 0x5C) = 0;
    };
    auto context = [&] {
        X86Context c{};
        c.esp = e;
        c.fcw = 0x37F;
        c.mxcsr = 0x1F80;
        return c;
    };
    struct Candidate {
        const char *name;
        X86Func *old_fn;
        X86Func *new_fn;
    };
    const Candidate candidates[] = {
        {"particle-billboard", __imp__sub_0015D320, sub_0015D320},
    };
    LOGI("HOT_BENCH: private finite fixtures, 5 alternating rounds; ns/call includes ABI/gates; not a gameplay FPS "
         "measurement");
    for (const Candidate &c : candidates) {
        setup();
        before = memory;
        X86Context old = context(), current = old;
        c.old_fn(old, base);
        const std::vector<uint8_t> expected = memory;
        memory = before;
        base = memory.data();
        c.new_fn(current, base);
        if (std::memcmp(&old, &current, sizeof old) || memory != expected) {
            LOGE("HOT_BENCH: %s fixture mismatch; aborting", c.name);
            return false;
        }
        double ns[2][5];
        constexpr unsigned count = 200000;
        for (int round = 0; round < 5; ++round) {
            for (int step = 0; step < 2; ++step) {
                const int which = step ^ (round & 1);
                setup();
                X86Context state = context();
                X86Func *fn = which ? c.new_fn : c.old_fn;
                auto start = std::chrono::steady_clock::now();
                for (unsigned i = 0; i < count; ++i) {
                    state.esp = e;
                    fn(state, base);
                }
                ns[which][round] =
                    std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / count;
            }
        }
        std::sort(ns[0], ns[0] + 5);
        std::sort(ns[1], ns[1] + 5);
        LOGI("HOT_BENCH: %s original_ns=%.2f native_ns=%.2f saved_pct=%.1f original_range=%.2f..%.2f "
             "native_range=%.2f..%.2f",
             c.name, ns[0][2], ns[1][2], 100 * (1 - ns[1][2] / ns[0][2]), ns[0][0], ns[0][4], ns[1][0], ns[1][4]);
    }
    LOGI("HOT_BENCH: fixtures match; continuing to game");
    return true;
}
} // namespace game
#endif
