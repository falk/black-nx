// The render state the title wrote since the render thread's capture last looked at it.
//
// The capture (src/d3d/render_thread.cpp SendState) compares the State against the copy it last sent, 64 bytes at a
// time, on every draw: about 5 KB, of which the 192 vertex shader constant registers are 3 KB and the render and
// texture stage states 1.2 KB. The HLE (src/d3d/d3d8.cpp) is the only writer of both, so it marks the pieces it
// writes here, one bit per 64 byte piece of the range, and the capture compares only those: a clear bit means the
// piece equals the sent copy (equal when last compared, unwritten since). Only the title's thread reads and writes
// the masks, as it alone runs the HLE and the capture.
#pragma once
#include <cstddef>
#include <cstdint>

namespace d3d {

constexpr uint32_t kVscRegisters = 192;
constexpr uint32_t kVscPieces = kVscRegisters / 4;  // 64 byte pieces of State::vsc
constexpr uint32_t kDirtyPiece = 64;

// all set until the capture first runs: nothing is assumed about the copy it starts from
inline uint64_t g_vsc_dirty = ~0ull;  // State::vsc, by register / 4
inline uint64_t g_rs_dirty = ~0ull;   // State::rs through State::tss, by 64 byte piece from the start of rs

inline void DirtyVsc(uint32_t reg, uint32_t count) {
    if (!count || reg >= kVscRegisters) return;
    uint32_t last = reg + count - 1;
    if (last >= kVscRegisters) last = kVscRegisters - 1;
    for (uint32_t p = reg >> 2; p <= last >> 2; p++) g_vsc_dirty |= 1ull << p;
}

// bytes [p, p + n) of the range starting at `range`, in the mask with a bit per piece
inline void DirtyBytes(uint64_t& mask, const void* range, const void* p, size_t n) {
    if (!n) return;
    size_t off = size_t(static_cast<const uint8_t*>(p) - static_cast<const uint8_t*>(range));
    size_t first = off / kDirtyPiece, last = (off + n - 1) / kDirtyPiece;
    if (last > 63) last = 63;
    for (size_t i = first; i <= last; i++) mask |= 1ull << i;
}

}  // namespace d3d
