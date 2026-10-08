#include "video.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>

#include "OpenGLSupport.h"  // melonDS: glad and the shader compile helper

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
    oColor = vec4(texture(uTex, vec3(uv, fTex.z)).rgb, 1.0);
}
)";

}  // namespace

void Presenter::save_png(const char* path) {
    SDL_Surface* surf = SDL_CreateSurface(width_, height_, SDL_PIXELFORMAT_ABGR8888);
    if (!surf) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, surf->pitch / 4);
    glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, surf->pixels);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    SDL_FlipSurface(surf, SDL_FLIP_VERTICAL);  // GL rows run bottom-up
    if (!SDL_SavePNG(surf, path)) std::fprintf(stderr, "cannot save %s: %s\n", path, SDL_GetError());
    SDL_DestroySurface(surf);
}

const char* filter_name(Filter f) {
    switch (f) {
    case Filter::Nearest: return "nearest";
    case Filter::Linear: return "linear";
    case Filter::Sharp: return "sharp";
    }
    return "?";
}

bool Presenter::init(SDL_Window* window) {
    window_ = window;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    // 4.3 adds the compute shaders melonDS's compute renderer needs; 3.2 is enough for everything else.
    for (auto [major, minor] : {std::pair{4, 3}, std::pair{3, 2}}) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
        if ((context_ = SDL_GL_CreateContext(window))) break;
    }
    if (!context_) {
        std::fprintf(stderr, "cannot create an OpenGL 3.2 context: %s\n", SDL_GetError());
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
    return true;
}

void Presenter::shutdown() {
    if (!context_) return;
    GLuint t = soft_tex_, b = vbo_, a = vao_;
    if (t) glDeleteTextures(1, &t);
    if (b) glDeleteBuffers(1, &b);
    if (a) glDeleteVertexArrays(1, &a);
    if (program_) glDeleteProgram(program_);
    SDL_GL_DestroyContext(static_cast<SDL_GLContext>(context_));
    context_ = nullptr;
}

void Presenter::layout(int w, int h) {
    width_ = w;
    height_ = h;
    const float s = std::min(float(w) / kScreenW, float(h) / (kScreenH * 2));
    const float sw = kScreenW * s, sh = kScreenH * s;
    const float x = (w - sw) / 2, y = (h - sh * 2) / 2;
    top_ = {x, y, sw, sh};
    bottom_ = {x, y + sh, sw, sh};
}

void Presenter::present(const ntr::Frame* frame, Filter filter, const char* screenshot) {
    // melonDS's GL renderers share this context: put back the state the screen pass relies on.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
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
        const Rect* screens[2] = {&top_, &bottom_};
        for (int layer = 0; layer < 2; layer++) {
            const Rect& r = *screens[layer];
            glUniform4f(u_dest_, r.x / width_ * 2 - 1, 1 - r.y / height_ * 2, (r.x + r.w) / width_ * 2 - 1,
                        1 - (r.y + r.h) / height_ * 2);
            glUniform1f(u_layer_, float(layer));
            glUniform2f(u_out_size_, r.w, r.h);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        glUseProgram(0);
    }
    if (screenshot) save_png(screenshot);
    SDL_GL_SwapWindow(window_);
}

}  // namespace video
