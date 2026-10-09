// OpenGL presentation of the DS screens: owns the window's GL context (which melonDS's GL renderers also draw with)
// and scales the two screens into the window.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

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
    Windows,     // the main screen fills the window; the other one fills a second window
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

class FlipPresenter;

class Presenter {
public:
    Presenter();
    ~Presenter();

    // Creates the GL context (4.3 core if available, else 3.3 core) on `window` and loads its functions. On Windows,
    // frames are shown through a DXGI flip-model swapchain when the driver allows it (unless `allow_flip_model` is
    // false), else with an ordinary GL swap.
    bool init(SDL_Window* window, bool allow_flip_model = true);
    // Destroys the context (before the window goes).
    void shutdown();

    bool has_compute() const { return gl43_; }
    bool flip_model() const { return flip_ != nullptr; }

    // Widescreen: the top screen is (256 + 2 * extra) pixels wide (0 = 4:3), like the renderer's output texture, in
    // which the bottom screen is in the middle. Takes effect with the next layout().
    void set_wide(int extra) { wide_extra_ = extra; }
    int wide_extra() const { return wide_extra_; }
    // Lays the screens out for the window's current size.
    void layout(int window_px_w, int window_px_h, const LayoutSettings& settings);

    // The Windows layout: the other screen is shown in a window of its own (opened and closed by the frontend, which
    // owns it), drawn and shown with every frame that present() shows. It has no menu or screenshots.
    bool open_second(SDL_Window* window, bool allow_flip_model = true);
    void close_second();
    bool has_second() const { return window2_ != nullptr; }
    void layout_second(int window_px_w, int window_px_h, const LayoutSettings& settings);
    // The screen under a position in the second window (see screen_at).
    int screen_at_second(float px, float py, float* local_x, float* local_y) const;
    // The visible screens in drawing order (later ones are on top).
    int view_count() const { return view_count_; }
    const ScreenView& view(int i) const { return views_[i]; }
    // The screen drawn at a window position (topmost), or -1. Sets `local` to the position on that screen in DS pixels.
    int screen_at(float px, float py, float* local_x, float* local_y) const;
    // The tallest screen on the window, in pixels (what the internal resolution should cover).
    float largest_screen_height() const;

    // Draws a frame (or black if `frame` is null) to the back buffer, and leaves it bound for drawing over.
    void draw(const ntr::Frame* frame, Filter filter);
    // Shows the back buffer. With `screenshot`, first saves it as a PNG.
    void swap(const char* screenshot = nullptr);
    // Display-rate presentation (vsync): swap() shows frames in step with the display's refresh, and
    // wait_for_refresh() returns when the display can take the next frame, so that the frame drawn after it is shown
    // at the following refresh. Off: frames show at once and the frontend paces them.
    void set_vsync(bool on);
    bool vsync() const { return vsync_; }
    // Without vsync, frames are shown at the moment they are presented, tearing allowed (variable refresh rate
    // displays). Needs a swapchain or driver that can; see tearing_supported().
    void set_tearing(bool on) { tearing_ = on; }
    bool tearing_supported() const;
    void wait_for_refresh();
    // The refresh rate of the display the window is on (Hz), or 0 if unknown.
    float refresh_rate() const;
    // Drawn over every frame that present() shows (the settings menu), before it is saved or swapped in.
    std::function<void()> overlay;
    void present(const ntr::Frame* frame, Filter filter, const char* screenshot = nullptr) {
        draw(frame, filter);
        if (overlay) overlay();
        swap(screenshot);
        if (window2_) show_second(frame, filter, screenshot);
    }

private:
    void save_png(const char* path, unsigned target, int width, int height);
    void draw_views(unsigned target, int width, int height, const ScreenView* views, int count, const ntr::Frame* frame,
                    Filter filter);
    void show_second(const ntr::Frame* frame, Filter filter, const char* screenshot);

    SDL_Window* window_ = nullptr;
    void* context_ = nullptr;  // SDL_GLContext
    std::unique_ptr<FlipPresenter> flip_;
    unsigned target_ = 0;      // the framebuffer this frame is drawn into (0 = the window's own)
    bool gl43_ = false;
    bool vsync_ = false;
    bool tearing_ = false;
    unsigned program_ = 0, vbo_ = 0, vao_ = 0, soft_tex_ = 0;
    int wide_extra_ = 0;
    int u_uv_x_ = -1;
    int u_dest_ = -1, u_layer_ = -1, u_sharp_ = -1, u_out_size_ = -1, u_tex_ = -1, u_opacity_ = -1;
    int width_ = 0, height_ = 0;
    ScreenView views_[2];
    int view_count_ = 0;
    SDL_Window* window2_ = nullptr;  // the second window (Windows layout)
    std::unique_ptr<FlipPresenter> flip2_;
    int width2_ = 0, height2_ = 0;
    ScreenView view2_;
};

}  // namespace video
