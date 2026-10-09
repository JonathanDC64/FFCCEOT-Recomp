#include "ui.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "RmlUi_Platform_SDL.h"
#include "RmlUi_Renderer_GL3.h"
#include "main_thread.hpp"
#include "ui_assets.hpp"

namespace ui {

namespace {

// Serves the embedded UI files to RmlUi.
class EmbeddedFiles final : public Rml::FileInterface {
    struct Open {
        const Asset* asset;
        size_t pos;
    };

public:
    Rml::FileHandle Open(const Rml::String& path) override {
        const Asset* a = find_asset(path);
        if (!a) return 0;
        return reinterpret_cast<Rml::FileHandle>(new struct Open{a, 0});
    }
    void Close(Rml::FileHandle file) override { delete reinterpret_cast<struct Open*>(file); }
    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override {
        auto* f = reinterpret_cast<struct Open*>(file);
        const size_t n = std::min(size, f->asset->size - f->pos);
        std::memcpy(buffer, f->asset->data + f->pos, n);
        f->pos += n;
        return n;
    }
    bool Seek(Rml::FileHandle file, long offset, int origin) override {
        auto* f = reinterpret_cast<struct Open*>(file);
        const long base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? long(f->pos) : long(f->asset->size);
        const long p = base + offset;
        if (p < 0 || size_t(p) > f->asset->size) return false;
        f->pos = size_t(p);
        return true;
    }
    size_t Tell(Rml::FileHandle file) override { return reinterpret_cast<struct Open*>(file)->pos; }
};

// RmlUi's SDL system interface, with its log messages printed.
class System final : public SystemInterface_SDL {
public:
    using SystemInterface_SDL::SystemInterface_SDL;
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        if (type <= Rml::Log::LT_WARNING) std::fprintf(stderr, "ui: %s\n", message.c_str());
        return true;
    }
};

const char* const kTabs[] = {"display", "graphics", "audio", "game", "network", "controls"};
constexpr int kTabCount = int(std::size(kTabs));

}  // namespace

// The menu's view of the settings: plain ints and bools for RmlUi's data bindings.
struct State {
    int layout = 0, swap = 0, pip_size = 30, side_align = 2;  // swap: 0 = top screen main, 1 = bottom
    bool second_screen = true, fullscreen = false;
    int aspect = 0;                 // 0 = 4:3, 1 = 16:9 (OpenGL renderer)
    int hud_position = 0;           // widescreen: 0 = centred, 1 = at the edges
    int hud_size = 100;             // percent
    int renderer = 1, scale = 0, filter = 1;
    bool better_polygons = false, hires_coordinates = true;
    int frame_rate = 1;  // 3D: 0 = the game's 30 fps, 1 = 60 fps, 2 = the display's refresh rate
    int volume = 100;
    bool skip_logos = true;
    bool fast_loading = true;
    bool fast_saving = true;
    int cpu_speed = 2;  // 0 = the DS's clock, 1 = twice, 2 = four times
    int online_server = 0;   // 0 = Kaeru WFC, 1 = WiiLink WFC, 2 = custom
    Rml::String custom_dns;  // the custom server's DNS address
    int online_buffer = 0;   // 0 = low, 1 = medium, 2 = the game's own
    bool stick_moves = true;
    int deadzone = 35;
    int analog_movement = 1;  // 0 = off, 1 = single-player, 2 = also multiplayer
    bool operator==(const State&) const = default;
};

// One line of the bindings table.
struct BindRow {
    Rml::String label, key, pad;
};

struct Menu::Impl {
    SDL_Window* window = nullptr;
    Settings* settings = nullptr;
    EmbeddedFiles files;
    std::unique_ptr<System> system;
    std::unique_ptr<RenderInterface_GL3> render;
    Rml::Context* context = nullptr;
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelHandle model;
    State state, applied;
    Rml::String tab = "display";
    bool compute_available = true;
    int focus = 0;           // index into the focusable rows of the current page
    bool stick_held = false;  // left stick past the threshold (one step per push)

    // Bindings table: the column the keyboard/gamepad focus is on, and the binding waiting for an input.
    std::vector<BindRow> binds;
    int bind_col = 0;      // 0 = keyboard, 1 = gamepad
    int capture_row = -1;  // the action being rebound, or -1
    int capture_col = 0;
    Uint64 capture_started = 0;
    bool editing_dns = false;  // typing the custom DNS address
    bool* changed = nullptr;   // the menu's flag

    // Fills the table from the settings, naming pad inputs for the connected gamepad's type.
    void refresh_binds() {
        SDL_GamepadType type = SDL_GAMEPAD_TYPE_STANDARD;
        int count = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
            if (count > 0) type = SDL_GetGamepadTypeForID(ids[0]);
            SDL_free(ids);
        }
        const input::Bindings& b = settings->controls;
        binds.resize(input::kActionCount);
        for (int i = 0; i < input::kActionCount; i++) {
            binds[i].label = input::action_label(input::Action(i));
            binds[i].key = input::key_label(b.key[i]);
            binds[i].pad = input::pad_label(b.pad[i], type);
        }
        model.DirtyVariable("binds");
    }

    void start_capture(int row, int col) {
        if (row < 0 || row >= input::kActionCount) return;
        capture_row = row;
        capture_col = col;
        bind_col = col;
        capture_started = SDL_GetTicks();
        model.DirtyVariable("capture_row");
        model.DirtyVariable("capture_col");
        model.DirtyVariable("bind_col");
    }

    void end_capture() {
        capture_row = -1;
        model.DirtyVariable("capture_row");
    }

    // Binds the action being captured. An input already bound to another action moves over to it, and that action
    // takes this one's old input, so nothing is bound twice.
    void bind_key(SDL_Scancode sc) {
        input::Bindings& b = settings->controls;
        for (int j = 0; j < input::kActionCount; j++)
            if (j != capture_row && sc != SDL_SCANCODE_UNKNOWN && b.key[j] == sc) b.key[j] = b.key[capture_row];
        b.key[capture_row] = sc;
        bound();
    }
    void bind_pad(input::PadInput p) {
        input::Bindings& b = settings->controls;
        for (int j = 0; j < input::kActionCount; j++)
            if (j != capture_row && p.kind != input::PadInput::Kind::None && b.pad[j] == p) b.pad[j] = b.pad[capture_row];
        b.pad[capture_row] = p;
        bound();
    }
    void bound() {
        end_capture();
        refresh_binds();
        *changed = true;
    }

    // While capturing, every input goes here.
    void capture_event(const SDL_Event& e) {
        switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
            if (e.key.repeat) return;
            if (e.key.key == SDLK_ESCAPE)
                end_capture();
            else if (e.key.key == SDLK_DELETE || e.key.key == SDLK_BACKSPACE)
                capture_col == 0 ? bind_key(SDL_SCANCODE_UNKNOWN) : bind_pad({});
            else if (capture_col == 0)
                bind_key(e.key.scancode);
            return;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
            const auto b = SDL_GamepadButton(e.gbutton.button);
            // Guide opens the menu, so it cannot be bound; on the keyboard column a pad button cancels.
            if (b == SDL_GAMEPAD_BUTTON_GUIDE || capture_col == 0)
                end_capture();
            else
                bind_pad(input::PadInput::button(b));
            return;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (capture_col == 1 && std::abs(int(e.gaxis.value)) > 22000)
                bind_pad(input::PadInput::axis(SDL_GamepadAxis(e.gaxis.axis), e.gaxis.value > 0));
            return;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: end_capture(); return;
        default: return;
        }
    }

    void start_dns_edit() {
        editing_dns = true;
        model.DirtyVariable("editing_dns");
        // (shows an on-screen keyboard where there is one)
        main_thread::run([w = window] { SDL_StartTextInput(w); }, false);
    }

    void end_dns_edit() {
        editing_dns = false;
        model.DirtyVariable("editing_dns");
        main_thread::run([w = window] { SDL_StopTextInput(w); }, false);
    }

    // While typing the address: digits and dots go in, Backspace deletes, Enter, Esc or a gamepad button ends it.
    void dns_event(const SDL_Event& e) {
        switch (e.type) {
        case SDL_EVENT_TEXT_INPUT:
            for (const char* t = e.text.text; *t; t++)
                if (((*t >= '0' && *t <= '9') || *t == '.') && state.custom_dns.size() < 15) state.custom_dns += *t;
            model.DirtyVariable("custom_dns");
            return;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_BACKSPACE && !state.custom_dns.empty()) {
                state.custom_dns.pop_back();
                model.DirtyVariable("custom_dns");
            } else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_ESCAPE) {
                end_dns_edit();
            }
            return;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_DOWN: end_dns_edit(); return;
        default: return;
        }
    }

    void load_state() {
        const Settings& s = *settings;
        state.layout = int(s.layout.layout);
        state.swap = s.layout.swap ? 1 : 0;
        state.second_screen = s.layout.second_screen;
        state.pip_size = int(s.layout.pip_size * 100 + 0.5f);
        state.side_align = int(s.layout.side_align);
        state.fullscreen = s.fullscreen;
        state.aspect = s.widescreen ? 1 : 0;
        state.hud_position = s.hud_edges ? 1 : 0;
        state.hud_size = s.hud_scale;
        state.renderer = int(s.renderer);
        state.scale = s.scale;
        state.filter = int(s.filter);
        state.better_polygons = s.better_polygons;
        state.hires_coordinates = s.hires_coordinates;
        state.frame_rate = !s.interpolate ? 0 : s.display_rate ? 2 : 1;
        state.volume = s.volume;
        state.skip_logos = s.skip_logos;
        state.fast_loading = s.fast_loading;
        state.fast_saving = s.fast_saving;
        state.cpu_speed = s.cpu_speed >= 4 ? 2 : s.cpu_speed >= 2 ? 1 : 0;
        state.online_server = s.online_server == "kaeru" ? 0 : s.online_server == "wiilink" ? 1 : 2;
        if (state.online_server == 2) state.custom_dns = s.online_server;
        state.online_buffer = int(s.online_buffer);
        state.stick_moves = s.controls.stick_moves;
        state.deadzone = s.controls.deadzone;
        state.analog_movement = int(s.controls.analog_movement);
        applied = state;
        model.DirtyAllVariables();
    }

    // Copies edits back into the settings. Returns true if anything changed.
    bool store_state() {
        if (state == applied) return false;
        Settings& s = *settings;
        s.layout.layout = video::Layout(std::clamp(state.layout, 0, 3));
        s.layout.swap = state.swap != 0;
        s.layout.second_screen = state.second_screen;
        s.layout.pip_size = std::clamp(state.pip_size, 10, 50) / 100.0f;
        s.layout.side_align = video::Align(std::clamp(state.side_align, 0, 2));
        s.fullscreen = state.fullscreen;
        s.widescreen = state.aspect != 0;
        s.hud_edges = state.hud_position != 0;
        s.hud_scale = std::clamp(state.hud_size, 50, 200);
        s.renderer = ntr::Renderer(std::clamp(state.renderer, 0, 2));
        s.scale = std::clamp(state.scale, 0, 16);
        s.filter = video::Filter(std::clamp(state.filter, 0, 2));
        s.better_polygons = state.better_polygons;
        s.hires_coordinates = state.hires_coordinates;
        s.interpolate = state.frame_rate != 0;
        s.display_rate = state.frame_rate == 2;
        s.volume = std::clamp(state.volume, 0, 100);
        s.skip_logos = state.skip_logos;
        s.fast_loading = state.fast_loading;
        s.fast_saving = state.fast_saving;
        s.cpu_speed = 1 << std::clamp(state.cpu_speed, 0, 2);
        std::array<uint8_t, 4> dns;
        if (state.online_server < 2)
            s.online_server = state.online_server == 0 ? "kaeru" : "wiilink";
        else if (online_dns(state.custom_dns, dns))  // (a partly typed address leaves the server as it was)
            s.online_server = state.custom_dns;
        s.online_buffer = cfie::OnlineBuffer(std::clamp(state.online_buffer, 0, 2));
        s.controls.stick_moves = state.stick_moves;
        s.controls.deadzone = std::clamp(state.deadzone, 5, 90);
        s.controls.analog_movement = cfie::AnalogMovement(std::clamp(state.analog_movement, 0, 2));
        applied = state;
        return true;
    }

    // The rows that keyboard/gamepad navigation moves between: the current page's settings, then the buttons.
    Rml::ElementList rows() {
        Rml::ElementList all, out;
        document->QuerySelectorAll(all, ".nav");
        for (Rml::Element* e : all)
            if (e->IsVisible(true)) out.push_back(e);
        return out;
    }

    void update_focus() {
        Rml::ElementList r = rows();
        if (r.empty()) return;
        focus = std::clamp(focus, 0, int(r.size()) - 1);
        for (int i = 0; i < int(r.size()); i++) r[i]->SetClass("focused", i == focus);
        r[focus]->ScrollIntoView(false);
    }

    void switch_tab(int dir) {
        int i = 0;
        while (i < kTabCount && tab != kTabs[i]) i++;
        tab = kTabs[(i + dir + kTabCount) % kTabCount];
        model.DirtyVariable("tab");
        focus = 0;
    }

    // Left/right on a row: step its select, slider or checkbox.
    void adjust(int dir) {
        Rml::ElementList r = rows();
        if (r.empty()) return;
        Rml::Element* row = r[std::clamp(focus, 0, int(r.size()) - 1)];
        if (row->IsClassSet("bind")) {
            bind_col = std::clamp(bind_col + dir, 0, 1);
            model.DirtyVariable("bind_col");
            return;
        }
        Rml::ElementList controls;
        row->QuerySelectorAll(controls, "select, input");
        if (controls.empty()) return;
        Rml::Element* c = controls[0];
        if (auto* sel = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(c)) {
            sel->SetSelection(std::clamp(sel->GetSelection() + dir, 0, sel->GetNumOptions() - 1));
        } else if (auto* in = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(c)) {
            const Rml::String type = in->GetAttribute<Rml::String>("type", "");
            if (type == "range") {
                const float step = in->GetAttribute<float>("step", 1.0f), lo = in->GetAttribute<float>("min", 0.0f),
                            hi = in->GetAttribute<float>("max", 100.0f);
                const float v = std::clamp(std::stof("0" + in->GetValue()) + dir * step, lo, hi);
                in->SetValue(std::to_string(int(v)));
            } else if (type == "checkbox") {
                in->Click();
            }
        }
    }

    // Enter / gamepad confirm on a row: toggle a checkbox, press a button, or step a select forward.
    void activate() {
        Rml::ElementList r = rows();
        if (r.empty()) return;
        Rml::Element* row = r[std::clamp(focus, 0, int(r.size()) - 1)];
        if (row->IsClassSet("button") || row->IsClassSet("action")) {
            row->Click();
            return;
        }
        if (row->IsClassSet("bind")) {
            start_capture(row->GetAttribute<int>("bind", -1), bind_col);
            return;
        }
        if (row->IsClassSet("textfield")) {
            start_dns_edit();
            return;
        }
        Rml::ElementList controls;
        row->QuerySelectorAll(controls, "select");
        if (!controls.empty()) {
            auto* sel = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(controls[0]);
            if (sel) sel->SetSelection((sel->GetSelection() + 1) % std::max(1, sel->GetNumOptions()));
            return;
        }
        adjust(+1);
    }
};

Menu::Menu() : impl_(std::make_unique<Impl>()) {}
Menu::~Menu() { shutdown(); }

bool Menu::init(SDL_Window* window, Settings* settings, bool compute_available) {
    Impl& m = *impl_;
    m.window = window;
    m.settings = settings;
    m.changed = &changed;
    m.compute_available = compute_available;
    RmlGL3::Initialize();  // no-op: the GL functions come from melonDS's glad
    m.render = std::make_unique<RenderInterface_GL3>();
    if (!*m.render) {
        std::fprintf(stderr, "ui: the OpenGL renderer could not start (needs OpenGL 3.3)\n");
        return false;
    }
    m.system = std::make_unique<System>(window);
    Rml::SetFileInterface(&m.files);
    Rml::SetSystemInterface(m.system.get());
    Rml::SetRenderInterface(m.render.get());
    if (!Rml::Initialise()) return false;

    struct Font {
        const char* file;
        const char* family;
        Rml::Style::FontWeight weight;
    };
    for (const Font& f : {Font{"fonts/Cinzel.ttf", "Cinzel", Rml::Style::FontWeight::Auto},
                          Font{"fonts/AlegreyaSans-Regular.ttf", "Alegreya Sans", Rml::Style::FontWeight::Normal},
                          Font{"fonts/AlegreyaSans-Bold.ttf", "Alegreya Sans", Rml::Style::FontWeight::Bold}}) {
        const Asset* a = find_asset(f.file);
        if (!a || !Rml::LoadFontFace({reinterpret_cast<const Rml::byte*>(a->data), a->size}, f.family,
                                     Rml::Style::FontStyle::Normal, f.weight))
            std::fprintf(stderr, "ui: cannot load font %s\n", f.file);
    }

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    m.context = Rml::CreateContext("main", {std::max(w, 1), std::max(h, 1)});
    if (!m.context) return false;

    Rml::DataModelConstructor c = m.context->CreateDataModel("settings");
    State& s = m.state;
    c.Bind("tab", &m.tab);
    c.Bind("compute_available", &m.compute_available);
    c.Bind("layout", &s.layout);
    c.Bind("swap", &s.swap);
    c.Bind("second_screen", &s.second_screen);
    c.Bind("pip_size", &s.pip_size);
    c.Bind("side_align", &s.side_align);
    c.Bind("fullscreen", &s.fullscreen);
    c.Bind("aspect", &s.aspect);
    c.Bind("hud_position", &s.hud_position);
    c.Bind("hud_size", &s.hud_size);
    c.Bind("renderer", &s.renderer);
    c.Bind("scale", &s.scale);
    c.Bind("filter", &s.filter);
    c.Bind("better_polygons", &s.better_polygons);
    c.Bind("hires_coordinates", &s.hires_coordinates);
    c.Bind("frame_rate", &s.frame_rate);
    c.Bind("volume", &s.volume);
    c.Bind("skip_logos", &s.skip_logos);
    c.Bind("fast_loading", &s.fast_loading);
    c.Bind("fast_saving", &s.fast_saving);
    c.Bind("cpu_speed", &s.cpu_speed);
    c.Bind("online_server", &s.online_server);
    c.Bind("custom_dns", &s.custom_dns);
    c.Bind("editing_dns", &m.editing_dns);
    c.Bind("online_buffer", &s.online_buffer);
    c.BindEventCallback("edit_dns", [&m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        if (!m.editing_dns) m.start_dns_edit();
    });
    c.Bind("stick_moves", &s.stick_moves);
    c.Bind("deadzone", &s.deadzone);
    c.Bind("analog_movement", &s.analog_movement);
    if (auto row = c.RegisterStruct<BindRow>()) {
        row.RegisterMember("label", &BindRow::label);
        row.RegisterMember("key", &BindRow::key);
        row.RegisterMember("pad", &BindRow::pad);
    }
    c.RegisterArray<std::vector<BindRow>>();
    c.Bind("binds", &m.binds);
    c.Bind("bind_col", &m.bind_col);
    c.Bind("capture_row", &m.capture_row);
    c.Bind("capture_col", &m.capture_col);
    c.BindEventCallback("capture", [&m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (args.size() == 2) m.start_capture(args[0].Get<int>(), args[1].Get<int>());
    });
    c.BindEventCallback("reset_controls", [&m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        m.settings->controls = input::Bindings::defaults();
        m.load_state();
        m.refresh_binds();
        *m.changed = true;
    });
    c.BindEventCallback("resume", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { close(); });
    c.BindEventCallback("quit", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        quit_requested = true;
    });
    c.BindEventCallback("set_tab", [&m](Rml::DataModelHandle h, Rml::Event&, const Rml::VariantList& args) {
        if (args.empty()) return;
        m.tab = args[0].Get<Rml::String>();
        m.focus = 0;
        h.DirtyVariable("tab");
    });
    m.model = c.GetModelHandle();

    m.document = m.context->LoadDocument("menu.rml");
    if (!m.document) {
        std::fprintf(stderr, "ui: cannot load menu.rml\n");
        return false;
    }
    return true;
}

void Menu::shutdown() {
    Impl& m = *impl_;
    if (!m.render) return;
    if (m.context) Rml::RemoveContext("main");
    m.context = nullptr;
    m.document = nullptr;
    Rml::Shutdown();
    m.render.reset();
    m.system.reset();
    open_ = false;
}

void Menu::open() {
    Impl& m = *impl_;
    if (!m.document || open_) return;
    m.load_state();
    m.refresh_binds();
    m.end_capture();
    if (m.editing_dns) m.end_dns_edit();
    m.focus = 0;
    m.document->Show();
    m.context->Update();  // lay the page out now, so input that arrives before the first draw finds its rows
    open_ = true;
}

void Menu::close() {
    Impl& m = *impl_;
    if (!open_) return;
    if (m.editing_dns) m.end_dns_edit();
    if (m.store_state()) changed = true;  // (a change made since the last frame is kept)
    m.document->Hide();
    open_ = false;
}

void Menu::reload() {
    if (!open_) return;
    impl_->load_state();
    impl_->refresh_binds();
}

void Menu::set_tab(const char* name) {
    Impl& m = *impl_;
    m.tab = name;
    m.focus = 0;
    m.model.DirtyVariable("tab");
}

void Menu::handle_event(const SDL_Event& event) {
    Impl& m = *impl_;
    if (!open_ || !m.context) return;
    SDL_Event e = event;
    if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) m.refresh_binds();
    if (m.capture_row >= 0) {
        m.capture_event(e);
        if (e.type != SDL_EVENT_MOUSE_MOTION) return;
    }
    if (m.editing_dns) {
        m.dns_event(e);
        if (e.type != SDL_EVENT_MOUSE_MOTION) return;
    }
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        switch (e.key.key) {
        case SDLK_ESCAPE: close(); break;
        case SDLK_UP: m.focus--; break;
        case SDLK_DOWN: m.focus++; break;
        case SDLK_LEFT: m.adjust(-1); break;
        case SDLK_RIGHT: m.adjust(+1); break;
        case SDLK_RETURN:
        case SDLK_SPACE: m.activate(); break;
        case SDLK_Q:
        case SDLK_PAGEUP: m.switch_tab(-1); break;
        case SDLK_E:
        case SDLK_PAGEDOWN:
        case SDLK_TAB: m.switch_tab(+1); break;
        default: break;
        }
        return;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        // Nintendo positions, as in the game: the right face button confirms and the bottom one goes back.
        switch (e.gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: m.focus--; break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: m.focus++; break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: m.adjust(-1); break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: m.adjust(+1); break;
        case SDL_GAMEPAD_BUTTON_EAST: m.activate(); break;
        case SDL_GAMEPAD_BUTTON_SOUTH:
        case SDL_GAMEPAD_BUTTON_START:
        case SDL_GAMEPAD_BUTTON_GUIDE: close(); break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: m.switch_tab(-1); break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: m.switch_tab(+1); break;
        default: break;
        }
        return;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (e.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTX && e.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTY) return;
        const int v = e.gaxis.value;
        if (std::abs(v) < 16000) {
            m.stick_held = false;
            return;
        }
        if (m.stick_held) return;
        m.stick_held = true;
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)
            m.focus += v > 0 ? 1 : -1;
        else
            m.adjust(v > 0 ? 1 : -1);
        return;
    }
    case SDL_EVENT_MOUSE_MOTION:  // RmlUi's backend scales mouse positions to pixels itself
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: break;
    default: return;  // other window events are handled by the host
    }
    RmlSDL::InputEventHandler(m.context, m.window, e);
}

void Menu::render(int width, int height) {
    Impl& m = *impl_;
    if (!open_ || !m.context) return;
    if (m.context->GetDimensions() != Rml::Vector2i{width, height}) m.context->SetDimensions({width, height});
    m.context->SetDensityIndependentPixelRatio(std::max(0.75f, height / 720.0f));
    if (m.capture_row >= 0 && SDL_GetTicks() - m.capture_started > 8000) m.end_capture();  // gave up waiting
    m.update_focus();
    m.context->Update();
    if (m.store_state()) changed = true;
    m.render->SetViewport(width, height);
    m.render->BeginFrame();
    m.context->Render();
    m.render->EndFrame();
}

}  // namespace ui
