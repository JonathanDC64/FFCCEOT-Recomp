// Matching and blending GX command streams (frame interpolation, see gx_stream.hpp). No melonDS dependency, so it is
// unit tested on its own (tests/test_gx.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ntr::gx {

// One GX command FIFO entry. A command with n parameters takes n entries (at least one), all with its opcode.
struct Entry {
    uint8_t op;
    uint32_t param;
};

// Parameters each GX command takes (0 for unknown opcodes).
int param_count(uint8_t op);
inline int entry_count(uint8_t op) { return param_count(op) > 1 ? param_count(op) : 1; }
inline bool is_matrix_op(uint8_t op) { return op >= 0x10 && op <= 0x1C; }
// Commands that load or multiply a matrix with parameters from the stream (what interpolation blends).
inline bool is_matrix_param_op(uint8_t op) { return op >= 0x16 && op <= 0x1C; }
// Vertex data: colour, normal, texture coordinates, positions.
inline bool is_vertex_op(uint8_t op) { return op >= 0x20 && op <= 0x28; }

bool same_opcodes(const std::vector<Entry>& a, const std::vector<Entry>& b);

// A run of commands drawn with one matrix setup: it starts where the stream sets the matrix mode or loads or restores
// a matrix after something that was not matrix work. Its key covers the opcodes and the polygon/texture attributes
// (not the alpha or polygon ID, which change as an object fades or as others come and go before it), so the same
// object drawn in two scenes gets the same key even when its matrices (or vertices) moved.
struct Chunk {
    uint64_t key = 0;
    size_t first = 0, entries = 0;
};
std::vector<Chunk> chunks(const std::vector<Entry>& e);

// Chunks of `a` and `b` that line up: the longest common subsequence of their keys (weighted by size), as pairs of
// indices. Empty when the scenes are too large to align.
std::vector<std::pair<size_t, size_t>> align(const std::vector<Chunk>& a, const std::vector<Chunk>& b);

// Blends the matrix parameters of `out` (a copy of `b`) back towards `a` by (den - num) / den, where the two streams
// match (the same opcodes, or aligned chunks). Object matrices (position mode, other than the camera) have their 3x3
// part blended as a rotation along the shortest arc plus a linear scale/shear; everything else is blended
// element-wise. Matrix commands that jumped are left as they are in `b`; if the camera jumped, nothing is blended and
// `cut` is set.
struct Blend {
    size_t blended = 0;  // parameters changed (0: nothing to interpolate)
    size_t snapped = 0;  // matrix commands that jumped, left as they are in `b`
    bool cut = false;
};
Blend blend(const std::vector<Entry>& a, const std::vector<Entry>& b, std::vector<Entry>& out, int num, int den);

}  // namespace ntr::gx
