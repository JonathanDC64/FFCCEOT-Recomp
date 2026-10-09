// Scripted input and its recorder: a recording read back gives the input it was made from (buttons, taps, touch, the analog
// stick as exact floats), marks and warnings come through, and a recording made with other settings is refused.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "input_script.hpp"
#include "ntr/runtime.hpp"

static int failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

namespace {

std::filesystem::path temp_file(const char* name) { return std::filesystem::temp_directory_path() / name; }

void write(const std::filesystem::path& p, const std::string& text) {
    std::ofstream(p, std::ios::binary) << text;
}

// The buttons, touch and stick a script gives frame `frame`, the way the replay reads them.
struct Frame {
    std::uint32_t held = 0, taps = 0;
    bool touching = false;
    int tx = 0, ty = 0;
    bool analog = false;
    float ax = 0, ay = 0;
};
Frame at(const input_script::Script& s, std::uint64_t frame) {
    Frame f;
    for (const auto& step : s.steps) {
        if (frame < step.frame) continue;
        if (frame - step.frame < step.frames) {
            f.held |= step.buttons;
            if (step.touch_x >= 0) f.touching = true, f.tx = step.touch_x, f.ty = step.touch_y;
            if (step.analog) f.analog = true, f.ax = step.analog_x, f.ay = step.analog_y;
        }
        if (frame == step.frame) f.taps |= step.taps;
    }
    return f;
}

void test_parse() {
    const auto path = temp_file("ffcc_test_script.txt");
    write(path, "# a comment\n"
                "\n"
                "10 press A+Up 3\n"
                "+5 tap Start\n"
                "30 touch 12,34 2\n"
                "40 analog 0.25,-0.5 4\n"
                "50 shot out.png\n"
                "60 quit\n");
    input_script::Script s;
    std::string error;
    CHECK(input_script::load(path, s, error));
    CHECK(s.steps.size() == 6);
    CHECK(s.steps[0].frame == 10 && s.steps[0].frames == 3 && s.steps[0].buttons == (ntr::KeyA | ntr::KeyUp));
    CHECK(s.steps[1].frame == 15 && s.steps[1].taps == ntr::KeyStart);  // "+5" counts from the step before
    CHECK(s.steps[2].touch_x == 12 && s.steps[2].touch_y == 34 && s.steps[2].frames == 2);
    CHECK(s.steps[3].analog && s.steps[3].analog_x == 0.25f && s.steps[3].analog_y == -0.5f);
    CHECK(s.steps[4].shot == "out.png");
    CHECK(s.steps[5].quit);

    write(path, "5 press Q\n");
    input_script::Script bad;
    CHECK(!input_script::load(path, bad, error));
    CHECK(error.find(":1:") != std::string::npos);  // says which line
    CHECK(!input_script::load(temp_file("ffcc_no_such_script.txt"), bad, error));
    std::filesystem::remove(path);
}

void test_comments() {
    const auto path = temp_file("ffcc_test_comments.txt");
    write(path, "# ffcc-recording 1\n"
                "# require cpu_speed=4 analog_movement=multiplayer\n"
                "# mark 120 the water looks wrong\n"
                "# mark 300\n"
                "# warning: frame 200: cpu_speed changed to 1: the replay drifts after this frame\n"
                "# rev 1.0 (abc)\n"
                "1 press A\n");
    input_script::Script s;
    std::string error;
    CHECK(input_script::load(path, s, error));
    CHECK(s.require.size() == 2 && s.require[0].first == "cpu_speed" && s.require[0].second == "4");
    CHECK(s.marks.size() == 2 && s.marks[0].frame == 120 && s.marks[0].label == "the water looks wrong");
    CHECK(s.marks[1].frame == 300 && s.marks[1].label.empty());
    CHECK(s.warnings.size() == 1 && s.warnings[0].find("cpu_speed changed") != std::string::npos);
    CHECK(s.steps.size() == 1);

    std::string message;
    CHECK(input_script::check_require(s, {{"cpu_speed", "4"}, {"analog_movement", "multiplayer"}, {"aspect", "0"}}, message));
    CHECK(!input_script::check_require(s, {{"cpu_speed", "1"}, {"analog_movement", "multiplayer"}}, message));
    CHECK(message.find("cpu_speed") != std::string::npos && message.find("analog") == std::string::npos);
    std::filesystem::remove(path);
}

void test_round_trip() {
    const auto path = temp_file("ffcc_test_recording.txt");
    std::filesystem::remove(path);
    // 40 frames of a made-up session: A held 3..8, a Start tap at 12 (shorter than a frame), a touch 20..24, the stick
    // pushed 30..39 (the exact floats, with the buttons the game derives from them).
    const float ax = 0.123456789f, ay = -0.987654321f;
    {
        input_script::Recorder rec;
        CHECK(rec.open(path));
        rec.comment("require cpu_speed=4");
        for (std::uint64_t f = 1; f <= 40; f++) {
            std::uint32_t held = 0, pressed = 0;
            bool touching = false;
            float x = 0, y = 0;
            if (f >= 3 && f <= 8) held |= ntr::KeyA;
            if (f == 3) pressed |= ntr::KeyA;
            if (f == 12) pressed |= ntr::KeyStart;
            if (f >= 20 && f <= 24) touching = true;
            if (f >= 30 && f <= 39) x = ax, y = ay, held |= ntr::KeyUp;
            rec.add(f, held, pressed, touching, 100, 50, x, y);
        }
        rec.mark(25, "bug");
        rec.warning(26, "something");
    }
    input_script::Script s;
    std::string error;
    CHECK(input_script::load(path, s, error));
    CHECK(s.require.size() == 1);
    CHECK(s.marks.size() == 1 && s.marks[0].frame == 25 && s.marks[0].label == "bug");
    CHECK(s.warnings.size() == 1);
    for (std::uint64_t f = 1; f <= 40; f++) {
        const Frame g = at(s, f);
        const bool a = f >= 3 && f <= 8, up = f >= 30 && f <= 39;
        CHECK(((g.held & ntr::KeyA) != 0) == a);
        CHECK(((g.held & ntr::KeyUp) != 0) == up);
        CHECK(((g.taps & ntr::KeyStart) != 0) == (f == 12));
        CHECK(g.touching == (f >= 20 && f <= 24));
        if (g.touching) CHECK(g.tx == 100 && g.ty == 50);
        CHECK(g.analog == up);
        if (g.analog) CHECK(g.ax == ax && g.ay == ay);  // bit for bit
    }
    std::filesystem::remove(path);
}

}  // namespace

int main() {
    test_parse();
    test_comments();
    test_round_trip();
    if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
    else std::printf("input script: all checks passed\n");
    return failures ? 1 : 0;
}
