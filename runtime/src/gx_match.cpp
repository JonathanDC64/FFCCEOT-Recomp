#include "gx_match.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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

// Whether the matrix command starting at a[ia] / b[ib] jumped. With `outside_3x3`, only the parameters outside its
// 3x3 part are checked (the translation, and the w column of a 4x4), for when that part is blended as a rotation.
bool jumped(const std::vector<Entry>& a, size_t ia, const std::vector<Entry>& b, size_t ib, bool outside_3x3 = false) {
    const uint8_t op = b[ib].op;
    const int n = entry_count(op);
    for (int k = 0; k < n; k++) {
        const bool in_3x3 = (op == 0x16 || op == 0x18) ? k < 12 && k % 4 != 3 : (op >= 0x17 && op <= 0x1A) && k < 9;
        if (outside_3x3 && in_3x3) continue;
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

// Rotations are blended along the shortest arc (element-wise blending shrinks a turning object: by 3.4% at the
// midpoint of a 30-degree turn). A turn further than this between two scenes is a snap, not a turn: near 180 degrees
// the direction is ambiguous anyway.
constexpr double kJumpAngle = 120.0 * 3.14159265358979323846 / 180.0;
// The stretch part of a matrix (scale and shear) jumped if an element moved further than this.
constexpr double kJumpStretch = 0.5;

using Mat3 = std::array<std::array<double, 3>, 3>;

double det(const Mat3& m) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            for (int k = 0; k < 3; k++) r[i][j] += a[i][k] * b[k][j];
    return r;
}

Mat3 transpose(const Mat3& m) {
    Mat3 r{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r[i][j] = m[j][i];
    return r;
}

// The rotation of a matrix: the orthogonal factor of its polar decomposition, by Newton's iteration
// Q <- (Q + Q^-T) / 2. False for a degenerate matrix (an object scaled to nothing).
bool rotation_of(const Mat3& m, Mat3& q) {
    if (std::abs(det(m)) < 1e-9) return false;
    q = m;
    for (int it = 0; it < 30; it++) {
        const double d = det(q);
        if (std::abs(d) < 1e-12) return false;
        Mat3 inv_t{};  // the inverse transposed: the cofactor matrix over the determinant
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                const int i1 = (i + 1) % 3, i2 = (i + 2) % 3, j1 = (j + 1) % 3, j2 = (j + 2) % 3;
                inv_t[i][j] = (q[i1][j1] * q[i2][j2] - q[i1][j2] * q[i2][j1]) / d;
            }
        double change = 0;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                const double v = (q[i][j] + inv_t[i][j]) / 2;
                change = std::max(change, std::abs(v - q[i][j]));
                q[i][j] = v;
            }
        if (change < 1e-12) return true;
    }
    return true;
}

using Quat = std::array<double, 4>;  // w, x, y, z

Quat to_quat(const Mat3& m) {
    Quat q;
    const double tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0) {
        const double s = std::sqrt(tr + 1) * 2;
        q = {s / 4, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s};
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const double s = std::sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
        q = {(m[2][1] - m[1][2]) / s, s / 4, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s};
    } else if (m[1][1] > m[2][2]) {
        const double s = std::sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
        q = {(m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, s / 4, (m[1][2] + m[2][1]) / s};
    } else {
        const double s = std::sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
        q = {(m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, s / 4};
    }
    return q;
}

Mat3 from_quat(const Quat& q) {
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    return {{{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
             {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
             {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}}};
}

// Where a matrix command keeps its 3x3 part: the parameter index of each row.
bool linear_rows(uint8_t op, int rows[3]) {
    switch (op) {
    case 0x16: case 0x18: rows[0] = 0, rows[1] = 4, rows[2] = 8; return true;           // 4x4
    case 0x17: case 0x19: case 0x1A: rows[0] = 0, rows[1] = 3, rows[2] = 6; return true;  // 4x3, 3x3
    default: return false;
    }
}

enum class Linear { Blended, Jumped, Unsupported };

// Blends the 3x3 part of the matrix command at a[ia] / out[ib] by fraction t from a: the rotation along the shortest
// arc, the rest (M = S R, with S the scale and shear) linearly. Unsupported when either matrix is degenerate or only
// one is mirrored; the caller then blends element-wise.
Linear blend_linear(const std::vector<Entry>& a, size_t ia, std::vector<Entry>& out, size_t ib, double t) {
    int rows[3];
    if (!linear_rows(out[ib].op, rows)) return Linear::Unsupported;
    Mat3 ma, mb;
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) {
            ma[r][k] = int32_t(a[ia + rows[r] + k].param) / 4096.0;
            mb[r][k] = int32_t(out[ib + rows[r] + k].param) / 4096.0;
        }
    Mat3 qa, qb;
    if (!rotation_of(ma, qa) || !rotation_of(mb, qb)) return Linear::Unsupported;
    const bool mirrored_a = det(qa) < 0, mirrored_b = det(qb) < 0;
    if (mirrored_a != mirrored_b) return Linear::Unsupported;
    if (mirrored_a) {  // a mirror is a rotation of the negated matrix (in 3D, negating flips the determinant)
        for (auto& row : qa)
            for (double& v : row) v = -v;
        for (auto& row : qb)
            for (double& v : row) v = -v;
    }
    // M = S R with S = M R^T; the sign of a mirror stays in S.
    const Mat3 sa = mul(ma, transpose(qa)), sb = mul(mb, transpose(qb));
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            if (std::abs(sb[i][j] - sa[i][j]) > kJumpStretch) return Linear::Jumped;
    Quat pa = to_quat(qa), pb = to_quat(qb);
    double dot = pa[0] * pb[0] + pa[1] * pb[1] + pa[2] * pb[2] + pa[3] * pb[3];
    if (dot < 0) {  // the same rotation has two quaternions: take the one on the short way round
        for (double& v : pb) v = -v;
        dot = -dot;
    }
    const double angle = 2 * std::acos(std::min(1.0, dot));
    if (angle > kJumpAngle) return Linear::Jumped;
    Quat p;
    if (dot > 0.9999) {  // nearly the same rotation: a normalised lerp is exact enough
        for (int i = 0; i < 4; i++) p[i] = pa[i] + (pb[i] - pa[i]) * t;
    } else {
        const double half = std::acos(dot), s = std::sin(half);
        const double wa = std::sin((1 - t) * half) / s, wb = std::sin(t * half) / s;
        for (int i = 0; i < 4; i++) p[i] = pa[i] * wa + pb[i] * wb;
    }
    const double len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2] + p[3] * p[3]);
    for (double& v : p) v /= len;
    Mat3 s{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) s[i][j] = sa[i][j] + (sb[i][j] - sa[i][j]) * t;
    const Mat3 m = mul(s, from_quat(p));
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) {
            const double v = std::round(m[r][k] * 4096.0);
            out[ib + rows[r] + k].param = uint32_t(int32_t(std::clamp(v, -2147483648.0, 2147483647.0)));
        }
    return Linear::Blended;
}

}  // namespace

Blend blend(const std::vector<Entry>& a, const std::vector<Entry>& b, std::vector<Entry>& out, int num, int den) {
    // A camera cut: show the new scene as it is.
    const size_t cam_a = camera_load(a), cam_b = camera_load(b);
    if (cam_a != SIZE_MAX && cam_b != SIZE_MAX && b[cam_b].op == a[cam_a].op &&
        cam_b + entry_count(b[cam_b].op) <= b.size() &&
        cam_a + entry_count(a[cam_a].op) <= a.size() && jumped(a, cam_a, b, cam_b)) {
        return {0, 0, true};
    }
    const auto lerp = [&](uint32_t x, uint32_t y) {
        const int64_t from = int32_t(y), to = int32_t(x);  // from the new scene back towards the old one
        return uint32_t(int32_t(from + (to - from) * (den - num) / den));
    };
    // The matrix mode each command of `b` runs in: object matrices are the ones in the position modes.
    std::vector<uint8_t> mode(b.size(), 0);
    for (size_t i = 0, m = 0; i < b.size(); i += entry_count(b[i].op)) {
        if (b[i].op == 0x10) m = b[i].param & 3;
        mode[i] = uint8_t(m);
    }
    size_t blended = 0, snapped = 0;
    // Blends the matrix commands of a run of commands that is the same in both scenes.
    const auto blend_range = [&](size_t ia, size_t ib, size_t n) {
        for (size_t k = 0; k < n;) {
            const uint8_t op = out[ib + k].op;
            const size_t len = std::min<size_t>(entry_count(op), n - k);
            const size_t ja = ia + k, jb = ib + k;
            k += len;
            if (!is_matrix_param_op(op) || len != size_t(entry_count(op))) continue;
            bool changed = false;
            for (size_t p = 0; p < len; p++) changed |= a[ja + p].param != b[jb + p].param;
            if (!changed) continue;
            // Object matrices turn along the shortest arc. The camera stays element-wise: like RT64, which found
            // cameras look better that way, and its jumps are what detects cuts.
            int rows[3];
            Linear linear = Linear::Unsupported;
            if ((mode[jb] == 1 || mode[jb] == 2) && jb != cam_b && linear_rows(op, rows)) {
                if (jumped(a, ja, out, jb, /*outside_3x3=*/true)) {
                    ++snapped;
                    continue;
                }
                linear = blend_linear(a, ja, out, jb, double(num) / den);
                if (linear == Linear::Jumped) {
                    ++snapped;
                    continue;
                }
            }
            if (linear == Linear::Unsupported && jumped(a, ja, out, jb)) {
                ++snapped;
                continue;
            }
            for (size_t p = 0; p < len; p++) {
                if (a[ja + p].param == b[jb + p].param) continue;
                ++blended;
                const auto in_row = [&](int r) { return p >= size_t(rows[r]) && p < size_t(rows[r]) + 3; };
                const bool in_3x3 = linear == Linear::Blended && (in_row(0) || in_row(1) || in_row(2));
                if (!in_3x3) out[jb + p].param = lerp(a[ja + p].param, b[jb + p].param);
            }
        }
    };
    if (same_opcodes(a, b)) {
        blend_range(0, 0, b.size());
        return {blended, snapped, false};
    }
    const std::vector<Chunk> ca = chunks(a), cb = chunks(b);
    for (const auto& [i, j] : align(ca, cb))
        if (ca[i].entries == cb[j].entries) blend_range(ca[i].first, cb[j].first, cb[j].entries);
    return {blended, snapped, false};
}

}  // namespace ntr::gx
