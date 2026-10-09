#include "video.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "OpenGLSupport.h"  // melonDS: glad and the shader compile helper
#ifdef _WIN32
#include "flip_present.hpp"
#endif

namespace video {

namespace {

constexpr int kScreenW = 256, kScreenH = 192;

const char* kVS = R"(#version 140
uniform vec4 uDest;  // x0, y0, x1, y1 in clip space (y0 = top edge)
uniform vec2 uUvX;   // the part of the texture's width the screen shows
uniform float uLayer;
in vec2 vPos;        // unit quad, (0,0) = top left
out vec3 fTex;
void main() {
    gl_Position = vec4(mix(uDest.xy, uDest.zw, vPos), 0.0, 1.0);
    fTex = vec3(mix(uUvX.x, uUvX.y, vPos.x), vPos.y, uLayer);
}
)";

// Sharp bilinear (after Themaister's shader): magnify each texel by the largest integer factor that fits and blend
// only across the remaining fraction of an output pixel at texel edges. Needs the texture's filter set to linear.
const char* kFS = R"(#version 140
uniform sampler2DArray uTex;
uniform int uSharp;
uniform vec2 uOutSize;  // the screen's size on the window, in pixels
uniform float uOpacity;
in vec3 fTex;
out vec4 oColor;
void main() {
    vec2 uv = fTex.xy;
    if (uSharp != 0) {
        vec2 size = vec2(textureSize(uTex, 0).xy);
        vec2 texel = uv * size;
        vec2 scale = max(floor(uOutSize / size), vec2(1.0));
        vec2 d = fract(texel) - 0.5;
        vec2 range = 0.5 - 0.5 / scale;
        uv = (floor(texel) + (d - clamp(d, -range, range)) * scale + 0.5) / size;
    }
    oColor = vec4(texture(uTex, vec3(uv, fTex.z)).rgb, uOpacity);
}
)";

}  // namespace

void Presenter::save_png(const char* path, unsigned target, int width, int height) {
    SDL_Surface* surf = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_ABGR8888);
    if (!surf) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, target);
    glReadBuffer(target ? GL_COLOR_ATTACHMENT0 : GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, surf->pitch / 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, surf->pixels);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    SDL_FlipSurface(surf, SDL_FLIP_VERTICAL);  // GL rows run bottom-up
    if (!SDL_SavePNG(surf, path)) std::fprintf(stderr, "cannot save %s: %s\n", path, SDL_GetError());
    SDL_DestroySurface(surf);
}

const char* layout_name(Layout l) {
    switch (l) {
    case Layout::Single: return "single screen";
    case Layout::MainSide: return "main + side";
    case Layout::Stacked: return "stacked";
    case Layout::SideBySide: return "side by side";
    case Layout::Windows: return "two windows";
    }
    return "?";
}

const char* align_name(Align a) {
    switch (a) {
    case Align::Top: return "top";
    case Align::Middle: return "middle";
    case Align::Bottom: return "bottom";
    }
    return "?";
}

const char* filter_name(Filter f) {
    switch (f) {
    case Filter::Nearest: return "nearest";
    case Filter::Linear: return "linear";
    case Filter::Sharp: return "sharp";
    }
    return "?";
}

#ifndef _WIN32
class FlipPresenter {};  // Windows only
#endif

Presenter::Presenter() = default;
Presenter::~Presenter() { shutdown(); }

bool Presenter::init(SDL_Window* window, bool allow_flip_model) {
    window_ = window;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    // 4.3 adds the compute shaders melonDS's compute renderer needs; 3.3 is enough for everything else, including the menu.
    for (auto [major, minor] : {std::pair{4, 3}, std::pair{3, 3}}) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
        if ((context_ = SDL_GL_CreateContext(window))) break;
    }
    if (!context_) {
        std::fprintf(stderr, "cannot create an OpenGL 3.3 context: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(window, static_cast<SDL_GLContext>(context_));
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress))) {
        std::fprintf(stderr, "cannot load the OpenGL functions\n");
        return false;
    }
    gl43_ = GLAD_GL_VERSION_4_3 != 0;
    SDL_GL_SetSwapInterval(0);  // the frontend paces frames to the DS refresh rate itself
    std::printf("OpenGL %s (%s)\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    GLuint prog = 0;
    if (!melonDS::OpenGL::CompileVertexFragmentProgram(prog, kVS, kFS, "ScreenShader", {{"vPos", 0}}, {{"oColor", 0}}))
        return false;
    program_ = prog;
    u_dest_ = glGetUniformLocation(prog, "uDest");
    u_layer_ = glGetUniformLocation(prog, "uLayer");
    u_sharp_ = glGetUniformLocation(prog, "uSharp");
    u_out_size_ = glGetUniformLocation(prog, "uOutSize");
    u_tex_ = glGetUniformLocation(prog, "uTex");
    u_opacity_ = glGetUniformLocation(prog, "uOpacity");
    u_uv_x_ = glGetUniformLocation(prog, "uUvX");

    const float quad[] = {0, 0, 1, 0, 0, 1, 1, 1};  // triangle strip
    GLuint b, a, t;
    glGenBuffers(1, &b);
    glBindBuffer(GL_ARRAY_BUFFER, b);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glGenVertexArrays(1, &a);
    glBindVertexArray(a);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    vbo_ = b;
    vao_ = a;

    // The software renderer's two screens are uploaded into the same kind of array texture the GL renderers output.
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D_ARRAY, t);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, kScreenW, kScreenH, 2, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    soft_tex_ = t;

#ifdef _WIN32
    if (allow_flip_model) {
        void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        auto flip = std::make_unique<FlipPresenter>();
        std::string error;
        if (hwnd && flip->init(hwnd, error)) {
            flip_ = std::move(flip);
            std::printf("presentation: DXGI flip model\n");
        } else {
            std::printf("presentation: OpenGL swap (flip model unavailable: %s)\n", error.c_str());
        }
    } else {
        std::printf("presentation: OpenGL swap\n");
    }
#else
    (void)allow_flip_model;
#endif
    return true;
}

void Presenter::shutdown() {
    if (!context_) return;
    close_second();
    flip_.reset();
    GLuint t = soft_tex_, b = vbo_, a = vao_;
    if (t) glDeleteTextures(1, &t);
    if (b) glDeleteBuffers(1, &b);
    if (a) glDeleteVertexArrays(1, &a);
    if (program_) glDeleteProgram(program_);
    SDL_GL_DestroyContext(static_cast<SDL_GLContext>(context_));
    context_ = nullptr;
}

void Presenter::layout(int w, int h, const LayoutSettings& s) {
    width_ = w;
    height_ = h;
    const int first = s.swap ? 1 : 0, second = 1 - first;
    // Width over height of each screen: the top one is wider with widescreen.
    const float standard = float(kScreenW) / kScreenH;
    const float aspect[2] = {wide_extra_ ? float(kScreenW + 2 * wide_extra_) / kScreenH : standard, standard};
    if (s.layout == Layout::Single || s.layout == Layout::Windows) {
        // The main screen as large as fits; the other in the bottom right corner, over the main one if it must.
        const float sh = std::min(float(h), w / aspect[first]), sw = sh * aspect[first];
        views_[0] = {first, {(w - sw) / 2, (h - sh) / 2, sw, sh}, 1.0f, false};
        view_count_ = 1;
        if (s.second_screen && s.layout == Layout::Single) {
            const float ph = std::min(h * std::clamp(s.pip_size, 0.1f, 0.5f), w / aspect[second] / 2);
            const float pw = ph * aspect[second], margin = std::round(h * 0.015f);
            views_[1] = {second, {w - pw - margin, h - ph - margin, pw, ph}, std::clamp(s.pip_opacity, 0.2f, 1.0f), true};
            view_count_ = 2;
        }
        return;
    }
    if (s.layout == Layout::MainSide) {
        // The main screen as tall as the window allows while leaving the side screen at least 30% of its width,
        // pinned to the left; the side screen takes the rest of the width, aligned to the main screen's top, middle
        // or bottom.
        const float mh = std::min(float(h), w / (aspect[first] * 1.3f));
        const Rect main{0, std::round((h - mh) / 2), mh * aspect[first], mh};
        float sw = w - main.w, sh = sw / aspect[second];
        if (sh > h) {
            sh = float(h);
            sw = sh * aspect[second];
        }
        const float sx = main.w + (w - main.w - sw) / 2;
        const float sy = s.side_align == Align::Top      ? main.y
                         : s.side_align == Align::Bottom ? main.y + main.h - sh
                                                         : (h - sh) / 2;
        views_[0] = {first, main, 1.0f, false};
        views_[1] = {second, {sx, std::clamp(sy, 0.0f, h - sh), sw, sh}, 1.0f, false};
        view_count_ = 2;
        return;
    }
    const bool stacked = s.layout == Layout::Stacked;
    const float wa = aspect[first], wb = aspect[second];
    const float sh = stacked ? std::min(float(h) / 2, w / std::max(wa, wb)) : std::min(float(h), w / (wa + wb));
    const float wa_px = sh * wa, wb_px = sh * wb;
    if (stacked) {
        const float y = (h - sh * 2) / 2;
        views_[0] = {first, {(w - wa_px) / 2, y, wa_px, sh}, 1.0f, false};
        views_[1] = {second, {(w - wb_px) / 2, y + sh, wb_px, sh}, 1.0f, false};
    } else {
        const float x = (w - wa_px - wb_px) / 2, y = (h - sh) / 2;
        views_[0] = {first, {x, y, wa_px, sh}, 1.0f, false};
        views_[1] = {second, {x + wa_px, y, wb_px, sh}, 1.0f, false};
    }
    view_count_ = 2;
}

int Presenter::screen_at(float px, float py, float* local_x, float* local_y) const {
    for (int i = view_count_ - 1; i >= 0; i--) {
        const Rect& r = views_[i].rect;
        if (!r.contains(px, py)) continue;
        *local_x = (px - r.x) / r.w * kScreenW;
        *local_y = (py - r.y) / r.h * kScreenH;
        return views_[i].screen;
    }
    return -1;
}

float Presenter::largest_screen_height() const {
    float h = 0;
    for (int i = 0; i < view_count_; i++) h = std::max(h, views_[i].rect.h);
    if (window2_) h = std::max(h, view2_.rect.h);
    return h;
}

void Presenter::draw(const ntr::Frame* frame, Filter filter) {
#ifdef _WIN32
    target_ = flip_ ? flip_->begin(width_, height_) : 0;
#endif
    draw_views(target_, width_, height_, views_, view_count_, frame, filter);
}

void Presenter::draw_views(unsigned target, int width, int height, const ScreenView* views, int count,
                           const ntr::Frame* frame, Filter filter) {
    // melonDS's GL renderers share this context: put back the state the screen pass relies on.
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    if (frame) {
        glActiveTexture(GL_TEXTURE0);
        if (frame->top) {
            glBindTexture(GL_TEXTURE_2D_ARRAY, soft_tex_);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, kScreenW, kScreenH, 1, GL_BGRA, GL_UNSIGNED_BYTE, frame->top);
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1, kScreenW, kScreenH, 1, GL_BGRA, GL_UNSIGNED_BYTE,
                            frame->bottom);
        } else {
            glBindTexture(GL_TEXTURE_2D_ARRAY, frame->gl_texture);
        }
        const GLint f = filter == Filter::Nearest ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, f);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, f);

        glUseProgram(program_);
        glUniform1i(u_tex_, 0);
        glUniform1i(u_sharp_, filter == Filter::Sharp);
        glBindVertexArray(vao_);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        for (int i = 0; i < count; i++) {
            const ScreenView& v = views[i];
            const Rect& r = v.rect;
            if (v.framed && v.opacity >= 1.0f) {
                // A thin border sets the picture-in-picture apart from the screen under it.
                const int b = std::max(2, height / 400);
                glEnable(GL_SCISSOR_TEST);
                glScissor(int(r.x) - b, height - int(r.y + r.h) - b, int(r.w) + 2 * b, int(r.h) + 2 * b);
                glClearColor(0.55f, 0.6f, 0.7f, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                glDisable(GL_SCISSOR_TEST);
            }
            if (v.opacity < 1.0f) glEnable(GL_BLEND);
            // The renderer's output is wider with widescreen: the bottom screen is its middle part.
            const bool wide = frame->wide_extra > 0 && !frame->top;
            const float inner = wide ? float(kScreenW) / (kScreenW + 2 * frame->wide_extra) : 1.0f;
            const float u0 = v.screen == 1 ? (1 - inner) / 2 : 0.0f, u1 = v.screen == 1 ? 1 - u0 : 1.0f;
            glUniform2f(u_uv_x_, u0, u1);
            const Rect& pr = r;
            glUniform4f(u_dest_, pr.x / width * 2 - 1, 1 - pr.y / height * 2, (pr.x + pr.w) / width * 2 - 1,
                        1 - (pr.y + pr.h) / height * 2);
            glUniform1f(u_layer_, float(v.screen));
            glUniform2f(u_out_size_, v.screen == 1 && inner < 1.0f ? pr.w / inner : pr.w, pr.h);
            glUniform1f(u_opacity_, v.opacity);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glDisable(GL_BLEND);
        }
        glUseProgram(0);
    }
}

bool Presenter::open_second(SDL_Window* window, bool allow_flip_model) {
    close_second();
    window2_ = window;
#ifdef _WIN32
    if (allow_flip_model && flip_) {  // (the first window's choice: a GL swap if it had none)
        void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        auto flip = std::make_unique<FlipPresenter>();
        std::string error;
        if (hwnd && flip->init(hwnd, error))
            flip2_ = std::move(flip);
        else
            std::printf("second window: OpenGL swap (flip model unavailable: %s)\n", error.c_str());
    }
#else
    (void)allow_flip_model;
#endif
    return true;
}

void Presenter::close_second() {
    flip2_.reset();
    window2_ = nullptr;
}

void Presenter::layout_second(int w, int h, const LayoutSettings& s) {
    width2_ = w;
    height2_ = h;
    const int second = s.swap ? 0 : 1;  // the screen that is not the main window's
    const float aspect = second == 0 && wide_extra_ ? float(kScreenW + 2 * wide_extra_) / kScreenH
                                                    : float(kScreenW) / kScreenH;
    const float sh = std::min(float(h), w / aspect), sw = sh * aspect;
    view2_ = {second, {(w - sw) / 2, (h - sh) / 2, sw, sh}, 1.0f, false};
}

int Presenter::screen_at_second(float px, float py, float* local_x, float* local_y) const {
    if (!window2_ || !view2_.rect.contains(px, py)) return -1;
    *local_x = (px - view2_.rect.x) / view2_.rect.w * kScreenW;
    *local_y = (py - view2_.rect.y) / view2_.rect.h * kScreenH;
    return view2_.screen;
}

void Presenter::show_second(const ntr::Frame* frame, Filter filter, const char* screenshot) {
    unsigned target = 0;
#ifdef _WIN32
    if (flip2_) target = flip2_->begin(width2_, height2_);
#endif
    if (!flip2_) {
        SDL_GL_MakeCurrent(window2_, static_cast<SDL_GLContext>(context_));
        SDL_GL_SetSwapInterval(0);
    }
    draw_views(target, width2_, height2_, &view2_, 1, frame, filter);
    if (screenshot) {  // (named like the first window's, with -window2 before the extension)
        std::string path = screenshot;
        const size_t dot = path.rfind('.');
        path.insert(dot == std::string::npos ? path.size() : dot, "-window2");
        save_png(path.c_str(), target, width2_, height2_);
    }
#ifdef _WIN32
    if (flip2_) {
        flip2_->present(false);  // (at once: the first window's vsync is the one that paces the frames)
        return;
    }
#endif
    SDL_GL_SwapWindow(window2_);
    SDL_GL_MakeCurrent(window_, static_cast<SDL_GLContext>(context_));
    SDL_GL_SetSwapInterval(flip_ || !vsync_ ? 0 : 1);
}

void Presenter::swap(const char* screenshot) {
    if (screenshot) save_png(screenshot, target_, width_, height_);
#ifdef _WIN32
    if (flip_) {
        flip_->present(vsync_, tearing_);
        return;
    }
#endif
    SDL_GL_SwapWindow(window_);
}

void Presenter::set_vsync(bool on) {
    if (on == vsync_) return;
    vsync_ = on;
    if (!flip_) SDL_GL_SetSwapInterval(on ? 1 : 0);  // (a GL swap with vsync waits for the refresh itself)
}

void Presenter::wait_for_refresh() {
#ifdef _WIN32
    if (flip_ && vsync_) flip_->wait();
#endif
}

bool Presenter::tearing_supported() const {
#ifdef _WIN32
    if (flip_) return flip_->tearing_supported();
#endif
    return true;  // (a GL swap without vsync tears by itself)
}

float Presenter::refresh_rate() const {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_));
    return mode ? mode->refresh_rate : 0.0f;
}

}  // namespace video
