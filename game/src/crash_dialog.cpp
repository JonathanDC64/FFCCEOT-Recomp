#include "crash_dialog.hpp"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <memory>

#include "OpenGLSupport.h"  // melonDS: glad
#include "RmlUi_Platform_SDL.h"
#include "RmlUi_Renderer_GL3.h"
#include "ui_support.hpp"

namespace crash_dialog {

namespace {

constexpr int kButtons = 4;  // Restart game, Report on GitHub, Show details, Close

struct State {
    std::string what, details, message;
    bool show_details = false;
    bool reported = false;
    int focus = 1;  // the button to press with Enter: reporting is what we want
    bool done = false;
};

}  // namespace

bool show(const Content& content, const Actions& actions) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "crash dialog: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_Window* window = SDL_CreateWindow("Echoes of Time", 900, 640, SDL_WINDOW_OPENGL | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_GLContext gl = window ? SDL_GL_CreateContext(window) : nullptr;
    if (!gl || !gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress))) {
        std::fprintf(stderr, "crash dialog: no OpenGL: %s\n", SDL_GetError());
        if (gl) SDL_GL_DestroyContext(gl);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }
    SDL_GL_SetSwapInterval(1);

    bool ok = false;
    {
        State state;
        state.what = content.what;
        state.details = content.details;
        auto render = std::make_unique<RenderInterface_GL3>();
        ui::EmbeddedFiles files;
        auto system = std::make_unique<ui::System>(window);
        Rml::SetFileInterface(&files);
        Rml::SetSystemInterface(system.get());
        Rml::SetRenderInterface(render.get());
        if (*render && Rml::Initialise()) {
            ui::load_fonts();
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            Rml::Context* context = Rml::CreateContext("crash", {std::max(w, 1), std::max(h, 1)});
            Rml::DataModelConstructor c = context->CreateDataModel("crash");
            c.Bind("what", &state.what);
            c.Bind("details", &state.details);
            c.Bind("message", &state.message);
            c.Bind("show_details", &state.show_details);
            c.Bind("reported", &state.reported);
            c.Bind("focus", &state.focus);
            Rml::DataModelHandle model = c.GetModelHandle();
            const auto act = [&](int button) {
                switch (button) {
                case 0:
                    if (actions.restart) actions.restart();
                    state.done = true;
                    break;
                case 1:
                    if (actions.report) {
                        state.message = actions.report();
                        state.reported = true;
                        model.DirtyVariable("message");
                        model.DirtyVariable("reported");
                    }
                    break;
                case 2:
                    state.show_details = !state.show_details;
                    model.DirtyVariable("show_details");
                    break;
                default: state.done = true; break;
                }
            };
            c.BindEventCallback("restart", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { act(0); });
            c.BindEventCallback("report", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { act(1); });
            c.BindEventCallback("toggle_details", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { act(2); });
            c.BindEventCallback("close", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { act(3); });
            Rml::ElementDocument* document = context->LoadDocument("crash.rml");
            if (document) {
                document->Show();
                ok = true;
                while (!state.done) {
                    SDL_Event e;
                    if (!SDL_WaitEventTimeout(&e, 100)) e.type = 0;
                    do {
                        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) state.done = true;
                        if (e.type == SDL_EVENT_KEY_DOWN) {
                            switch (e.key.key) {
                            case SDLK_ESCAPE: state.done = true; break;
                            case SDLK_LEFT:
                            case SDLK_UP: state.focus = (state.focus + kButtons - 1) % kButtons; model.DirtyVariable("focus"); break;
                            case SDLK_RIGHT:
                            case SDLK_DOWN:
                            case SDLK_TAB: state.focus = (state.focus + 1) % kButtons; model.DirtyVariable("focus"); break;
                            case SDLK_RETURN:
                            case SDLK_KP_ENTER:
                            case SDLK_SPACE: act(state.focus); break;
                            default: break;
                            }
                        } else if (e.type != 0) {
                            RmlSDL::InputEventHandler(context, window, e);
                        }
                    } while (SDL_PollEvent(&e));
                    SDL_GetWindowSizeInPixels(window, &w, &h);
                    if (context->GetDimensions() != Rml::Vector2i{w, h}) context->SetDimensions({w, h});
                    context->SetDensityIndependentPixelRatio(SDL_GetWindowPixelDensity(window));  // (the window is 900 x 640 points)
                    context->Update();
                    glViewport(0, 0, w, h);
                    glClearColor(0.02f, 0.04f, 0.09f, 1.0f);
                    glClear(GL_COLOR_BUFFER_BIT);
                    render->SetViewport(w, h);
                    render->BeginFrame();
                    context->Render();
                    render->EndFrame();
                    SDL_GL_SwapWindow(window);
                }
            }
            Rml::Shutdown();
        }
    }
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return ok;
}

}  // namespace crash_dialog
