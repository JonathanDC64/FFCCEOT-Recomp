#include "gx_match.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace ntr::gx {

int param_count(uint8_t op) {
    switch (op) {
    case 0x10: case 0x12: case 0x13: case 0x14: return 1;
    case 0x11: case 0x15: return 0;
    case 0x16: case 0x18: return 16;
    case 0x17: case 0x19: return 12;
    case 0x1A: return 9;
    case 0x1B: case 0x1C: return 3;
    case 0x20: case 0x21: case 0x22: return 1;
    case 0x23: return 2;
    case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: return 1;
    case 0x30: case 0x31: case 0x32: case 0x33: return 1;
    case 0x34: return 32;
    case 0x40: return 1;
    case 0x41: return 0;
    case 0x50: case 0x60: return 1;
    case 0x70: return 3;
    case 0x71: return 2;
    case 0x72: return 1;
    default: return 0;
    }
}

std::vector<Chunk> chunks(const std::vector<Entry>& e) {
    std::vector<Chunk> out;
    uint8_t prev_op = 0;
    uint64_t h = 0;
    for (size_t i = 0; i < e.size();) {
        const uint8_t op = e[i].op;
        const size_t n = std::min<size_t>(entry_count(op), e.size() - i);
        const bool starts = op == 0x12 || op == 0x14 || op == 0x15 || op == 0x16 || op == 0x17;
        if (out.empty() || (starts && !is_matrix_op(prev_op))) {
            out.push_back({0, i, 0});
            h = 1469598103934665603ull;
        }
        const auto mix = [&](uint64_t v) {
            h ^= v;
            h *= 1099511628211ull;
        };
        mix(op);
        if (op == 0x10 || op == 0x29 || op == 0x2A || op == 0x2B) mix(e[i].param);
        out.back().key = h;
        out.back().entries += n;
        prev_op = op;
        i += n;
    }
    return out;
}

std::vector<std::pair<size_t, size_t>> align(const std::vector<Chunk>& a, const std::vector<Chunk>& b) {
    const size_t n = a.size(), m = b.size();
    std::vector<std::pair<size_t, size_t>> out;
    if (n == 0 || m == 0 || n * m > 16'000'000) return out;
    std::vector<uint32_t> dp((n + 1) * (m + 1), 0);  // dp[i][j]: entries matched in a[i..], b[j..]
    const auto at = [&](size_t i, size_t j) -> uint32_t& { return dp[i * (m + 1) + j]; };
    for (size_t i = n; i-- > 0;)
        for (size_t j = m; j-- > 0;)
            at(i, j) = a[i].key == b[j].key ? at(i + 1, j + 1) + uint32_t(b[j].entries)
                                            : std::max(at(i + 1, j), at(i, j + 1));
    for (size_t i = 0, j = 0; i < n && j < m;) {
        if (a[i].key == b[j].key && at(i, j) == at(i + 1, j + 1) + b[j].entries) {
            out.push_back({i, j});
            ++i, ++j;
        } else if (at(i + 1, j) >= at(i, j + 1)) {
            ++i;
        } else {
            ++j;
        }
    }
    return out;
}
bool same_opcodes(const std::vector<Entry>& a, const std::vector<Entry>& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](const Entry& x, const Entry& y) { return x.op == y.op; });
}

namespace {

// Matrix parameters are 1.19.12 fixed point. A matrix command whose parameters move further than this between two
// scenes jumped rather than moved (a camera cut, a teleport): it is not blended.
constexpr int32_t kJumpLinear = 0x800;         // 0.5 in a rotation/scale element
constexpr int32_t kJumpTranslation = 0x10000;  // 16 units

// Whether the matrix command starting at a[ia] / b[ib] jumped.
bool jumped(const std::vector<Entry>& a, size_t ia, const std::vector<Entry>& b, size_t ib) {
    const uint8_t op = b[ib].op;
    const int n = entry_count(op);
    for (int k = 0; k < n; k++) {
        const int64_t d = int64_t(int32_t(b[ib + k].param)) - int32_t(a[ia + k].param);
        // The translation row: the last three parameters of 4x4 (before w) and 4x3 loads/multiplies, and translate.
        const bool translation = op == 0x1C || ((op == 0x17 || op == 0x19) && k >= 9) ||
                                 ((op == 0x16 || op == 0x18) && k >= 12 && k < 15);
        if (std::abs(d) > (translation ? kJumpTranslation : kJumpLinear)) return true;
    }
    return false;
}

// The camera: the first matrix load in position mode (the projection matrix stays fixed in this game).
size_t camera_load(const std::vector<Entry>& e) {
    uint32_t mode = 0;
    for (size_t i = 0; i < e.size(); i += entry_count(e[i].op)) {
        if (e[i].op == 0x10) mode = e[i].param & 3;
        if ((e[i].op == 0x16 || e[i].op == 0x17) && (mode == 1 || mode == 2)) return i;
    }
    return SIZE_MAX;
}

}  // namespace

Blend blend(const std::vector<Entry>& a, const std::vector<Entry>& b, std::vector<Entry>& out, int num, int den) {
    // A camera cut: show the new scene as it is.
    const size_t cam_a = camera_load(a), cam_b = camera_load(b);
    if (cam_a != SIZE_MAX && cam_b != SIZE_MAX && b[cam_b].op == a[cam_a].op &&
        cam_b + entry_count(b[cam_b].op) <= b.size() &&
        cam_a + entry_count(a[cam_a].op) <= a.size() && jumped(a, cam_a, b, cam_b)) {
        return {0, true};
    }
    const auto lerp = [&](uint32_t x, uint32_t y) {
        const int64_t from = int32_t(y), to = int32_t(x);  // from the new scene back towards the old one
        return uint32_t(int32_t(from + (to - from) * (den - num) / den));
    };
    size_t blended = 0;
    // Blends the matrix commands of a run of commands that is the same in both scenes.
    const auto blend_range = [&](size_t ia, size_t ib, size_t n) {
        for (size_t k = 0; k < n;) {
            const uint8_t op = out[ib + k].op;
            const size_t len = std::min<size_t>(entry_count(op), n - k);
            if (is_matrix_param_op(op) && len == size_t(entry_count(op)) && !jumped(a, ia + k, out, ib + k)) {
                for (size_t p = 0; p < len; p++) {
                    const uint32_t x = a[ia + k + p].param;
                    Entry& y = out[ib + k + p];
                    if (x == y.param) continue;
                    y.param = lerp(x, y.param);
                    ++blended;
                }
            }
            k += len;
        }
    };
    if (same_opcodes(a, b)) {
        blend_range(0, 0, b.size());
        return {blended, false};
    }
    const std::vector<Chunk> ca = chunks(a), cb = chunks(b);
    for (const auto& [i, j] : align(ca, cb))
        if (ca[i].entries == cb[j].entries) blend_range(ca[i].first, cb[j].first, cb[j].entries);
    return {blended, false};
}

}  // namespace ntr::gx
