// Adapted from Dusklight (CC0), src/dusk/ui/settings.cpp: the same window, tabs, rows and help panes, with this game's
// settings. A row's value is shown on the left, its choices or its help on the right; a row that does not apply right
// now is disabled, not hidden.
#include "settings_window.hpp"

#include "app.hpp"
#include "bool_button.hpp"
#include "controls_window.hpp"
#include "host.hpp"
#include "number_button.hpp"
#include "pane.hpp"
#include "slider.hpp"
#include "rom_setup.hpp"
#include "string_button.hpp"
#include "ui.hpp"

#include "../report.hpp"
#include "../settings.hpp"
#include "../setup.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <type_traits>

namespace fs = std::filesystem;

namespace ui {
namespace {

constexpr int kRateCaps[] = {0, 60, 90, 120, 144, 165, 240};  // the frame rate limits offered (0 = the display's)
constexpr int kScales[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16};
constexpr int kUiScales[] = {0, 50, 75, 100, 125, 150, 175, 200};

Settings& cfg() {
    return *app().settings;
}

const Settings& defaults() {
    static const Settings instance;
    return instance;
}

// A setting by a function of the settings, so that the same code reads the current and the default value.
#define SETTING(field) [](auto& s) -> auto& { return s.field; }

// Applies the change (the program updates the video, the audio and so on).
void apply_live() {
    apply_scale();
    if (app().settings_changed) {
        app().settings_changed();
    }
}

// Applies the change and saves settings.ini, as Dusklight does for each change.
void commit() {
    apply_live();
    if (app().save_settings) {
        app().save_settings();
    }
}

std::string path_text(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

template <typename Ref>
SelectButton& bool_row(Pane& leftPane, Pane& rightPane, Rml::String key, Ref ref, Rml::String helpText,
    std::function<bool()> isDisabled = {}) {
    auto& button = leftPane.add_child<BoolButton>(BoolButton::Props{
        .key = std::move(key),
        .getValue = [ref] { return bool(ref(cfg())); },
        .setValue =
            [ref](bool value) {
                if (value == bool(ref(cfg()))) {
                    return;
                }
                ref(cfg()) = value;
                commit();
            },
        .isDisabled = std::move(isDisabled),
        .isModified = [ref] { return bool(ref(cfg())) != bool(ref(defaults())); },
    });
    leftPane.register_control(button, rightPane, [helpText = std::move(helpText)](Pane& pane) {
        pane.clear();
        pane.add_rml(helpText);
    });
    return button;
}

SelectButton& number_row(Pane& leftPane, Pane& rightPane, Rml::String key, std::function<int()> get,
    std::function<void(int)> set, std::function<bool()> isModified, Rml::String helpText, int min, int max,
    int step, Rml::String suffix = "%", std::function<bool()> isDisabled = {}) {
    auto getter = get;
    auto setter = [set, min, max](int value) { set(std::clamp(value, min, max)); };
    auto disabledFn = isDisabled;
    auto& button = leftPane.add_child<NumberButton>(NumberButton::Props{
        .key = key,
        .getValue = std::move(get),
        .setValue =
            [setter](int value) {
                setter(value);
                commit();
            },
        .isDisabled = std::move(isDisabled),
        .isModified = std::move(isModified),
        .min = min,
        .max = max,
        .step = step,
        .suffix = suffix,
    });
    leftPane.register_control(button, rightPane,
        [helpText = std::move(helpText), getter, setter, disabledFn, min, max, step, suffix](Pane& pane) {
            pane.clear();
            // A slider as well as the typed value: each value applies as it is dragged, the file is saved on release.
            pane.add_child<Slider>(Slider::Props{
                .min = min,
                .max = max,
                .step = step,
                .suffix = suffix,
                .getValue = getter,
                .setValue =
                    [setter](int value) {
                        setter(value);
                        apply_live();
                    },
                .commit = [] { commit(); },
                .isDisabled = disabledFn,
            });
            pane.add_rml(helpText);
        });
    return button;
}

template <typename Ref>
SelectButton& number_row(Pane& leftPane, Pane& rightPane, Rml::String key, Ref ref, Rml::String helpText, int min,
    int max, int step, Rml::String suffix = "%", std::function<bool()> isDisabled = {}) {
    return number_row(
        leftPane, rightPane, std::move(key), [ref] { return int(ref(cfg())); },
        [ref](int value) { ref(cfg()) = value; }, [ref] { return ref(cfg()) != ref(defaults()); },
        std::move(helpText), min, max, step, std::move(suffix), std::move(isDisabled));
}

// A row with a list of choices on the right, the current one marked.
SelectButton& choice_row(Pane& leftPane, Pane& rightPane, Rml::String key, std::vector<Rml::String> labels,
    std::function<int()> get, std::function<void(int)> set, std::function<bool()> isModified, Rml::String helpText,
    std::function<bool()> isDisabled = {}) {
    auto& button = leftPane.add_select_button({
        .key = std::move(key),
        .getValue =
            [labels, get] {
                const int index = get();
                return index >= 0 && index < int(labels.size()) ? labels[index] : Rml::String{};
            },
        .isDisabled = isDisabled,
        .isModified = std::move(isModified),
    });
    leftPane.register_control(button, rightPane,
        [labels = std::move(labels), get = std::move(get), set = std::move(set), helpText = std::move(helpText)](
            Pane& pane) {
            for (int i = 0; i < int(labels.size()); ++i) {
                pane.add_button({
                                    .text = labels[i],
                                    .isSelected = [get, i] { return get() == i; },
                                })
                    .on_pressed([set, i] {
                        play_sound(kSoundItemChange);
                        set(i);
                        commit();
                    });
            }
            if (!helpText.empty()) {
                pane.add_rml("<br/>" + helpText);
            }
        });
    return button;
}

// A choice among the values of an enum (or an int setting): `values` and `labels` line up.
template <typename Ref>
SelectButton& enum_row(Pane& leftPane, Pane& rightPane, Rml::String key, Ref ref, std::vector<int> values,
    std::vector<Rml::String> labels, Rml::String helpText, std::function<bool()> isDisabled = {}) {
    using T = std::remove_cvref_t<decltype(ref(std::declval<Settings&>()))>;
    const auto indexOf = [values](int value) {
        const auto it = std::find(values.begin(), values.end(), value);
        return it == values.end() ? 0 : int(it - values.begin());
    };
    return choice_row(
        leftPane, rightPane, std::move(key), std::move(labels), [ref, indexOf] { return indexOf(int(ref(cfg()))); },
        [ref, values](int index) { ref(cfg()) = static_cast<T>(values[index]); },
        [ref] { return ref(cfg()) != ref(defaults()); }, std::move(helpText), std::move(isDisabled));
}

bool renderer_is(ntr::Renderer renderer) {
    return cfg().renderer == renderer;
}

int frame_rate_of(const Settings& s) {
    return !s.interpolate ? 0 : s.display_rate ? 2 : 1;
}

int rate_cap_index(const Settings& s) {
    int best = 0;
    for (int i = 0; i < int(std::size(kRateCaps)); i++) {
        if (std::abs(kRateCaps[i] - s.rate_cap) < std::abs(kRateCaps[best] - s.rate_cap)) {
            best = i;
        }
    }
    return best;
}

int online_server_of(const Settings& s) {
    return s.online_server == "kaeru" ? 0 : s.online_server == "wiilink" ? 1 : 2;
}

Rml::String custom_dns_of(const Settings& s) {
    return online_server_of(s) == 2 ? s.online_server : Rml::String{};
}

std::string toolchain_text() {
    const auto toolchain = setup::find_toolchain(app().toolchain_dir);
    return toolchain.ok() ? path_text(toolchain.clang.parent_path()) : std::string("Not found");
}

}  // namespace

SettingsWindow::SettingsWindow(bool prelaunch) : mPrelaunch(prelaunch) {
    if (prelaunch) {
        add_tab("Game files", [this](Rml::Element* content) {
            auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
            auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

            const auto running = [] { return app().game_running && app().game_running(); };

            leftPane.register_control(
                leftPane
                    .add_select_button({
                        .key = "ROM",
                        .getValue =
                            [] {
                                const auto& status = app().status;
                                return status.have_rom_copy ? Rml::String(path_text(status.rom.filename())) :
                                                              Rml::String("(none)");
                            },
                        .isDisabled = [running] { return running() || rom_setup::busy(); },
                    })
                    .on_pressed([] { rom_setup::select_rom(); }),
                rightPane, [running](Pane& pane) {
                    const auto expected = setup::expected_rom();
                    pane.add_rml("The game is built from your own copy of " + escape(expected.name) +
                                 ". It has to be this exact dump:<br/><br/>SHA-1 " + escape(expected.sha1) +
                                 "<br/>CRC-32 " + escape(expected.crc32));
                    if (running()) {
                        pane.add_rml("<br/><br/>The game is running: close the program to choose another ROM.");
                    }
                });
            leftPane.register_control(
                leftPane
                    .add_button(ControlledButton::Props{
                        .text = "Set up again",
                        .isDisabled =
                            [running] { return running() || !app().status.have_rom_copy || rom_setup::busy(); },
                    })
                    .on_pressed([] {
                        play_sound(kSoundItemChange);
                        rom_setup::rebuild(false);
                    }),
                rightPane, [running](Pane& pane) {
                    pane.add_rml("Builds the game again from the copy of your ROM that was kept. Needed when a new "
                                 "version of this program changes what is generated.");
                    if (running()) {
                        pane.add_rml("<br/><br/>The game is running: close the program first.");
                    }
                });
            leftPane.register_control(
                leftPane.add_select_button({
                    .key = "Data folder",
                    .getValue = [] { return Rml::String(path_text(app().data_dir)); },
                }),
                rightPane, [](Pane& pane) {
                    pane.add_text("The data folder is where the settings, saves, logs, reports and the built game are "
                                  "kept.");
                    pane.add_rml("<br/>" + escape(path_text(app().data_dir)));
                    pane.add_button("Open data folder").on_pressed([] {
                        play_sound(kSoundClick);
                        report::reveal(app().data_dir);
                    });
                });
            leftPane.register_control(
                leftPane.add_select_button({
                    .key = "Compiler",
                    .getValue = [] { return Rml::String(toolchain_text()); },
                }),
                rightPane, [](Pane& pane) {
                    pane.add_text("The compiler that builds the game from your ROM. It comes with this program.");
                    pane.add_rml("<br/>" + escape(toolchain_text()));
                });
        });
    }

    add_tab("Video", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        const auto notOpenGL = [] { return !renderer_is(ntr::Renderer::OpenGL); };

        leftPane.add_section("Display");
        enum_row(leftPane, rightPane, "Screen layout", [](auto& s) -> auto& { return s.layout.layout; },
            {0, 1, 2, 3, 4}, {"Single screen", "Main + side", "Stacked", "Side by side", "Two windows"},
            "How the DS's two screens are arranged: one screen (the other as a small picture-in-picture), a main "
            "screen with a side screen, stacked, side by side, or a second window you can move and resize. Click "
            "the bottom screen with the mouse to touch it.");
        choice_row(
            leftPane, rightPane, "Main screen", {"Top screen", "Bottom screen"},
            [] { return cfg().layout.swap ? 1 : 0; }, [](int i) { cfg().layout.swap = i == 1; },
            [] { return cfg().layout.swap != defaults().layout.swap; },
            "Which DS screen is the large one: the top screen, or the bottom (touch) screen.");
        bool_row(
            leftPane, rightPane, "Show the other screen", [](auto& s) -> auto& { return s.layout.second_screen; },
            "Shows the other screen as a small picture inside the main one. F3 switches which screen is shown.",
            [] { return cfg().layout.layout != video::Layout::Single; });
        number_row(
            leftPane, rightPane, "Small screen size", [] { return int(cfg().layout.pip_size * 100 + 0.5f); },
            [](int v) { cfg().layout.pip_size = float(v) / 100.0f; },
            [] { return cfg().layout.pip_size != defaults().layout.pip_size; },
            "How big the small picture of the other screen is, as a share of the window.", 10, 50, 5, "%",
            [] { return cfg().layout.layout != video::Layout::Single || !cfg().layout.second_screen; });
        enum_row(
            leftPane, rightPane, "Side screen position", [](auto& s) -> auto& { return s.layout.side_align; },
            {0, 1, 2}, {"Top", "Middle", "Bottom"},
            "Where the side screen sits next to the main one: top, middle or bottom.",
            [] { return cfg().layout.layout != video::Layout::MainSide; });
        bool_row(leftPane, rightPane, "Fullscreen", SETTING(fullscreen),
            "Borderless fullscreen over the whole display. F11 switches it while playing.");
        enum_row(
            leftPane, rightPane, "Aspect ratio", SETTING(aspect), {0, 1, 2, 3, 4},
            {"4:3 (original)", "16:9 (widescreen)", "16:10", "21:9 (ultrawide)", "32:9 (super ultrawide)"},
            "A wider picture shows more of the world to the sides. Menus, dialogs and the title stay 4:3. Needs the "
            "OpenGL renderer.",
            notOpenGL);
        choice_row(
            leftPane, rightPane, "HUD position", {"Centred (as the original)", "At the screen edges"},
            [] { return cfg().hud_edges ? 1 : 0; }, [](int i) { cfg().hud_edges = i == 1; },
            [] { return cfg().hud_edges != defaults().hud_edges; },
            "With a wide picture: keep the health and magic bars in the middle as in the original, or move them to "
            "the edges of the screen. Needs the OpenGL renderer.",
            [notOpenGL] { return notOpenGL() || cfg().aspect == Aspect::Original; });
        number_row(leftPane, rightPane, "HUD size", SETTING(hud_scale),
            "How big the health and magic bars in the corners are. Needs the OpenGL renderer.", 50, 200, 5, "%",
            notOpenGL);

        leftPane.add_section("Graphics");
        enum_row(
            leftPane, rightPane, "Renderer", SETTING(renderer), {0, 1, 2},
            {"Software (exact, native)", "OpenGL (upscaled)", "Compute (upscaled, accurate)"},
            std::string("Software is the exact, native renderer. OpenGL draws upscaled and supports widescreen. "
                        "Compute is upscaled and the most accurate (needs OpenGL 4.3).") +
                (app().compute_available ? "" : "<br/><br/>Compute is not available here: OpenGL is used instead."));
        {
            std::vector<int> values(std::begin(kScales), std::end(kScales));
            std::vector<Rml::String> labels;
            for (const int v : kScales) {
                labels.push_back(v == 0 ? "Match the window" : v == 1 ? "1x (256x192)" : std::format("{}x", v));
            }
            enum_row(
                leftPane, rightPane, "Internal resolution", SETTING(scale), values, labels,
                "How sharp the 3D picture is: a multiple of the DS's 256x192 per screen. Higher looks better and "
                "needs a stronger graphics card. Match the window follows the window's size.",
                [] { return renderer_is(ntr::Renderer::Software); });
        }
        choice_row(
            leftPane, rightPane, "3D frame rate",
            {"30 fps (original)", "60 fps (interpolated)", "Display refresh rate (interpolated)"},
            [] { return frame_rate_of(cfg()); },
            [](int i) {
                cfg().interpolate = i != 0;
                cfg().display_rate = i == 2;
            },
            [] { return frame_rate_of(cfg()) != frame_rate_of(defaults()); },
            "The game draws its 3D at 30 fps. Interpolation shows blended images in between, for one frame (16 ms) "
            "of extra latency. The display's refresh rate (120, 144 Hz and up) needs the OpenGL or compute "
            "renderer and syncs frames to the display. F9 switches between the three.");
        enum_row(
            leftPane, rightPane, "Frame pacing", SETTING(pacing), {0, 1}, {"V-sync", "Variable refresh (VRR)"},
            "V-sync shows frames in step with the display. Variable refresh presents each one the moment it is "
            "ready, on a timer of its own, with the lowest delay. It is meant for a display with G-Sync or "
            "FreeSync on (fullscreen shows it best); on any other display it can tear.",
            [] { return frame_rate_of(cfg()) != 2 || renderer_is(ntr::Renderer::Software); });
        {
            std::vector<Rml::String> labels;
            for (const int v : kRateCaps) {
                labels.push_back(v == 0 ? "Just under the display's rate" : std::format("{} fps", v));
            }
            choice_row(
                leftPane, rightPane, "Frame rate limit", labels, [] { return rate_cap_index(cfg()); },
                [](int i) { cfg().rate_cap = kRateCaps[i]; },
                [] { return cfg().rate_cap != defaults().rate_cap; },
                "The most frames per second to present with variable refresh. Just under the display's rate keeps "
                "G-Sync or FreeSync from falling back to V-sync.",
                [] {
                    return frame_rate_of(cfg()) != 2 || renderer_is(ntr::Renderer::Software) ||
                           cfg().pacing != Pacing::Variable;
                });
        }
        enum_row(
            leftPane, rightPane, "Scaling filter", SETTING(filter), {0, 1, 2},
            {"Nearest (pixel edges)", "Linear (smooth)", "Sharp (crisp pixels)"},
            "How the picture is scaled to the window: nearest keeps hard pixel edges, linear is smooth, sharp keeps "
            "crisp pixels without shimmering.");
        bool_row(leftPane, rightPane, "Better polygons", SETTING(better_polygons),
            "Splits quads into triangles in a way that avoids visible seams on some surfaces. OpenGL renderer only.",
            notOpenGL);
        enum_row(
            leftPane, rightPane, "Texture smoothing", SETTING(texture_smoothing), {0, 1, 2},
            {"Off (blocky texels)", "Smooth", "Sharp (crisp texels)"},
            "The game's textures are drawn with nearest sampling, so upscaled they show their texels as blocks (this is "
            "separate from the scaling filter, which only scales the finished picture). Smooth blends neighbouring texels. "
            "Sharp keeps each texel crisp and blends only at the edges between them. Differs from the original console. "
            "OpenGL renderer only.",
            notOpenGL);
        bool_row(leftPane, rightPane, "Stable textures", SETTING(stable_textures),
            "Some ground tiles in the game have textures that cannot be mapped flat, so they seem to swim as you "
            "move (the DS does this too). This cuts them into small pieces so the texture stays on the ground. "
            "Differs from the original console.");
        bool_row(leftPane, rightPane, "High-precision vertices", SETTING(hires_coordinates),
            "Keeps vertex positions at full precision instead of snapping them to the DS's pixel grid. Compute "
            "renderer only.",
            [] { return !renderer_is(ntr::Renderer::Compute); });
    });

    add_tab("Input", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.add_section("Inputs");
        leftPane.register_control(
            leftPane.add_group_button({.text = "Configure Inputs"}).on_pressed([this] {
                push(std::make_unique<ControlsWindow>());
            }),
            rightPane, [](Pane& pane) {
                pane.clear();
                pane.add_text("Open input binding configuration.");
            });
        bool_row(leftPane, rightPane, "Left stick moves too", [](auto& s) -> auto& { return s.controls.stick_moves; },
            "The left stick also moves the character and steers the menus, like the D-pad.");
        number_row(
            leftPane, rightPane, "Stick deadzone", [](auto& s) -> auto& { return s.controls.deadzone; },
            "How far the stick has to be pushed before it counts. Raise it if the character drifts when you let go.",
            10, 80, 5, "%", [] { return !cfg().controls.stick_moves; });
        enum_row(
            leftPane, rightPane, "Analog movement", [](auto& s) -> auto& { return s.controls.analog_movement; },
            {0, 1, 2}, {"Off", "Single-player", "Single-player and multiplayer"},
            "Your character walks the way the stick points, slower when it is pushed less, instead of the D-pad's "
            "eight directions.<br/><br/>Multiplayer: every player must use this PC version. A DS or another emulator "
            "moves your character in eight directions, so its game falls out of step with yours. This version only "
            "sends your stick while every player's game is known to be this one, but a player who joins as you "
            "enter a new area could still fall out of step. Choose Single-player when playing with a DS or an "
            "emulator.",
            [] { return !cfg().controls.stick_moves; });
        bool_row(leftPane, rightPane, "Spell radial", [](auto& s) -> auto& { return s.controls.spell_ring; },
            "Push the right stick to pick the spell element from a radial of the six gems. The element is chosen as soon "
            "as you point at it, then hold X to cast. The game's own ways (touching a gem, R + left or right) still work.");
        enum_row(
            leftPane, rightPane, "Radial orientation", [](auto& s) -> auto& { return s.controls.spell_ring_orientation; },
            {0, 1}, {"One each side", "Three on top"},
            "Which way round the six gems sit. One each side: one gem to the right, one to the left, two above and two below. "
            "Three on top: red, blue and yellow across the top from left to right, then green, orange and teal below, "
            "going round clockwise.",
            [] { return !cfg().controls.spell_ring; });
        number_row(
            leftPane, rightPane, "Radial size", [](auto& s) -> auto& { return s.controls.spell_ring_size; },
            "How big the spell radial is, in percent of the screen's height.", 30, 150, 5, "%",
            [] { return !cfg().controls.spell_ring; });
        number_row(
            leftPane, rightPane, "Radial position, across", [](auto& s) -> auto& { return s.controls.spell_ring_x; },
            "Where the spell radial sits on the screen from left (0) to right (100). 50 is the middle; it always stays "
            "on the screen.",
            0, 100, 5, "%", [] { return !cfg().controls.spell_ring; });
        number_row(
            leftPane, rightPane, "Radial position, down", [](auto& s) -> auto& { return s.controls.spell_ring_y; },
            "Where the spell radial sits on the screen from top (0) to bottom (100). 50 is the middle; it always stays "
            "on the screen.",
            0, 100, 5, "%", [] { return !cfg().controls.spell_ring; });
    });

    add_tab("Game", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.add_section("Audio");
        number_row(leftPane, rightPane, "Volume", SETTING(volume), "The game's sound volume.", 0, 100, 5);
        bool_row(leftPane, rightPane, "Menu sounds", SETTING(menu_sounds),
            "Plays the game's own sound effects in these menus. They play while the game is running.");

        leftPane.add_section("Game");
        bool_row(leftPane, rightPane, "Skip the company logos", SETTING(skip_logos),
            "Goes straight to the title screen when the game starts. Takes effect the next time the game starts.");
        bool_row(leftPane, rightPane, "Fast loading", SETTING(fast_loading),
            "When an attack lands, the game loads and sets up data for longer than a frame, and the picture hitches "
            "(on the DS too). Fast loading and a faster CPU remove the hitch. The short freeze on a combo's last "
            "hit is the game's own.");
        choice_row(
            leftPane, rightPane, "CPU speed", {"DS (67 MHz)", "2x", "4x"},
            [] { return cfg().cpu_speed >= 4 ? 2 : cfg().cpu_speed >= 2 ? 1 : 0; },
            [](int i) { cfg().cpu_speed = 1 << i; },
            [] { return cfg().cpu_speed != defaults().cpu_speed; },
            "How fast the DS's main processor runs: the DS's own 67 MHz, or two or four times faster. Faster "
            "removes the hitches in busy scenes.");
        bool_row(leftPane, rightPane, "Fast saving", SETTING(fast_saving),
            "The game writes its save a little at a time, a few seconds at the DS's pace. Fast saving writes it at "
            "once.");
    });

    add_tab("Network", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.register_control(
            leftPane.add_child<StringButton>(StringButton::Props{
                .key = "Console name",
                .getValue = [] { return Rml::String(resolved_console_name(cfg())); },
                .setValue =
                    [](Rml::String value) {
                        cfg().console_name = sanitize_console_name(value);
                        commit();
                    },
                .isModified = [] { return !cfg().console_name.empty(); },
                .maxLength = 10,
            }),
            rightPane, [](Pane& pane) {
                pane.clear();
                pane.add_rml("The name other players see for you (up to 10 characters). Clear it to use this PC's "
                             "name (" +
                             escape(system_console_name()) + "). Takes effect the next time the game starts.");
            });
        choice_row(
            leftPane, rightPane, "Wi-Fi Connection server", {"Kaeru WFC", "WiiLink WFC", "Custom"},
            [] { return online_server_of(cfg()); },
            [](int i) {
                if (i == 0) {
                    cfg().online_server = "kaeru";
                } else if (i == 1) {
                    cfg().online_server = "wiilink";
                } else if (online_server_of(cfg()) != 2) {
                    cfg().online_server = "0.0.0.0";  // the host's own DNS, until an address is typed
                }
            },
            [] { return cfg().online_server != defaults().online_server; },
            "The game's Wi-Fi Connection goes through a revival server, found through its DNS server. Takes effect "
            "the next time the game starts.");
        leftPane.register_control(
            leftPane.add_child<StringButton>(StringButton::Props{
                .key = "DNS server address",
                .getValue = [] { return custom_dns_of(cfg()); },
                .setValue =
                    [](Rml::String value) {
                        std::array<uint8_t, 4> dns;
                        if (online_dns(value, dns)) {  // (an address that is not one leaves the server as it was)
                            cfg().online_server = value;
                            commit();
                        }
                    },
                .isDisabled = [] { return online_server_of(cfg()) != 2; },
                .isModified = [] { return online_server_of(cfg()) == 2; },
                .maxLength = 15,
            }),
            rightPane, [](Pane& pane) {
                pane.clear();
                pane.add_rml("The address of the revival server's DNS (digits and dots). Used when the server is set "
                             "to Custom.");
            });
        enum_row(
            leftPane, rightPane, "Input buffer", SETTING(online_buffer), {0, 1, 2}, {"Low delay", "Medium", "Original"},
            "How long the host holds each guest's inputs, so a late packet doesn't stop the game; it grows with the "
            "ping. The game's own buffer delays a guest's controls by about 200 ms. Try Medium, then Original, if "
            "online play stutters. Takes effect from the next multiplayer session.");
    });

    add_tab("Interface", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        {
            std::vector<int> values(std::begin(kUiScales), std::end(kUiScales));
            std::vector<Rml::String> labels;
            for (const int v : kUiScales) {
                labels.push_back(v == 0 ? "Auto" : std::format("{}%", v));
            }
            enum_row(leftPane, rightPane, "UI scale", SETTING(ui_scale), values, labels,
                "Scales this interface relative to the display's DPI scale. Auto follows the display. Has no effect "
                "on the game's own screens.");
        }
        bool_row(leftPane, rightPane, "Skip the main menu at start", SETTING(skip_prelaunch),
            "Starts the game right away when it is set up, without the main menu. The menu is still available in "
            "the game: press F1 or R+Start.");
    });
}

void SettingsWindow::update() {
    if (mPrelaunch && top_document() == this) {
        rom_setup::update_modals(*this);
    }

    Window::update();
}

void SettingsWindow::hide(bool close) {
    if (app().save_settings) {
        app().save_settings();
    }
    Window::hide(close);
}

}  // namespace ui
