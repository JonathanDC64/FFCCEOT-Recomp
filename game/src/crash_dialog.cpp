#include "crash_dialog.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <memory>

#include "OpenGLSupport.h"  // melonDS: glad
#include "ui/host.hpp"
#include "ui/modal.hpp"
#include "ui/ui.hpp"

namespace crash_dialog {

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
    if (ui::host::init(window) && ui::initialize()) {
        bool done = false;
        const auto dismiss = [&done](ui::Modal&) { done = true; };
        ui::push_document(std::make_unique<ui::Modal>(ui::Modal::Props{
            .title = "The game has crashed",
            .bodyRml = "<b>" + ui::escape(content.what) +
                       "</b><br/><br/>A report was saved on this PC. Reporting the problem opens a new issue on "
                       "GitHub with the details filled in, and a zip to attach with the full stack, your game "
                       "settings (settings.ini) and the game's recent messages. It has no save data or game "
                       "files; nothing is sent until you submit the issue.",
            .actions =
                {
                    ui::ModalAction{
                        .label = "Restart game",
                        .onPressed =
                            [&](ui::Modal&) {
                                if (actions.restart) actions.restart();
                                done = true;
                            },
                    },
                    ui::ModalAction{
                        .label = "Report issue",
                        .onPressed =
                            [&](ui::Modal& modal) {
                                if (actions.report) modal.set_body_text(actions.report());
                            },
                    },
                    ui::ModalAction{
                        .label = "Show details",
                        .onPressed =
                            [&](ui::Modal& modal) {
                                modal.push(std::make_unique<ui::Modal>(ui::Modal::Props{
                                    .title = "Details",
                                    .bodyRml = ui::escape(content.details),
                                    .actions = {ui::ModalAction{
                                        .label = "Close", .onPressed = [](ui::Modal& details) { details.pop(); }}},
                                    .onDismiss = [](ui::Modal& details) { details.pop(); },
                                }));
                            },
                    },
                    ui::ModalAction{.label = "Close", .onPressed = dismiss},
                },
            .onDismiss = dismiss,
            .variant = "danger",
            .icon = "error",
        }));
        ok = true;
        while (!done) {
            SDL_Event e;
            if (!SDL_WaitEventTimeout(&e, 100)) e.type = 0;
            do {
                if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) done = true;
                if (e.type != 0) ui::handle_event(e);
            } while (SDL_PollEvent(&e));
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            ui::update();
            glViewport(0, 0, w, h);
            glClearColor(0.02f, 0.04f, 0.09f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ui::host::render(w, h);
            SDL_GL_SwapWindow(window);
        }
        ui::shutdown();
    }
    ui::host::shutdown();
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return ok;
}

}  // namespace crash_dialog
