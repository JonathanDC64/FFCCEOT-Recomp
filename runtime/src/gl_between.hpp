// Frames between emulated frames, for 3D at the display's refresh rate (docs/ARCHITECTURE.md §5.3): draws the 3D of
// the current render list again with melonDS's OpenGL or compute renderer, and composites both screens again into an
// output of its own, the way the last emulated frame did. The renderer's own 3D output, which the next emulated frame
// composites, is put back afterwards.
#pragma once

#include <cstdint>

namespace melonDS {
class GLRenderer;
}

namespace ntr {

class GlBetween {
public:
    GlBetween() = default;
    GlBetween(const GlBetween&) = delete;
    GlBetween& operator=(const GlBetween&) = delete;
    ~GlBetween();  // needs the GL context current

    // Draws a frame from the current render list. Returns the output texture (a 2-layer array: the top and bottom
    // screens, like GLRenderer::GetFramebuffers), valid until the next call.
    uint32_t render(melonDS::GLRenderer& renderer);

private:
    void ensure_size(int w, int h);
    void release();

    int w_ = 0, h_ = 0;
    uint32_t saved_3d_ = 0;   // the renderer's own 3D output, kept while the extra frame is drawn
    uint32_t out_tex_ = 0;    // the composited screens
    uint32_t out_fb_ = 0;
    uint32_t read_fb_ = 0, draw_fb_ = 0;  // for copying 3D outputs
};

}  // namespace ntr
