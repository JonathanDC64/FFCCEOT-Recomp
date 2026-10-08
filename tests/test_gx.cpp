// Unit tests for frame interpolation's GX stream matching and blending (runtime/src/gx_match.cpp), on synthetic
// command streams.
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
    test_identical_scenes();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("all tests passed");
    return 0;
}
