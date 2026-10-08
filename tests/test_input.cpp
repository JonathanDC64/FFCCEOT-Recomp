// InputLatch: what the DS sees of the host's buttons, against a game that reads them every second frame.
#include <cstdio>
#include <vector>

#include "input_latch.hpp"

static int failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

namespace {

constexpr uint32_t kA = 1;

// One host frame: the button's state when sampled, and whether it went down since the last sample.
struct Sample {
    bool held, pressed;
};

// Presses the game sees: button up in one read and down in the next, reading on frames with f % 2 == phase, except
// for frames in [stall_from, stall_to) (the game busy loading).
int presses_seen(const std::vector<Sample>& host, int phase, std::vector<bool>* shown = nullptr, size_t stall_from = 0,
                 size_t stall_to = 0) {
    ntr::InputLatch latch;
    bool last_read = false, read = false;
    int presses = 0;
    for (size_t f = 0; f < host.size(); f++) {
        const bool down = latch.step(host[f].held ? kA : 0, host[f].pressed ? kA : 0, read) & kA;
        if (shown) shown->push_back(down);
        read = int(f % 2) == phase && (f < stall_from || f >= stall_to);
        if (read) {
            if (down && !last_read) presses++;
            last_read = down;
        }
    }
    return presses;
}

// `count` presses of `down` frames each with `up` frames between them, then idle.
std::vector<Sample> mash(int count, int down, int up) {
    std::vector<Sample> s(4, Sample{false, false});
    for (int i = 0; i < count; i++) {
        for (int d = 0; d < down; d++) s.push_back({true, d == 0});
        for (int u = 0; u < up; u++) s.push_back({false, false});
    }
    s.resize(s.size() + 16, Sample{false, false});
    return s;
}

void test_tap_is_seen() {
    for (int phase = 0; phase < 2; phase++) {
        CHECK(presses_seen(mash(1, 1, 0), phase) == 1);
        // A tap shorter than a frame: only the input event saw it.
        std::vector<Sample> s(4, Sample{false, false});
        s.push_back({false, true});
        s.resize(20, Sample{false, false});
        CHECK(presses_seen(s, phase) == 1);
    }
}

void test_mashing_is_seen() {
    // Any rhythm the game can follow (a press and a release per tick or slower) loses no press, whatever the
    // phase of the game's reads; a short release no longer merges two presses.
    for (int down = 1; down <= 6; down++)
        for (int up = 1; up <= 6; up++) {
            if (down + up < 2 * ntr::InputLatch::kMinFrames) continue;
            for (int phase = 0; phase < 2; phase++) {
                const int seen = presses_seen(mash(10, down, up), phase);
                if (seen != 10) std::fprintf(stderr, "  down %d up %d phase %d: %d of 10\n", down, up, phase, seen);
                CHECK(seen == 10);
            }
        }
}

void test_stall_keeps_presses() {
    // The game skips its reads for 9 frames (loading after an attack lands): taps and presses made meanwhile, and
    // the releases between them, wait for its next reads.
    for (int phase = 0; phase < 2; phase++)
        for (int down = 1; down <= 3; down++)
            for (int up = 1; up <= 3; up++) {
                const std::vector<Sample> s = mash(2, down, up);
                const int seen = presses_seen(s, phase, nullptr, 3, 12);
                if (seen != 2) std::fprintf(stderr, "  stall: down %d up %d phase %d: %d of 2\n", down, up, phase, seen);
                CHECK(seen == 2);
            }
}

void test_hold_is_one_press() {
    std::vector<Sample> s(4, Sample{false, false});
    s.push_back({true, true});
    for (int i = 0; i < 40; i++) s.push_back({true, false});
    std::vector<bool> shown;
    for (int phase = 0; phase < 2; phase++) CHECK(presses_seen(s, phase) == 1);
    presses_seen(s, 0, &shown);
    for (size_t f = 4; f < shown.size(); f++) CHECK(shown[f]);  // held throughout, from the first frame
}

void test_no_added_delay() {
    // An idle button goes down on the frame it is pressed and up on the frame it is let go (after the minimum).
    std::vector<bool> shown;
    presses_seen(mash(1, 5, 0), 0, &shown);
    CHECK(!shown[3] && shown[4] && shown[8] && !shown[9]);
}

void test_fast_mashing_lag_is_bounded() {
    // Mashing faster than the game can read: the extra presses are dropped rather than queued, so the button
    // settles within a few frames of the last release.
    std::vector<bool> shown;
    const std::vector<Sample> s = mash(30, 1, 1);
    presses_seen(s, 0, &shown);
    const size_t last_release = s.size() - 16;
    bool settled = false;
    for (size_t f = last_release; f < last_release + 2 * ntr::InputLatch::kMinFrames + 1; f++) settled |= !shown[f];
    CHECK(settled);
    CHECK(!shown.back());
}

}  // namespace

int main() {
    test_tap_is_seen();
    test_mashing_is_seen();
    test_stall_keeps_presses();
    test_hold_is_one_press();
    test_no_added_delay();
    test_fast_mashing_lag_is_bounded();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("input: all tests passed\n");
    return 0;
}
