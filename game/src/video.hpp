// OpenGL presentation of the DS screens: owns the window's GL context (which melonDS's GL renderers also draw with)
// and scales the two screens into the window.
#pragma once

#include <cstdint>

#include "ntr/runtime.hpp"

struct SDL_Window;

namespace video {

// How the screen images are filtered when scaled to the window.
enum class Filter {
    Nearest,  // hard pixel edges, uneven at non-integer sizes
    Linear,   // smooth (soft at large magnification)
    Sharp,    // sharp bilinear: integer-scaled pixels with a one-pixel blend at the edges
};
const char* filter_name(Filter f);

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;  // in window pixels, origin at the top left
};

class Presenter {
public:
    // Creates the GL context (4.3 core if available, else 3.2 core) on `window` and loads its functions.
    bool init(SDL_Window* window);
    // Destroys the context (before the window goes).
    void shutdown();
    ~Presenter() { shutdown(); }

    bool has_compute() const { return gl43_; }

    // Lays the screens out for the window's current size (top above bottom).
    void layout(int window_px_w, int window_px_h);
    const Rect& top_rect() const { return top_; }
    const Rect& bottom_rect() const { return bottom_; }

    // Draws a frame (or black if `frame` is null) and swaps. With `screenshot`, also saves the window as a PNG.
    void present(const ntr::Frame* frame, Filter filter, const char* screenshot = nullptr);

private:
    void save_png(const char* path);

    SDL_Window* window_ = nullptr;
    void* context_ = nullptr;  // SDL_GLContext
    bool gl43_ = false;
    unsigned program_ = 0, vbo_ = 0, vao_ = 0, soft_tex_ = 0;
    int u_dest_ = -1, u_layer_ = -1, u_sharp_ = -1, u_out_size_ = -1, u_tex_ = -1;
    int width_ = 0, height_ = 0;
    Rect top_, bottom_;
};

}  // namespace video
