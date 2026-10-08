// Windows: shows OpenGL rendering through a DXGI flip-model swapchain (Direct3D 11), using WGL_NV_DX_interop2.
//
// OpenGL on Windows presents with the old blit model, which the GPU driver and Windows can only take direct to the
// display ("fullscreen optimisations") by treating a window that covers the monitor as exclusive fullscreen: alt-tab
// then blanks the screen. A flip-model swapchain gets the same direct path (independent flip) for a borderless window
// and leaves it seamlessly. See docs/ARCHITECTURE.md section 5.5.
#pragma once

#include <memory>
#include <string>

namespace video {

class FlipPresenter {
public:
    FlipPresenter();
    ~FlipPresenter();

    // Sets up the swapchain on the window (an HWND). The GL context must be current. On failure `error` says why.
    bool init(void* hwnd, std::string& error);
    void shutdown();

    // The GL framebuffer to draw this frame into, sized to the window (recreated when the size changes).
    unsigned begin(int width, int height);
    // Shows what was drawn into the framebuffer: at the next refresh with `vsync`, else at once (the latest frame
    // drawn before a refresh is the one shown).
    void present(bool vsync = false);
    // With vsync: waits until the swapchain can take the next frame (a refresh), so the frame drawn next is the one
    // shown at the refresh after it.
    void wait();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace video
