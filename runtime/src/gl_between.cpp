#include "gl_between.hpp"

#include <memory>

#include "GPU.h"
#include "GPU2D_OpenGL.h"
#include "GPU_OpenGL.h"

namespace ntr {

namespace {

// Access to the GL renderer's private parts, through explicit template instantiation (which may name private
// members): the 3D output texture, the final pass and its framebuffers, and engine A's 2D compositor. melonDS's own
// sources are left as they are.
template <typename Tag, typename Tag::type M>
struct Grant {
    friend typename Tag::type get(Tag) { return M; }
};

struct Output3D {
    using type = GLuint melonDS::GLRenderer::*;
    friend type get(Output3D);
};
struct OutputFBs {
    using type = GLuint (melonDS::GLRenderer::*)[2];
    friend type get(OutputFBs);
};
struct BackBuffer {
    using type = int melonDS::Renderer::*;
    friend type get(BackBuffer);
};
struct Engine2DA {
    using type = std::unique_ptr<melonDS::Renderer2D> melonDS::Renderer::*;
    friend type get(Engine2DA);
};
struct FinalPass {
    using type = void (melonDS::GLRenderer::*)(int, int);
    friend type get(FinalPass);
};
struct Composite2D {
    using type = void (melonDS::GLRenderer2D::*)(int, int);
    friend type get(Composite2D);
};

template struct Grant<Output3D, &melonDS::GLRenderer::OutputTex3D>;
template struct Grant<OutputFBs, &melonDS::GLRenderer::FPOutputFB>;
template struct Grant<BackBuffer, &melonDS::Renderer::BackBuffer>;
template struct Grant<Engine2DA, &melonDS::Renderer::Rend2D_A>;
template struct Grant<FinalPass, &melonDS::GLRenderer::RenderScreen>;
template struct Grant<Composite2D, &melonDS::GLRenderer2D::RenderScreen>;

void copy_texture(GLuint read_fb, GLuint draw_fb, GLuint from, GLuint to, int w, int h) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fb);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, from, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fb);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, to, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

}  // namespace

GlBetween::~GlBetween() { release(); }

void GlBetween::release() {
    GLuint textures[2] = {saved_3d_, out_tex_};
    GLuint fbs[3] = {out_fb_, read_fb_, draw_fb_};
    if (saved_3d_ || out_tex_) glDeleteTextures(2, textures);
    if (out_fb_ || read_fb_ || draw_fb_) glDeleteFramebuffers(3, fbs);
    saved_3d_ = out_tex_ = out_fb_ = read_fb_ = draw_fb_ = 0;
    w_ = h_ = 0;
}

void GlBetween::ensure_size(int w, int h) {
    if (w == w_ && h == h_) return;
    release();
    w_ = w;
    h_ = h;
    GLuint t[2], f[3];
    glGenTextures(2, t);
    glGenFramebuffers(3, f);
    saved_3d_ = t[0];
    out_tex_ = t[1];
    out_fb_ = f[0];
    read_fb_ = f[1];
    draw_fb_ = f[2];
    glBindTexture(GL_TEXTURE_2D, saved_3d_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    // Laid out like the renderer's final output: one layer per screen, written as two colour attachments.
    glBindTexture(GL_TEXTURE_2D_ARRAY, out_tex_);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, w, h, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, out_fb_);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, out_tex_, 0, 0);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, out_tex_, 0, 1);
    const GLenum attachments[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, attachments);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

uint32_t GlBetween::render(melonDS::GLRenderer& r) {
    const GLuint tex3d = r.*get(Output3D{});
    GLint w = 0, h = 0;
    glBindTexture(GL_TEXTURE_2D, tex3d);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    if (w <= 0 || h <= 0) return 0;
    ensure_size(w, h);

    copy_texture(read_fb_, draw_fb_, tex3d, saved_3d_, w, h);
    r.Start3DRendering();  // the 3D of the current render list, into the renderer's 3D output
    auto& engine_a = static_cast<melonDS::GLRenderer2D&>(*(r.*get(Engine2DA{})));
    (engine_a.*get(Composite2D{}))(0, 192);  // engine A's screen again, with that 3D
    // The final pass (screen order, master brightness) into our output instead of the renderer's back buffer.
    GLuint(&fbs)[2] = r.*get(OutputFBs{});
    const int back = r.*get(BackBuffer{});
    const GLuint own = fbs[back];
    fbs[back] = out_fb_;
    (r.*get(FinalPass{}))(0, 192);
    fbs[back] = own;
    copy_texture(read_fb_, draw_fb_, saved_3d_, tex3d, w, h);
    return out_tex_;
}

}  // namespace ntr
