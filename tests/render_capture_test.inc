// Included after the capture methods extracted from render_thread.cpp.
int main() {
    State glow_state{};
    PixelKey glow_key{};
    TextureDesc glow_texture{};
    glow_state.simple[kMBlendEnable >> 2] = 1;
    glow_state.simple[kMSrcBlend >> 2] = 1;
    glow_state.simple[kMDestBlend >> 2] = 0x302;
    glow_state.simple[kMBlendOp >> 2] = 0x8006;
    glow_state.simple[kMPsFinalConstant0 >> 2] = 0xFF000000;
    glow_key.programmable = 1; glow_key.count = 1; glow_key.tex_modes = 1;
    glow_key.rgb_in[0] = 0xC8C10000; glow_key.rgb_out[0] = 0xC0;
    glow_key.final_abcd = 0xC00; glow_key.final_efg = 0x1180;
    glow_texture.format = 0x12; glow_texture.width = 160; glow_texture.height = 120;
    if (!IsGlowComposite(glow_state, glow_key, glow_texture, 5, 3, 640, 480, 640, 480, true)) std::abort();
    for (unsigned t = 0; t < 24; ++t) {
        State s = glow_state; PixelKey pk = glow_key; TextureDesc texture = glow_texture;
        uint32_t prim = 5, count = 3, rw = 640, rh = 480, sw = 640, sh = 480;
        bool rt = true;
        switch (t) {
            case 0: prim = 4; break;
            case 1: count = 6; break;
            case 2: rw = 160; break;
            case 3: rh = 120; break;
            case 4: s.rs[kRsZEnable] = 1; break;
            case 5: pk.programmable = 0; break;
            case 6: pk.count = 2; break;
            case 7: pk.tex_modes = 2; break;
            case 8: pk.alphakill = 1; break;
            case 9: pk.rgb_in[0] ^= 1; break;
            case 10: pk.rgb_out[0] ^= 1; break;
            case 11: pk.final_abcd ^= 1; break;
            case 12: pk.final_efg ^= 1; break;
            case 13: s.simple[kMBlendEnable >> 2] = 0; break;
            case 14: s.simple[kMBlendOp >> 2] = 0x800A; break;
            case 15: rt = false; break;
            case 16: s.simple[kMPsFinalConstant0 >> 2] = 0x7F000000; break;
            case 17: s.textures[1] = 1; break;
            case 18: texture.format = 0x06; break;
            case 19: texture.width = 320; break;
            case 20: sw = 0; sh = 0; break;
            case 21: s.simple[kMBlendOp >> 2] = 0x800B; break;
            case 22: s.simple[kMBlendOp >> 2] = 0x8007; break;
            case 23: s.simple[kMBlendOp >> 2] = 0x8008; break;
        }
        if (IsGlowComposite(s, pk, texture, prim, count, rw, rh, sw, sh, rt)) std::abort();
    }
    for (float part : {1.0f, 0.9f, 0.75f}) {
        float texsize[] = {160, 120, -0.5f / 160, -0.5f / 120};
        const float before_x = texsize[2], before_y = texsize[3];
        AlignGlow(texsize, part, part, 640, 480);
        if (std::abs((texsize[2] - before_x) / part * 640 - 7.38f) > 0.0001f ||
            std::abs((texsize[3] - before_y) / part * 480 - 10.77f) > 0.0001f ||
            texsize[0] != 160 || texsize[1] != 120) std::abort();
    }
    std::puts("PASS final glow pass isolation and resolution-independent sample alignment");
    // Recompiled projection code reads the device fields; shaders read State.
    // Both must use the same convention after startup and offset changes.
    g_state.viewport = {0, 0, 160, 120, 0.0f, 1.0f};
    X86Context ctx;
    for (const auto offset : {0.0f, 0.53125f, -0.5f, 1.25f, -8.0f}) {
        const float xy[] = {offset, -offset};
        uint32_t args[2]; std::memcpy(args, xy, sizeof(args));
        D3DDevice_SetScreenSpaceOffset(args);
        if (std::memcmp(HostPtr(kDevice + XDK_CDevice_ScreenSpaceOffset), xy, sizeof(xy))) std::abort();
        if (g_state.vsc[kViewportOffset][0] != 80.0f + offset ||
            g_state.vsc[kViewportOffset][1] != 60.0f - offset) std::abort();
        // Simulate stale fields left by the recompiled device initialization.
        W32(kDevice + XDK_CDevice_ScreenSpaceOffset, 0x12345678);
        W32(kDevice + XDK_CDevice_ScreenSpaceOffset + 4, 0x87654321);
        StoreViewport(ctx, nullptr);
        if (std::memcmp(HostPtr(kDevice + XDK_CDevice_ScreenSpaceOffset), xy, sizeof(xy))) std::abort();
        if (R32(kDevice + XDK_CDevice_Viewport + 8) != 160 ||
            R32(kDevice + XDK_CDevice_Viewport + 12) != 120) std::abort();
    }
    g_state = {};
    std::puts("PASS guest projection fields and renderer screen offsets agree after initialization and updates");
    Capture c;
    State current{}, replay{};
    auto ranges = StateRanges();
    auto fixed = FixedRanges();
    ranges.insert(ranges.end(), fixed.begin(), fixed.end());
    std::mt19937 rng(616703);
    std::vector<uint8_t> payload(2048);
    for (unsigned t = 0; t < 12000; ++t) {
        for (unsigned j = 0; j < t % 23; ++j) {
            auto r = ranges[rng() % ranges.size()];
            reinterpret_cast<uint8_t*>(&current)[r.off + rng() % r.len] = uint8_t(rng());
        }
        c.batch.bytes.clear();
        for (const auto& r : ranges) c.SendRange(reinterpret_cast<const uint8_t*>(&current), r);
        const auto& bytes = c.batch.bytes;
        for (size_t at = 0; at < bytes.size();) {
            if (bytes.size() - at < 8) std::abort();
            uint32_t h[2]; std::memcpy(h, bytes.data() + at, 8); at += 8;
            if (!h[1] || uint64_t(h[0]) + h[1] > sizeof(State) || h[1] > bytes.size() - at) std::abort();
            std::memcpy(reinterpret_cast<uint8_t*>(&replay) + h[0], bytes.data() + at, h[1]); at += h[1];
        }
        for (auto r : ranges)
            if (std::memcmp(reinterpret_cast<uint8_t*>(&current) + r.off,
                            reinterpret_cast<uint8_t*>(&replay) + r.off, r.len)) std::abort();
        const size_t recorded = c.batch.bytes.size();
        for (const auto& r : ranges) c.SendRange(reinterpret_cast<const uint8_t*>(&current), r);
        if (c.batch.bytes.size() != recorded) std::abort();
        for (auto& v : payload) v = uint8_t(rng());
        const size_t n = t % payload.size();
        const uint32_t at = c.Put(payload.data(), n);
        if ((at & 3) || std::memcmp(c.batch.bytes.data() + at, payload.data(), n)) std::abort();
    }
    for (auto& v : guest) v = uint8_t(rng());
    SyncDeferred();
    for (unsigned i = 0; i < g_state.rs.size(); ++i) {
        const uint32_t expected = i >= kRsDeferredFirst && i < kRsComplexFirst ? R32(i * 4) : 0;
        if (g_state.rs[i] != expected) std::abort();
    }
    for (unsigned stage = 0; stage < 4; ++stage) for (unsigned i = 0; i < g_state.tss[stage].size(); ++i) {
        const uint32_t expected = i < kTssBumpEnvMat00 ? R32(kTextureState + (stage * kStageStates + i) * 4) : 0;
        if (g_state.tss[stage][i] != expected) std::abort();
    }
    for (uint32_t reg : {0u, 1u, 188u, 189u, 190u, 191u, 192u, 193u, 0xffffffffu}) {
        for (uint32_t words : {0u, 1u, 3u, 4u, 5u, 16u, 128u, 1024u}) {
            for (unsigned fixed = 0; fixed < 2; ++fixed) {
                for (auto& v : g_state.vsc) for (auto& f : v) f = 0;
                State expected = g_state;
                const uint32_t args[] = {reg, 8192, words};
                for (uint32_t i = 0; i < (fixed ? 4 : words / 4) && uint64_t(reg) + i < kVsConstants; ++i)
                    std::memcpy(expected.vsc[reg + i].data(), HostPtr(8192 + i * 16), 16);
                if (fixed) D3DDevice_SetVertexShaderConstant4(args);
                else D3DDevice_SetVertexShaderConstantNotInlineFast(args);
                if (std::memcmp(expected.vsc.data(), g_state.vsc.data(), sizeof(g_state.vsc))) std::abort();
            }
        }
    }
    std::puts("PASS deferred-state boundaries and shader-constant bounds, DWORD counts and invalid registers");
    HeaderCapture hc;
    if (hc.Header(0)) std::abort();
    for (unsigned t = 0; t < 40000; ++t) {
        const uint32_t p = 0x8000 + (rng() % 32) * 32;
        const uint32_t type = (rng() % 8) << 16;
        std::memcpy(HostPtr(p), &type, 4);
        for (unsigned i = 4; i < 20; ++i) guest[p + i] = uint8_t(rng());
        auto snapshot = [&](uint32_t at) {
            const unsigned size = type < 0x20000 || type == 0x30000 ? 12 : 20;
            if (std::memcmp(HostPtr(at), HostPtr(p), size)) std::abort();
            for (unsigned i = size; i < 20; ++i) if (guest[at + i]) std::abort();
        };
        hc.batch.id = t + 1;
        hc.Use(p);
        if (hc.last_use_[p] != t + 1) std::abort();
        hc.Use(0);
        if (hc.last_use_.contains(0)) std::abort();
        const uint32_t copied = hc.Header(p);
        snapshot(copied);
        if (hc.Header(p) != copied) std::abort();
        if (type < 0x20000 || type == 0x30000) {
            guest[p + 12] ^= 0xff;
            if (hc.Header(p) != copied) std::abort();
        }
        if (t % 1000 == 0) {
            hc.ring_pos_ += kHeaderRing / 2;
            const auto n = hc.header_copies_;
            snapshot(hc.Header(p));
            if (hc.header_copies_ != n + 1) std::abort();
        }
    }
    hc.ring_ = 0;
    if (hc.Header(0xffffffffu) != 0xffffffffu) std::abort();
    std::puts("PASS 40000 resource snapshots: short headers, mutations, cache collisions and ring expiration");
    std::puts("PASS 12000 capture/replay states: sparse/dense changes, unchanged draws, partial pieces, aligned payloads");
#ifdef CAPTURE_BENCH
    {
        HeaderCapture headers;
        for (unsigned i = 0; i < 64; ++i) {
            const uint32_t key = 0x180000 + i * 32, type = 0x40000;
            std::memcpy(HostPtr(key), &type, 4);
            for (unsigned j = 4; j < 20; ++j) guest[key + j] = uint8_t(rng());
        }
        auto start = std::chrono::steady_clock::now();
        constexpr unsigned count = 2000000;
        uint32_t sink = 0;
        for (unsigned i = 0; i < count; ++i) {
            const uint32_t key = 0x180000 + ((i * 7) & 63) * 32;
            if (i % 127 == 0) guest[key + 8] ^= 1;
            sink ^= headers.Header(key);
            headers.Use(key);
        }
        std::printf("BENCH resource header capture %.2f ns/lookup, check %x (host/qemu only)\n",
            std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / count, sink);
    }
    for (unsigned density : {1u, 32u}) {
        auto start = std::chrono::steady_clock::now();
        constexpr unsigned count = 50000;
        for (unsigned i = 0; i < count; ++i) {
            c.batch.bytes.clear();
            for (unsigned j = 0; j < density; ++j) current.vsc[j][0] = float(i);
            for (const auto& r : ranges) c.SendRange(reinterpret_cast<const uint8_t*>(&current), r);
        }
        std::printf("BENCH capture %u changed vectors %.2f ns/draw (host/qemu only)\n", density,
            std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / count);
    }
#endif
}
