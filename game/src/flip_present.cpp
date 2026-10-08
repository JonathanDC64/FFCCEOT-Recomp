#include "flip_present.hpp"

#include "OpenGLSupport.h"  // melonDS: glad (before windows.h's GL bits)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace video {

namespace {

// WGL_NV_DX_interop(2): lets GL draw into Direct3D resources.
using PFNOPENDEVICE = HANDLE(WINAPI*)(void* dx_device);
using PFNCLOSEDEVICE = BOOL(WINAPI*)(HANDLE device);
using PFNREGISTEROBJECT = HANDLE(WINAPI*)(HANDLE device, void* dx_object, GLuint name, GLenum type, GLenum access);
using PFNUNREGISTEROBJECT = BOOL(WINAPI*)(HANDLE device, HANDLE object);
using PFNLOCKOBJECTS = BOOL(WINAPI*)(HANDLE device, GLint count, HANDLE* objects);
constexpr GLenum kAccessWriteDiscard = 0x0002;  // WGL_ACCESS_WRITE_DISCARD_NV

}  // namespace

struct FlipPresenter::Impl {
    PFNOPENDEVICE open_device = nullptr;
    PFNCLOSEDEVICE close_device = nullptr;
    PFNREGISTEROBJECT register_object = nullptr;
    PFNUNREGISTEROBJECT unregister_object = nullptr;
    PFNLOCKOBJECTS lock_objects = nullptr, unlock_objects = nullptr;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapchain;
    UINT swapchain_flags = 0;
    HANDLE waitable = nullptr;  // signalled when the swapchain can take another frame (frame latency 1)
    HANDLE interop = nullptr;   // the D3D device opened for GL

    // Per size: the GL framebuffer the frame is drawn into, and a D3D texture GL copies it to (rows flipped, since GL
    // rows run bottom-up and D3D rows top-down), which D3D then copies to the back buffer.
    int width = 0, height = 0;
    GLuint draw_fbo = 0, draw_tex = 0;
    GLuint shared_fbo = 0, shared_rb = 0;
    ComPtr<ID3D11Texture2D> shared_tex;
    HANDLE shared_object = nullptr;

    void release_size() {
        if (shared_object) unregister_object(interop, shared_object);
        shared_object = nullptr;
        shared_tex.Reset();
        if (draw_fbo) glDeleteFramebuffers(1, &draw_fbo);
        if (draw_tex) glDeleteTextures(1, &draw_tex);
        if (shared_fbo) glDeleteFramebuffers(1, &shared_fbo);
        if (shared_rb) glDeleteRenderbuffers(1, &shared_rb);
        draw_fbo = draw_tex = shared_fbo = shared_rb = 0;
        width = height = 0;
    }

    bool create_size(int w, int h) {
        release_size();
        if (FAILED(swapchain->ResizeBuffers(0, UINT(w), UINT(h), DXGI_FORMAT_UNKNOWN, swapchain_flags))) return false;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(w);
        td.Height = UINT(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &shared_tex))) return false;
        glGenRenderbuffers(1, &shared_rb);
        shared_object = register_object(interop, shared_tex.Get(), shared_rb, GL_RENDERBUFFER, kAccessWriteDiscard);
        if (!shared_object) return false;
        glGenFramebuffers(1, &shared_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, shared_fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, shared_rb);

        glGenTextures(1, &draw_tex);
        glBindTexture(GL_TEXTURE_2D, draw_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &draw_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, draw_tex, 0);
        const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (!complete) return false;
        width = w;
        height = h;
        return true;
    }
};

FlipPresenter::FlipPresenter() : impl_(std::make_unique<Impl>()) {}
FlipPresenter::~FlipPresenter() { shutdown(); }

bool FlipPresenter::init(void* hwnd, std::string& error) {
    Impl& m = *impl_;
    auto load = [](const char* name) { return reinterpret_cast<void*>(wglGetProcAddress(name)); };
    m.open_device = reinterpret_cast<PFNOPENDEVICE>(load("wglDXOpenDeviceNV"));
    m.close_device = reinterpret_cast<PFNCLOSEDEVICE>(load("wglDXCloseDeviceNV"));
    m.register_object = reinterpret_cast<PFNREGISTEROBJECT>(load("wglDXRegisterObjectNV"));
    m.unregister_object = reinterpret_cast<PFNUNREGISTEROBJECT>(load("wglDXUnregisterObjectNV"));
    m.lock_objects = reinterpret_cast<PFNLOCKOBJECTS>(load("wglDXLockObjectsNV"));
    m.unlock_objects = reinterpret_cast<PFNLOCKOBJECTS>(load("wglDXUnlockObjectsNV"));
    if (!m.open_device || !m.close_device || !m.register_object || !m.unregister_object || !m.lock_objects ||
        !m.unlock_objects) {
        error = "the OpenGL driver has no WGL_NV_DX_interop";
        return false;
    }

    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr,
                                 0, D3D11_SDK_VERSION, &m.device, nullptr, &m.context))) {
        error = "cannot create a Direct3D 11 device";
        return false;
    }
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(m.device.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        error = "cannot reach the DXGI factory";
        return false;
    }

    RECT rc{};
    GetClientRect(static_cast<HWND>(hwnd), &rc);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = UINT(std::max<LONG>(1, rc.right - rc.left));
    sd.Height = UINT(std::max<LONG>(1, rc.bottom - rc.top));
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_NONE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;  // Windows 10+
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    // The waitable object paces frames to the display's refresh when vsync is wanted (Windows 8.1+).
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    HRESULT hr = factory->CreateSwapChainForHwnd(m.device.Get(), static_cast<HWND>(hwnd), &sd, nullptr, nullptr,
                                                 &m.swapchain);
    if (FAILED(hr)) {
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;  // Windows 8
        sd.Flags = 0;
        hr = factory->CreateSwapChainForHwnd(m.device.Get(), static_cast<HWND>(hwnd), &sd, nullptr, nullptr,
                                             &m.swapchain);
    }
    if (FAILED(hr)) {
        error = "cannot create a flip-model swapchain";
        return false;
    }
    m.swapchain_flags = sd.Flags;
    ComPtr<IDXGISwapChain2> swapchain2;
    if (sd.Flags && SUCCEEDED(m.swapchain.As(&swapchain2))) {
        swapchain2->SetMaximumFrameLatency(1);
        m.waitable = swapchain2->GetFrameLatencyWaitableObject();
    }
    factory->MakeWindowAssociation(static_cast<HWND>(hwnd), DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

    m.interop = m.open_device(m.device.Get());
    if (!m.interop) {
        error = "OpenGL cannot share with the Direct3D device (different GPUs?)";
        m.swapchain.Reset();
        return false;
    }
    if (!m.create_size(int(sd.Width), int(sd.Height))) {
        error = "cannot share a texture between OpenGL and Direct3D";
        shutdown();
        return false;
    }
    return true;
}

void FlipPresenter::shutdown() {
    Impl& m = *impl_;
    if (!m.interop) return;
    m.release_size();
    m.close_device(m.interop);
    m.interop = nullptr;
    if (m.waitable) CloseHandle(m.waitable);
    m.waitable = nullptr;
    m.swapchain.Reset();
    m.context.Reset();
    m.device.Reset();
}

unsigned FlipPresenter::begin(int width, int height) {
    Impl& m = *impl_;
    width = std::max(width, 1);
    height = std::max(height, 1);
    if ((width != m.width || height != m.height) && !m.create_size(width, height))
        std::fprintf(stderr, "flip model: cannot resize to %dx%d\n", width, height);
    return m.draw_fbo;
}

void FlipPresenter::wait() {
    Impl& m = *impl_;
    if (m.waitable) WaitForSingleObjectEx(m.waitable, 100, TRUE);
}

void FlipPresenter::present(bool vsync) {
    Impl& m = *impl_;
    if (!m.shared_object) return;
    // GL -> shared texture, flipping the rows.
    m.lock_objects(m.interop, 1, &m.shared_object);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m.draw_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m.shared_fbo);
    glBlitFramebuffer(0, 0, m.width, m.height, 0, m.height, m.width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    m.unlock_objects(m.interop, 1, &m.shared_object);
    // Shared texture -> back buffer, and show it. Sync interval 0: the frontend paces frames itself; the flip model
    // shows the latest frame at the next refresh without tearing. Sync interval 1: in step with the refresh.
    ComPtr<ID3D11Texture2D> back;
    if (SUCCEEDED(m.swapchain->GetBuffer(0, IID_PPV_ARGS(&back)))) m.context->CopyResource(back.Get(), m.shared_tex.Get());
    m.swapchain->Present(vsync ? 1 : 0, 0);
}

}  // namespace video
