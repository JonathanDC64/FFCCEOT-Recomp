// What the DS sees of the host's buttons. The game reads them once per 30 Hz tick (every second frame) and finds a
// press by seeing the button up in one read and down in the next, so every press and every release is shown for at
// least kMinFrames frames and until the game has read it: a quick tap is not lost between two reads, when buttons are
// mashed a short release does not merge two presses into one, and when the game stalls (it skips its reads while an
// attack's data loads) a tap made meanwhile waits for it. A press made while that is not yet possible waits (one at
// most, so mashing faster than the game can read adds no lag that builds up).
#pragma once

#include <cstdint>

namespace ntr {

class InputLatch {
public:
    static constexpr int kMinFrames = 2;
    static constexpr int kButtons = 12;

    // One emulated frame: `held` are the buttons down now, `pressed` those that went down since the last call, even
    // if already released (from input events, which see taps shorter than a frame), and `read` whether the game read
    // the buttons during the frame since the last call. Returns the buttons to show.
    uint32_t step(uint32_t held, uint32_t pressed, bool read = true) {
        if (read) seen_ = ~0u;  // the game has seen what was shown
        for (int i = 0; i < kButtons; i++) {
            const uint32_t bit = 1u << i;
            if ((pressed & bit) || ((held & bit) && !(prev_held_ & bit))) pending_ |= bit;
            if (age_[i] < kMinFrames) age_[i]++;
            if (age_[i] < kMinFrames || !(seen_ & bit)) continue;
            if (shown_ & bit) {
                // Release when let go, or when a new press is waiting (let go and pressed again meanwhile).
                if (!(held & bit) || (pending_ & bit)) {
                    shown_ &= ~bit;
                    age_[i] = 0;
                    seen_ &= ~bit;
                }
            } else if (pending_ & bit) {
                shown_ |= bit;
                pending_ &= ~bit;
                age_[i] = 0;
                seen_ &= ~bit;
            }
        }
        prev_held_ = held;
        return shown_;
    }

private:
    uint32_t shown_ = 0, pending_ = 0, prev_held_ = 0;
    uint32_t seen_ = ~0u;  // buttons whose shown state the game has read since it last changed
    uint8_t age_[kButtons] = {kMinFrames, kMinFrames, kMinFrames, kMinFrames, kMinFrames, kMinFrames,
                              kMinFrames, kMinFrames, kMinFrames, kMinFrames, kMinFrames, kMinFrames};
};

}  // namespace ntr
