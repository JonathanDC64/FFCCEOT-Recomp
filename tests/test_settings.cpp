// Settings: the settings.ini round trip (every key, including the interface ones and the bindings) and the binding
// swap rules of the controls window.
#include <cstdio>
#include <filesystem>

#include "settings.hpp"

static int failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

namespace {

std::filesystem::path temp_ini() {
    return std::filesystem::temp_directory_path() / "ffcc_test_settings.ini";
}

void test_round_trip() {
    Settings a;
    a.ui_scale = 125;
    a.texture_smoothing = ntr::TextureSmoothing::Sharp;
    a.skip_prelaunch = true;
    a.menu_sounds = false;
    a.preset_chosen = true;
    a.volume = 40;
    a.hud_scale = 150;
    a.controls.spell_ring = false;
    a.controls.spell_ring_orientation = ring::Orientation::Top;
    a.controls.spell_ring_size = 90;
    a.controls.spell_ring_x = 20;
    a.controls.spell_ring_y = 80;
    input::assign_key(a.controls, input::Action::OpenMenu, SDL_SCANCODE_F9);
    input::assign_pad(a.controls, input::Action::OpenMenu, input::PadInput::button(SDL_GAMEPAD_BUTTON_GUIDE));
    const auto path = temp_ini();
    CHECK(save_settings(path, a));
    Settings b;
    CHECK(load_settings(path, b));
    CHECK(b.ui_scale == 125);
    CHECK(b.texture_smoothing == ntr::TextureSmoothing::Sharp);
    CHECK(b.skip_prelaunch);
    CHECK(!b.menu_sounds);
    CHECK(b.preset_chosen);
    CHECK(b.volume == 40);
    CHECK(b.hud_scale == 150);
    CHECK(b.controls == a.controls);
    CHECK(!b.controls.spell_ring);
    CHECK(b.controls.spell_ring_orientation == ring::Orientation::Top);
    CHECK(b.controls.spell_ring_size == 90 && b.controls.spell_ring_x == 20 && b.controls.spell_ring_y == 80);
    CHECK(b.controls.key[int(input::Action::OpenMenu)] == SDL_SCANCODE_F9);
    std::filesystem::remove(path);
}

void test_defaults_survive() {
    const Settings d;
    const auto path = temp_ini();
    CHECK(save_settings(path, d));
    Settings b;
    b.ui_scale = 90;
    b.menu_sounds = false;
    CHECK(load_settings(path, b));
    CHECK(b.ui_scale == d.ui_scale);
    CHECK(b.menu_sounds == d.menu_sounds);
    CHECK(b.controls == d.controls);
    CHECK(d.controls.spell_ring);
    CHECK(d.controls.key[int(input::Action::OpenMenu)] == SDL_SCANCODE_F1);
    std::filesystem::remove(path);
}

void test_set_setting() {
    Settings s;
    CHECK(set_setting(s, "graphics.renderer", "compute"));  // (the compute renderer is switched off: OpenGL instead)
    CHECK(s.renderer == (kComputeRenderer ? ntr::Renderer::Compute : ntr::Renderer::OpenGL));
    CHECK(set_setting(s, "interface.ui_scale", "150"));
    CHECK(s.ui_scale == 150);
    CHECK(!set_setting(s, "interface.ui_scale", "20"));  // (0 or 50-200)
    CHECK(s.ui_scale == 150);
    CHECK(set_setting(s, "interface.skip_prelaunch", "true"));
    CHECK(s.skip_prelaunch);
    CHECK(set_setting(s, "audio.menu_sounds", "false"));
    CHECK(!s.menu_sounds);
    CHECK(set_setting(s, "controls.spell_ring_x", "100"));
    CHECK(set_setting(s, "controls.spell_ring_x", "101"));  // (clamped, like the other numbers)
    CHECK(set_setting(s, "controls.spell_ring_size", "10"));
    CHECK(s.controls.spell_ring_x == 100 && s.controls.spell_ring_size == 30);
    CHECK(set_setting(s, "graphics.texture_smoothing", "smooth"));
    CHECK(s.texture_smoothing == ntr::TextureSmoothing::Smooth);
    CHECK(!set_setting(s, "graphics.texture_smoothing", "blurry"));
    CHECK(s.texture_smoothing == ntr::TextureSmoothing::Smooth);
    CHECK(!set_setting(s, "no.such_key", "1"));
}

void test_assign_key_swaps() {
    using input::Action;
    auto b = input::Bindings::defaults();
    const auto a = b.key[int(Action::A)], bb = b.key[int(Action::B)];
    // B takes A's key: A gets B's old one, so no key does two things.
    input::assign_key(b, Action::B, a);
    CHECK(b.key[int(Action::B)] == a);
    CHECK(b.key[int(Action::A)] == bb);
    // Unbinding.
    input::assign_key(b, Action::X, SDL_SCANCODE_UNKNOWN);
    CHECK(b.key[int(Action::X)] == SDL_SCANCODE_UNKNOWN);
    // Binding a key nobody has takes nothing from anyone.
    const auto before = b;
    input::assign_key(b, Action::Y, SDL_SCANCODE_F7);
    CHECK(b.key[int(Action::Y)] == SDL_SCANCODE_F7);
    for (int i = 0; i < input::kActionCount; i++)
        if (i != int(Action::Y)) CHECK(b.key[i] == before.key[i]);
}

void test_assign_pad_swaps() {
    using input::Action;
    using input::PadInput;
    auto b = input::Bindings::defaults();
    const PadInput a = b.pad[int(Action::A)], bb = b.pad[int(Action::B)];
    input::assign_pad(b, Action::B, a);
    CHECK(b.pad[int(Action::B)] == a);
    CHECK(b.pad[int(Action::A)] == bb);
    input::assign_pad(b, Action::X, PadInput{});
    CHECK(b.pad[int(Action::X)].kind == PadInput::Kind::None);
    // An axis in one direction is not the same input as the other direction.
    input::assign_pad(b, Action::Up, PadInput::axis(SDL_GAMEPAD_AXIS_RIGHTY, false));
    input::assign_pad(b, Action::Down, PadInput::axis(SDL_GAMEPAD_AXIS_RIGHTY, true));
    CHECK(b.pad[int(Action::Up)] == PadInput::axis(SDL_GAMEPAD_AXIS_RIGHTY, false));
    CHECK(b.pad[int(Action::Down)] == PadInput::axis(SDL_GAMEPAD_AXIS_RIGHTY, true));
}

}  // namespace

int main() {
    test_round_trip();
    test_defaults_survive();
    test_set_setting();
    test_assign_key_swaps();
    test_assign_pad_swaps();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("ok");
    return 0;
}
