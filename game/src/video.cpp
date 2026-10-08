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
uniform float uLayer;
in vec2 vPos;        // unit quad, (0,0) = top left
out vec3 fTex;
void main() {
    gl_Position = vec4(mix(uDest.xy, uDest.zw, vPos), 0.0, 1.0);
    fTex = vec3(vPos, uLayer);
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

void Presenter::save_png(const char* path) {
    SDL_Surface* surf = SDL_CreateSurface(width_, height_, SDL_PIXELFORMAT_ABGR8888);
    if (!surf) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, target_);
    glReadBuffer(target_ ? GL_COLOR_ATTACHMENT0 : GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, surf->pitch / 4);
    glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, surf->pixels);
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
    if (s.layout == Layout::Single) {
        // The main screen as large as fits; the other in the bottom right corner, over the main one if it must.
        const float k = std::min(float(w) / kScreenW, float(h) / kScreenH);
        const float sw = kScreenW * k, sh = kScreenH * k;
        views_[0] = {first, {(w - sw) / 2, (h - sh) / 2, sw, sh}, 1.0f, false};
        view_count_ = 1;
        if (s.second_screen) {
            const float ph = std::min(h * std::clamp(s.pip_size, 0.1f, 0.5f), float(w) * kScreenH / kScreenW / 2);
            const float pw = ph * kScreenW / kScreenH, margin = std::round(h * 0.015f);
            views_[1] = {second, {w - pw - margin, h - ph - margin, pw, ph}, std::clamp(s.pip_opacity, 0.2f, 1.0f), true};
            view_count_ = 2;
        }
        return;
    }
    if (s.layout == Layout::MainSide) {
        // The main screen as tall as the window allows while leaving the side screen at least 30% of its width,
        // pinned to the left; the side screen takes the rest of the width, aligned to the main screen's top, middle
        // or bottom.
        const float k = std::min(float(h) / kScreenH, float(w) / (kScreenW * 1.3f));
        const Rect main{0, std::round((h - kScreenH * k) / 2), kScreenW * k, kScreenH * k};
        float sw = w - main.w, sh = sw * kScreenH / kScreenW;
        if (sh > h) {
            sh = float(h);
            sw = sh * kScreenW / kScreenH;
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
    const float k = stacked ? std::min(float(w) / kScreenW, float(h) / (kScreenH * 2))
                            : std::min(float(w) / (kScreenW * 2), float(h) / kScreenH);
    const float sw = kScreenW * k, sh = kScreenH * k;
    const float x = (w - (stacked ? sw : sw * 2)) / 2, y = (h - (stacked ? sh * 2 : sh)) / 2;
    views_[0] = {first, {x, y, sw, sh}, 1.0f, false};
    views_[1] = {second, {stacked ? x : x + sw, stacked ? y + sh : y, sw, sh}, 1.0f, false};
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
    return h;
}

void Presenter::draw(const ntr::Frame* frame, Filter filter) {
    // melonDS's GL renderers share this context: put back the state the screen pass relies on.
#ifdef _WIN32
    target_ = flip_ ? flip_->begin(width_, height_) : 0;
#endif
    glBindFramebuffer(GL_FRAMEBUFFER, target_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, width_, height_);
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
        for (int i = 0; i < view_count_; i++) {
            const ScreenView& v = views_[i];
            const Rect& r = v.rect;
            if (v.framed && v.opacity >= 1.0f) {
                // A thin border sets the picture-in-picture apart from the screen under it.
                const int b = std::max(2, height_ / 400);
                glEnable(GL_SCISSOR_TEST);
                glScissor(int(r.x) - b, height_ - int(r.y + r.h) - b, int(r.w) + 2 * b, int(r.h) + 2 * b);
                glClearColor(0.55f, 0.6f, 0.7f, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                glDisable(GL_SCISSOR_TEST);
            }
            if (v.opacity < 1.0f) glEnable(GL_BLEND);
            glUniform4f(u_dest_, r.x / width_ * 2 - 1, 1 - r.y / height_ * 2, (r.x + r.w) / width_ * 2 - 1,
                        1 - (r.y + r.h) / height_ * 2);
            glUniform1f(u_layer_, float(v.screen));
            glUniform2f(u_out_size_, r.w, r.h);
            glUniform1f(u_opacity_, v.opacity);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glDisable(GL_BLEND);
        }
        glUseProgram(0);
    }
}

void Presenter::swap(const char* screenshot) {
    if (screenshot) save_png(screenshot);
#ifdef _WIN32
    if (flip_) {
        flip_->present();
        return;
    }
#endif
    SDL_GL_SwapWindow(window_);
}

}  // namespace video
