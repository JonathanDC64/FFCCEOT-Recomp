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
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

// Where the two screens go on the window.
enum class Layout {
    Single,      // one screen fills the window; the other is an optional picture-in-picture
    MainSide,    // the main screen at the left edge; the other fills the width that is left, on the right
    Stacked,     // one above the other, as on the console
    SideBySide,  // next to each other, the same size
};
const char* layout_name(Layout l);

// Where the smaller screen sits vertically in the MainSide layout.
enum class Align { Top, Middle, Bottom };
const char* align_name(Align a);

struct LayoutSettings {
    Layout layout = Layout::Single;
    bool swap = false;           // Single/MainSide: the bottom screen is the main one. Others: the bottom screen first.
    bool second_screen = true;   // Single: show the other screen as a picture-in-picture
    float pip_size = 0.3f;       // picture-in-picture height, as a fraction of the window height
    float pip_opacity = 1.0f;
    Align side_align = Align::Bottom;  // MainSide: the side screen's vertical position
};

// One DS screen's place on the window.
struct ScreenView {
    int screen = 0;  // 0 = top, 1 = bottom
    Rect rect;
    float opacity = 1;
    bool framed = false;  // drawn with a border (the picture-in-picture)
};

class Presenter {
public:
    // Creates the GL context (4.3 core if available, else 3.3 core) on `window` and loads its functions.
    bool init(SDL_Window* window);
    // Destroys the context (before the window goes).
    void shutdown();
    ~Presenter() { shutdown(); }

    bool has_compute() const { return gl43_; }

    // Lays the screens out for the window's current size.
    void layout(int window_px_w, int window_px_h, const LayoutSettings& settings);
    // The visible screens in drawing order (later ones are on top).
    int view_count() const { return view_count_; }
    const ScreenView& view(int i) const { return views_[i]; }
    // The screen drawn at a window position (topmost), or -1. Sets `local` to the position on that screen in DS pixels.
    int screen_at(float px, float py, float* local_x, float* local_y) const;
    // The tallest screen on the window, in pixels (what the internal resolution should cover).
    float largest_screen_height() const;

    // Draws a frame (or black if `frame` is null) to the back buffer.
    void draw(const ntr::Frame* frame, Filter filter);
    // Shows the back buffer. With `screenshot`, first saves it as a PNG.
    void swap(const char* screenshot = nullptr);
    void present(const ntr::Frame* frame, Filter filter, const char* screenshot = nullptr) {
        draw(frame, filter);
        swap(screenshot);
    }

private:
    void save_png(const char* path);

    SDL_Window* window_ = nullptr;
    void* context_ = nullptr;  // SDL_GLContext
    bool gl43_ = false;
    unsigned program_ = 0, vbo_ = 0, vao_ = 0, soft_tex_ = 0;
    int u_dest_ = -1, u_layer_ = -1, u_sharp_ = -1, u_out_size_ = -1, u_tex_ = -1, u_opacity_ = -1;
    int width_ = 0, height_ = 0;
    ScreenView views_[2];
    int view_count_ = 0;
};

}  // namespace video
