// The spell radial's sector picking: sectors, the threshold with its hysteresis, the margin at the boundaries.
#include <cmath>
#include <cstdio>

#include "spell_ring.hpp"

static int failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

namespace {
// The stick pushed to `push` at `degrees` (0 = right, counter-clockwise: 90 is up).
ring::State at(float degrees, float push, const ring::State& previous = {}) {
    const float r = degrees * 3.14159265f / 180.0f;
    return ring::pick(std::cos(r) * push, -std::sin(r) * push, previous);
}
}  // namespace

int main() {
    // Centres of the six sectors.
    for (int s = 0; s < ring::kSectors; s++) {
        const ring::State st = at(ring::sector_angle(s), 1.0f);
        CHECK(st.open && st.sector == s);
    }
    CHECK(at(0, 1).sector == 0);
    CHECK(at(60, 1).sector == 1);
    CHECK(at(180, 1).sector == 3);
    CHECK(at(300, 1).sector == 5);
    CHECK(at(-60, 1).sector == 5);  // (negative angles wrap)
    CHECK(at(359, 1).sector == 0);
    // The six sectors map to six different elements, clockwise from the upper right: red, blue, yellow, green, orange, teal.
    bool seen[6] = {};
    for (int s = 0; s < ring::kSectors; s++) seen[ring::element_of(s)] = true;
    for (bool b : seen) CHECK(b);
    CHECK(ring::element_of(1) == 0);  // upper right: red
    CHECK(ring::element_of(0) == 1);  // right: blue
    CHECK(ring::element_of(5) == 2);  // lower right: yellow
    CHECK(ring::element_of(4) == 3);  // lower left: green
    CHECK(ring::element_of(3) == 4);  // left: orange
    CHECK(ring::element_of(2) == 5);  // upper left: teal
    CHECK(ring::element_of(-1) == 2 && ring::element_of(7) == 0);

    // A stick at rest, or pushed only a little, opens nothing.
    CHECK(!at(90, 0.0f).open && at(90, 0.0f).sector == -1);
    CHECK(!at(90, 0.59f).open);
    CHECK(at(90, 0.61f).open);
    // Once open it stays open down to the lower threshold (a stick drifting back does not flicker), then closes.
    const ring::State open = at(90, 1.0f);
    CHECK(at(90, 0.5f, open).open);
    CHECK(at(90, 0.41f, open).open);
    CHECK(!at(90, 0.39f, open).open);
    CHECK(!at(90, 0.5f).open);  // (but a closed one needs the higher push to open again)

    // The boundary between sectors 0 and 1 is at 30 degrees: from sector 0 the stick has to go 8 degrees past it.
    const ring::State s0 = at(0, 1.0f);
    CHECK(at(31, 1.0f, s0).sector == 0);
    CHECK(at(37, 1.0f, s0).sector == 0);
    CHECK(at(39, 1.0f, s0).sector == 1);
    // ... and back from sector 1, the same way.
    const ring::State s1 = at(60, 1.0f);
    CHECK(at(29, 1.0f, s1).sector == 1);
    CHECK(at(21, 1.0f, s1).sector == 0);
    // Without a previous sector the boundary goes to the nearest one.
    CHECK(at(29, 1.0f).sector == 0);
    CHECK(at(31, 1.0f).sector == 1);
    // The wrap-around between sector 5 (300) and 0 (360) works the same.
    const ring::State s5 = at(300, 1.0f);
    CHECK(at(331, 1.0f, s5).sector == 5);
    CHECK(at(340, 1.0f, s5).sector == 0);

    // The Top orientation turns the sectors 30 degrees: one at the top, one at the bottom, three above the middle and three
    // below: red, blue, yellow left to right on top, then green, orange, teal going on clockwise.
    {
        using ring::Orientation;
        const Orientation top = Orientation::Top;
        CHECK(ring::sector_angle(0, top) == 30.0f && ring::sector_angle(1, top) == 90.0f);
        const float angle_of_element[6] = {150, 90, 30, 330, 270, 210};  // red, blue, yellow, green, orange, teal
        for (int e = 0; e < 6; e++) {
            bool found = false;
            for (int s = 0; s < ring::kSectors; s++) {
                if (ring::element_of(s, top) != e) continue;
                found = true;
                CHECK(ring::sector_angle(s, top) == angle_of_element[e]);
                // pushing the stick that way picks that sector
                const float r = angle_of_element[e] * 3.14159265f / 180.0f;
                const ring::State st = ring::pick(std::cos(r), -std::sin(r), {}, top);
                CHECK(st.open && st.sector == s);
            }
            CHECK(found);
        }
        // straight up is the top one (blue); the boundaries are at 60, 120, ... and the margin works there too
        CHECK(ring::pick(0, -1, {}, top).sector == 1 && ring::element_of(1, top) == 1);
        const ring::State up = ring::pick(0, -1, {}, top);
        const float r = 128.0f * 3.14159265f / 180.0f;  // 8 degrees past the boundary at 120
        CHECK(ring::pick(std::cos(r), -std::sin(r), up, top).sector == 1);
        const float r2 = 130.0f * 3.14159265f / 180.0f;
        CHECK(ring::pick(std::cos(r2), -std::sin(r2), up, top).sector == 2);
    }

    if (failures == 0) std::printf("spell_ring: ok\n");
    return failures == 0 ? 0 : 1;
}
