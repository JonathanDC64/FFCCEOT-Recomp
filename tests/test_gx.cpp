// Unit tests for frame interpolation's GX stream matching and blending (runtime/src/gx_match.cpp), on synthetic
// command streams.
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

#include "gx_match.hpp"

using ntr::gx::Entry;

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

namespace {

struct Object {
    uint32_t attr;  // polygon attributes: tells objects apart
    int32_t x;      // translation
};

// A scene as the game sends it: the camera (a 4x3 load in position mode with identity rotation), then each object
// drawn after restoring the camera matrix, moved by a translate.
std::vector<Entry> scene(int32_t camera_x, const std::vector<Object>& objects) {
    std::vector<Entry> s;
    const auto add = [&](uint8_t op, uint32_t p) { s.push_back({op, p}); };
    add(0x10, 2);
    const uint32_t camera[12] = {0x1000, 0, 0, 0, 0x1000, 0, 0, 0, 0x1000, uint32_t(camera_x), 0, 0};
    for (uint32_t p : camera) add(0x17, p);
    add(0x13, 0);  // store the camera
    for (const Object& o : objects) {
        add(0x14, 0);  // restore the camera
        add(0x1C, uint32_t(o.x)), add(0x1C, 0), add(0x1C, 0);
        add(0x29, o.attr);
        add(0x40, 0);
        add(0x23, 0x00100010), add(0x23, 0x10);
        add(0x41, 0);
    }
    add(0x50, 0);
    return s;
}

// The parameter of the n-th command with opcode `op` (its first parameter + k).
uint32_t param(const std::vector<Entry>& s, uint8_t op, int n, int k = 0) {
    for (size_t i = 0; i < s.size(); i += ntr::gx::entry_count(s[i].op))
        if (s[i].op == op && n-- == 0) return s[i + k].param;
    return 0xDEADBEEF;
}

void test_param_counts() {
    CHECK(ntr::gx::param_count(0x16) == 16);
    CHECK(ntr::gx::param_count(0x17) == 12);
    CHECK(ntr::gx::param_count(0x1C) == 3);
    CHECK(ntr::gx::entry_count(0x15) == 1);  // no parameters, but one FIFO entry
    CHECK(ntr::gx::entry_count(0x34) == 32);
}

void test_same_opcodes_blend() {
    const auto a = scene(0, {{1, 0x1000}});
    const auto b = scene(0x2000, {{1, 0x3000}});
    auto out = b;
    const auto r = ntr::gx::blend(a, b, out, 1, 2);
    CHECK(!r.cut);
    CHECK(r.blended == 2);
    CHECK(param(out, 0x17, 0, 9) == 0x1000);  // camera halfway
    CHECK(param(out, 0x1C, 0) == 0x2000);     // object halfway
    CHECK(param(out, 0x17, 0, 0) == 0x1000);  // unchanged parameters stay
}

void test_blend_fraction() {
    const auto a = scene(0, {{1, 0}});
    const auto b = scene(0x3000, {{1, -0x3000}});
    auto out = b;
    ntr::gx::blend(a, b, out, 2, 3);  // two thirds of the way from a to b
    CHECK(param(out, 0x17, 0, 9) == 0x2000);
    CHECK(int32_t(param(out, 0x1C, 0)) == -0x2000);
}

void test_camera_cut() {
    const auto a = scene(0, {{1, 0x1000}});
    const auto b = scene(0x100000, {{1, 0x3000}});  // the camera moved 256 units: a cut
    auto out = b;
    const auto r = ntr::gx::blend(a, b, out, 1, 2);
    CHECK(r.cut);
    CHECK(r.blended == 0);
    CHECK(param(out, 0x1C, 0) == 0x3000);  // nothing blended
}

void test_teleport() {
    const auto a = scene(0, {{1, 0x1000}, {2, 0x1000}});
    const auto b = scene(0x2000, {{1, 0x41000}, {2, 0x3000}});  // object 1 jumped 64 units
    auto out = b;
    const auto r = ntr::gx::blend(a, b, out, 1, 2);
    CHECK(!r.cut);
    CHECK(param(out, 0x1C, 0) == 0x41000);  // snaps
    CHECK(param(out, 0x1C, 1) == 0x2000);   // the others still blend
    CHECK(param(out, 0x17, 0, 9) == 0x1000);
}

void test_chunk_alignment() {
    // An object appears between two others: the opcode sequences differ, the common objects still line up.
    const auto a = scene(0, {{1, 0x1000}, {2, 0x1000}});
    const auto b = scene(0, {{1, 0x3000}, {3, 0x5000}, {2, 0x7000}});
    CHECK(!ntr::gx::same_opcodes(a, b));
    auto out = b;
    const auto r = ntr::gx::blend(a, b, out, 1, 2);
    CHECK(!r.cut);
    CHECK(r.blended == 2);
    CHECK(param(out, 0x1C, 0) == 0x2000);  // object 1
    CHECK(param(out, 0x1C, 1) == 0x5000);  // the new object as it is
    CHECK(param(out, 0x1C, 2) == 0x4000);  // object 2
}

// A scene with one object turned by `yaw` degrees about y and scaled by `scale`, drawn through a 4x3 multiply. With
// `camera_yaw`, the camera load is turned instead.
std::vector<Entry> turning_scene(double yaw, double scale = 1, double camera_yaw = 0) {
    std::vector<Entry> s;
    const auto add = [&](uint8_t op, uint32_t p) { s.push_back({op, p}); };
    const auto fx = [](double v) { return uint32_t(int32_t(std::lround(v * 4096))); };
    const auto rot_y = [&](uint8_t op, double deg, double k) {
        const double r = deg * 3.14159265358979323846 / 180, c = std::cos(r) * k, n = std::sin(r) * k;
        const double m[12] = {c, 0, -n, 0, k, 0, n, 0, c, 0, 0, 0};
        for (double v : m) add(op, fx(v));
    };
    add(0x10, 2);
    rot_y(0x17, camera_yaw, 1);
    add(0x13, 0);
    add(0x14, 0);
    rot_y(0x19, yaw, scale);
    add(0x29, 1);
    add(0x40, 0);
    add(0x23, 0x00100010), add(0x23, 0x10);
    add(0x41, 0);
    add(0x50, 0);
    return s;
}

// The 3x3 part of the n-th command with opcode `op`, as doubles.
std::array<double, 9> linear(const std::vector<Entry>& s, uint8_t op, int n = 0) {
    std::array<double, 9> m{};
    for (int k = 0; k < 9; k++) m[k] = int32_t(param(s, op, n, k)) / 4096.0;
    return m;
}

double row_length(const std::array<double, 9>& m, int r) {
    return std::sqrt(m[r * 3] * m[r * 3] + m[r * 3 + 1] * m[r * 3 + 1] + m[r * 3 + 2] * m[r * 3 + 2]);
}

double yaw_of(const std::array<double, 9>& m) { return std::atan2(-m[2], m[0]) * 180 / 3.14159265358979323846; }

bool near(double a, double b, double eps = 0.01) { return std::abs(a - b) < eps; }

void test_rotation_slerp() {
    // A 90-degree turn at the midpoint is a 45-degree turn of the same size (element-wise it would shrink to 0.71).
    const auto a = turning_scene(0), b = turning_scene(90);
    auto out = b;
    const auto r = ntr::gx::blend(a, b, out, 1, 2);
    CHECK(!r.cut);
    CHECK(r.blended > 0);
    const auto m = linear(out, 0x19);
    CHECK(near(yaw_of(m), 45, 0.1));
    for (int row = 0; row < 3; row++) CHECK(near(row_length(m, row), 1));
}

void test_rotation_with_scale() {
    // A scaled object turning 60 degrees, a third of the way: 20 degrees, still twice the size.
    const auto a = turning_scene(0, 2), b = turning_scene(60, 2);
    auto out = b;
    ntr::gx::blend(a, b, out, 1, 3);
    const auto m = linear(out, 0x19);
    CHECK(near(yaw_of(m), 20, 0.1));
    CHECK(near(row_length(m, 0), 2));
    CHECK(near(row_length(m, 1), 2));
}

void test_rotation_snap() {
    // 150 degrees in one step is a snap, not a turn: the new matrix stays.
    const auto a = turning_scene(0), b = turning_scene(150);
    auto out = b;
    ntr::gx::blend(a, b, out, 1, 2);
    CHECK(linear(out, 0x19) == linear(b, 0x19));
}

void test_degenerate_falls_back() {
    // An object growing from nothing cannot be decomposed: it is blended element-wise.
    const auto a = turning_scene(0, 0), b = turning_scene(0, 0.25);
    auto out = b;
    ntr::gx::blend(a, b, out, 1, 2);
    CHECK(near(linear(out, 0x19)[0], 0.125, 0.001));
}

void test_camera_stays_element_wise() {
    // The camera is blended element-wise: cos 20 = 0.94 halfway towards 1 is 0.97, not cos 10 = 0.985.
    const auto a = turning_scene(0, 1, 0), b = turning_scene(0, 1, 20);
    auto out = b;
    const auto r = ntr::gx::blend(a, b, out, 1, 2);
    CHECK(!r.cut);
    CHECK(near(linear(out, 0x17)[0], (1 + std::cos(20 * 3.14159265358979323846 / 180)) / 2, 0.001));
}

void test_identical_scenes() {
    const auto a = scene(0x1000, {{1, 0x1000}});
    auto out = a;
    const auto r = ntr::gx::blend(a, a, out, 1, 2);
    CHECK(r.blended == 0);  // nothing to interpolate
    CHECK(!r.cut);
}

}  // namespace

int main() {
    test_param_counts();
    test_same_opcodes_blend();
    test_blend_fraction();
    test_camera_cut();
    test_teleport();
    test_chunk_alignment();
    test_rotation_slerp();
    test_rotation_with_scale();
    test_rotation_snap();
    test_degenerate_falls_back();
    test_camera_stays_element_wise();
    test_identical_scenes();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("all tests passed");
    return 0;
}
