# Switch port development

The port translates `default.xbe` to C++, compiles it for Cortex-A57, and links
it with the Xbox runtime and deko3d renderer as a native Horizon NRO. See the
[README](../README.md) for dependencies, game data, controls and build commands.

## Source layout

| Path | Purpose |
|---|---|
| `switch/recomp.sh` | Host recompiler and generated game C++ |
| `switch/build.sh` | Playable NRO; threaded rendering enabled by default |
| `switch/build-headless.sh` | Renderer-free game-thread benchmark |
| `switch/build-uam.sh` | Patched GLSL-to-DKSH compiler library |
| `switch/headless/` | Horizon entry point, HID, audio, clocks and diagnostics |
| `switch/deko3d/` | NV2A shader translation, shader cache and GPU backend |
| `src/game/hot_*` | Native replacements and verification helpers |

The SDK revision is pinned in `CMakeLists.txt`. Regenerate the game C++ after
changing the SDK recompiler or game configuration. Build caches distinguish
compile flags and header content. Generated code uses `-ffp-contract=off` to
preserve floating-point operation order.

## Runtime and renderer

Guest memory uses process-code-memory aliases when loader permissions allow,
with a CPU-only fallback. File I/O uses bounce buffers where Horizon services
cannot access guest memory directly. The renderer can read eligible vertices
from GPU-mappable guest RAM when the loader provides a suitable mapping.

Guest render commands are captured and processed by a render thread. Main targets
scale up to 1280x720 with dynamic resolution; smaller glow and shadow targets keep
their guest sizes. Depth-alias copies preserve depth for effects such as streetlight
shafts. Translated shaders are compiled with UAM and cached under
`sdmc:/switch/black/shaders`.

The handheld APM profile is `0x92220007`; docked mode uses Horizon's policy.
Actual clock rates are logged because system overrides can replace the request.
Loading-only CPU FastLoad is unsuitable for gameplay because it throttles the GPU.

The render thread's capture compares the title's render state against the copy
last sent, 64 bytes at a time, on every draw. The HLE marks the vertex shader
constant and render/texture stage state pieces it writes (`src/d3d/vsc_dirty.h`),
so only those are compared; `BLACK_VSC_MASK=0` compares every piece as before and
`BLACK_CAPTURE_CHECK=1` compares the clean pieces too and logs any that differ (a
writer the mask misses). Immediate-mode vertices (Begin/End) are packed as the
draw takes them while the push buffer is walked; `BLACK_IM_CHECK=1` also builds
the earlier every-slot form and logs primitives whose packing differs. On the
console the D3D HLE takes no mutex per call: every caller is the title's thread
(the hooks included), and a call from any other thread is logged.

## Performance work

Particles, matrices, collision and culling already emit NEON through vector types
and ordered arithmetic helpers. YUV conversion and index-range scans also use NEON.
The video IDCT remains scalar and is a candidate for exact integer SIMD; it mainly
benefits films. ADPCM's predictor is sequential, although its final conversion can
potentially be vectorized. Measure render-state copies, batching and GPU passes
before assuming another SIMD rewrite will improve combat frame times.

Native replacements must preserve guest registers, memory effects, intermediate
rounding and arithmetic order. Avoid blanket fast-math, FMA contraction and
approximate reciprocals. `EXTRA_CXXFLAGS=-DBLACK_VERIFY_HOT` enables the native
verification harness; codegen differential tests are in the SDK's
`harness/recomp_fuzz`. NaN canonicalization and excluded trials limit what those
tests prove.
