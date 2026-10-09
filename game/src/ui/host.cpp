#include "host.hpp"

#include <RmlUi/Core.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "RmlUi_Platform_SDL.h"
#include "RmlUi_Renderer_GL3.h"
#include "main_thread.hpp"
#include "ui_assets.hpp"

namespace ui::host {
namespace {

// "res/rml/../prelaunch-bg.png" -> "res/prelaunch-bg.png"
std::string normalize(const std::string& path) {
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t slash = path.find('/', pos);
        if (slash == std::string::npos) slash = path.size();
        const std::string part = path.substr(pos, slash - pos);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        pos = slash + 1;
    }
    std::string out;
    for (const std::string& p : parts) out += (out.empty() ? "" : "/") + p;
    return out;
}

// Serves the embedded UI files to RmlUi.
class EmbeddedFiles final : public Rml::FileInterface {
    struct Handle {
        const Asset* asset;
        size_t pos;
    };

public:
    Rml::FileHandle Open(const Rml::String& path) override {
        const Asset* a = find_asset(normalize(path));
        if (!a) return 0;
        return reinterpret_cast<Rml::FileHandle>(new Handle{a, 0});
    }
    void Close(Rml::FileHandle file) override { delete reinterpret_cast<Handle*>(file); }
    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override {
        auto* f = reinterpret_cast<Handle*>(file);
        const size_t n = std::min(size, f->asset->size - f->pos);
        std::memcpy(buffer, f->asset->data + f->pos, n);
        f->pos += n;
        return n;
    }
    bool Seek(Rml::FileHandle file, long offset, int origin) override {
        auto* f = reinterpret_cast<Handle*>(file);
        const long base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? long(f->pos) : long(f->asset->size);
        const long p = base + offset;
        if (p < 0 || size_t(p) > f->asset->size) return false;
        f->pos = size_t(p);
        return true;
    }
    size_t Tell(Rml::FileHandle file) override { return reinterpret_cast<Handle*>(file)->pos; }
};

// RmlUi's SDL system interface, with its log messages printed. The window's text input belongs to the thread that
// owns the window; the UI runs on the game's.
class System final : public SystemInterface_SDL {
public:
    using SystemInterface_SDL::SystemInterface_SDL;
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        if (type <= Rml::Log::LT_WARNING) std::fprintf(stderr, "ui: %s\n", message.c_str());
        return true;
    }
    void ActivateKeyboard(Rml::Vector2f caret_position, float line_height) override {
        main_thread::run([=, this] { SystemInterface_SDL::ActivateKeyboard(caret_position, line_height); }, false);
    }
    void DeactivateKeyboard() override {
        main_thread::run([this] { SystemInterface_SDL::DeactivateKeyboard(); }, false);
    }
};

// The GL3 renderer, which also reads PNG images (its own loader reads TGA only).
class Renderer final : public RenderInterface_GL3 {
public:
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override {
        if (source.size() < 4 || source.compare(source.size() - 4, 4, ".png") != 0)
            return RenderInterface_GL3::LoadTexture(dimensions, source);
        const Asset* a = find_asset(normalize(source));
        if (!a) return 0;
        SDL_Surface* png = SDL_LoadPNG_IO(SDL_IOFromConstMem(a->data, a->size), true);
        if (!png) return 0;
        SDL_Surface* rgba = SDL_ConvertSurface(png, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(png);
        if (!rgba) return 0;
        // RmlUi wants premultiplied alpha.
        for (int y = 0; y < rgba->h; y++) {
            auto* row = static_cast<unsigned char*>(rgba->pixels) + size_t(y) * size_t(rgba->pitch);
            for (int x = 0; x < rgba->w; x++) {
                const unsigned a8 = row[x * 4 + 3];
                for (int c = 0; c < 3; c++) row[x * 4 + c] = static_cast<unsigned char>(row[x * 4 + c] * a8 / 255);
            }
        }
        dimensions = {rgba->w, rgba->h};
        std::vector<Rml::byte> bytes(size_t(rgba->w) * size_t(rgba->h) * 4);
        for (int y = 0; y < rgba->h; y++)
            std::memcpy(bytes.data() + size_t(y) * size_t(rgba->w) * 4,
                        static_cast<unsigned char*>(rgba->pixels) + size_t(y) * size_t(rgba->pitch), size_t(rgba->w) * 4);
        SDL_DestroySurface(rgba);
        return GenerateTexture({bytes.data(), bytes.size()}, dimensions);
    }
};

struct State {
    SDL_Window* window = nullptr;
    EmbeddedFiles files;
    std::unique_ptr<System> system;
    std::unique_ptr<Renderer> renderer;
    Rml::Context* context = nullptr;
    float ui_scale = 0.0f;
    bool initialised = false;
};
State s;

void load_fonts() {
    struct Font {
        const char* file;
        const char* family;
        Rml::Style::FontWeight weight;
        bool fallback;
    };
    for (const Font& f : {Font{"res/fonts/AlegreyaSans-Regular.ttf", "Alegreya Sans", Rml::Style::FontWeight::Normal, true},
                          Font{"res/fonts/AlegreyaSans-Bold.ttf", "Alegreya Sans", Rml::Style::FontWeight::Bold, false},
                          Font{"res/fonts/Cinzel.ttf", "Cinzel", Rml::Style::FontWeight::Auto, false},
                          Font{"res/fonts/MaterialSymbolsRounded-Regular.ttf", "Material Symbols Rounded",
                               Rml::Style::FontWeight::Auto, false}}) {
        const Asset* a = find_asset(f.file);
        if (!a || !Rml::LoadFontFace({reinterpret_cast<const Rml::byte*>(a->data), a->size}, f.family,
                                     Rml::Style::FontStyle::Normal, f.weight, f.fallback))
            std::fprintf(stderr, "ui: cannot load font %s\n", f.file);
    }
}

}  // namespace

bool init(SDL_Window* window) {
    if (s.initialised) return true;
    s.window = window;
    RmlGL3::Initialize();  // no-op: the GL functions come from melonDS's glad
    s.renderer = std::make_unique<Renderer>();
    if (!*s.renderer) {
        std::fprintf(stderr, "ui: the OpenGL renderer could not start (needs OpenGL 3.3)\n");
        return false;
    }
    s.system = std::make_unique<System>(window);
    Rml::SetFileInterface(&s.files);
    Rml::SetSystemInterface(s.system.get());
    Rml::SetRenderInterface(s.renderer.get());
    if (!Rml::Initialise()) return false;
    load_fonts();
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    s.context = Rml::CreateContext("main", {std::max(w, 1), std::max(h, 1)});
    if (!s.context) return false;
    s.initialised = true;
    return true;
}

void shutdown() {
    if (!s.initialised) return;
    Rml::Shutdown();
    s.context = nullptr;
    s.renderer.reset();
    s.system.reset();
    s.initialised = false;
}

bool is_initialized() { return s.initialised; }
Rml::Context* context() { return s.context; }
SDL_Window* window() { return s.window; }

WindowSize window_size() {
    WindowSize size;
    if (!s.window) return size;
    SDL_GetWindowSizeInPixels(s.window, &size.width, &size.height);
    const float scale = SDL_GetWindowDisplayScale(s.window);
    size.scale = scale > 0.0f ? scale : 1.0f;
    return size;
}

void set_ui_scale(float scale) { s.ui_scale = std::clamp(scale, 0.0f, 4.0f); }

void forward_event(const SDL_Event& event) {
    if (!s.context) return;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_MOUSE_MOTION:  // RmlUi's backend scales mouse positions to pixels itself
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: break;
    default: return;  // other window events are handled by the program
    }
    SDL_Event copy = event;
    RmlSDL::InputEventHandler(s.context, s.window, copy);
}

void render(int width, int height) {
    if (!s.context) return;
    if (s.context->GetDimensions() != Rml::Vector2i{width, height}) s.context->SetDimensions({width, height});
    // Dusklight's (aurora's) rule: the user's scale, or the display's.
    s.context->SetDensityIndependentPixelRatio(s.ui_scale > 0.0f ? s.ui_scale : window_size().scale);
    s.context->Update();
    s.renderer->SetViewport(width, height);
    s.renderer->BeginFrame();
    s.context->Render();
    s.renderer->EndFrame();
}

void set_input_type(InputType) {}

bool animations_enabled() {
    static const bool enabled = !std::getenv("FFCC_NO_UI_ANIM");
    return enabled;
}

}  // namespace ui::host
