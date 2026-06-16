#if defined(__ANDROID__)

#include "host_gl_backend.h"

#include <EGL/egl.h>
#include <android/log.h>
#include <android/native_window.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <dlfcn.h>
#include <mutex>
#include <stdexcept>

namespace {

constexpr const char* kLogTag = "atrasis";
constexpr const char* kLogPrefix = "[atrasis] ";

std::mutex g_window_mutex;
ANativeWindow* g_window = nullptr;

using GlBindFramebufferFn = void (*)(unsigned int target, unsigned int framebuffer);
using GlClearFn = void (*)(unsigned int mask);
using GlClearColorFn = void (*)(float red, float green, float blue, float alpha);
using GlDisableFn = void (*)(unsigned int cap);
using GlEnableFn = void (*)(unsigned int cap);
using GlGetFloatvFn = void (*)(unsigned int pname, float* data);
using GlGetIntegervFn = void (*)(unsigned int pname, int* data);
using GlIsEnabledFn = unsigned char (*)(unsigned int cap);
using GlScissorFn = void (*)(int x, int y, int width, int height);
using GlViewportFn = void (*)(int x, int y, int width, int height);

GlBindFramebufferFn g_real_gl_bind_framebuffer = nullptr;
GlClearFn g_real_gl_clear = nullptr;
GlClearColorFn g_real_gl_clear_color = nullptr;
GlDisableFn g_real_gl_disable = nullptr;
GlEnableFn g_real_gl_enable = nullptr;
GlGetFloatvFn g_real_gl_get_floatv = nullptr;
GlGetIntegervFn g_real_gl_get_integerv = nullptr;
GlIsEnabledFn g_real_gl_is_enabled = nullptr;
GlScissorFn g_real_gl_scissor = nullptr;
GlViewportFn g_real_gl_viewport = nullptr;

std::mutex g_gl_mapping_mutex;
unsigned int g_bound_draw_framebuffer = 0;
int g_content_width = 960;
int g_content_height = 640;
int g_surface_width = 960;
int g_surface_height = 640;
int g_letterbox_x = 0;
int g_letterbox_y = 0;
int g_letterbox_width = 960;
int g_letterbox_height = 640;

ANativeWindow* AcquireAndroidWindow() {
    std::lock_guard lock(g_window_mutex);
    if (g_window != nullptr) {
        ANativeWindow_acquire(g_window);
    }
    return g_window;
}

int LogicalPixelsFromRequest(const int value) {
    // UIKit reports points (480x320), while EAGL renders pixels (960x640).
    return value <= 640 ? std::max(1, value * 2) : std::max(1, value);
}

void UpdateLetterboxLocked() {
    const float content_aspect = static_cast<float>(g_content_width) / static_cast<float>(g_content_height);
    const float surface_aspect = static_cast<float>(g_surface_width) / static_cast<float>(g_surface_height);

    if (surface_aspect > content_aspect) {
        g_letterbox_height = g_surface_height;
        g_letterbox_width = std::max(1, static_cast<int>(std::lround(static_cast<float>(g_surface_height) * content_aspect)));
    } else {
        g_letterbox_width = g_surface_width;
        g_letterbox_height = std::max(1, static_cast<int>(std::lround(static_cast<float>(g_surface_width) / content_aspect)));
    }
    g_letterbox_x = (g_surface_width - g_letterbox_width) / 2;
    g_letterbox_y = (g_surface_height - g_letterbox_height) / 2;
}

void SetAndroidGlMapping(const int content_width, const int content_height, const int surface_width, const int surface_height) {
    std::lock_guard lock(g_gl_mapping_mutex);
    g_content_width = std::max(1, content_width);
    g_content_height = std::max(1, content_height);
    g_surface_width = std::max(1, surface_width);
    g_surface_height = std::max(1, surface_height);
    UpdateLetterboxLocked();
}

void TransformDefaultFramebufferRect(int x, int y, int width, int height, int& out_x, int& out_y, int& out_width, int& out_height) {
    std::lock_guard lock(g_gl_mapping_mutex);
    const float scale_x = static_cast<float>(g_letterbox_width) / static_cast<float>(g_content_width);
    const float scale_y = static_cast<float>(g_letterbox_height) / static_cast<float>(g_content_height);

    out_x = g_letterbox_x + static_cast<int>(std::lround(static_cast<float>(x) * scale_x));
    out_y = g_letterbox_y + static_cast<int>(std::lround(static_cast<float>(y) * scale_y));
    out_width = std::max(1, static_cast<int>(std::lround(static_cast<float>(width) * scale_x)));
    out_height = std::max(1, static_cast<int>(std::lround(static_cast<float>(height) * scale_y)));
}

void AndroidGlBindFramebuffer(const unsigned int target, const unsigned int framebuffer) {
    constexpr unsigned int kGlFramebuffer = 0x8D40;
    constexpr unsigned int kGlDrawFramebuffer = 0x8CA9;
    if (target == kGlFramebuffer || target == kGlDrawFramebuffer) {
        std::lock_guard lock(g_gl_mapping_mutex);
        g_bound_draw_framebuffer = framebuffer;
    }
    if (g_real_gl_bind_framebuffer != nullptr) {
        g_real_gl_bind_framebuffer(target, framebuffer);
    }
}

void AndroidGlScissor(const int x, const int y, const int width, const int height) {
    bool default_framebuffer = false;
    {
        std::lock_guard lock(g_gl_mapping_mutex);
        default_framebuffer = g_bound_draw_framebuffer == 0;
    }
    if (default_framebuffer) {
        int mapped_x = 0;
        int mapped_y = 0;
        int mapped_width = 0;
        int mapped_height = 0;
        TransformDefaultFramebufferRect(x, y, width, height, mapped_x, mapped_y, mapped_width, mapped_height);
        if (g_real_gl_scissor != nullptr) {
            g_real_gl_scissor(mapped_x, mapped_y, mapped_width, mapped_height);
        }
        return;
    }
    if (g_real_gl_scissor != nullptr) {
        g_real_gl_scissor(x, y, width, height);
    }
}

void AndroidGlViewport(const int x, const int y, const int width, const int height) {
    bool default_framebuffer = false;
    {
        std::lock_guard lock(g_gl_mapping_mutex);
        default_framebuffer = g_bound_draw_framebuffer == 0;
    }
    if (default_framebuffer) {
        int mapped_x = 0;
        int mapped_y = 0;
        int mapped_width = 0;
        int mapped_height = 0;
        TransformDefaultFramebufferRect(x, y, width, height, mapped_x, mapped_y, mapped_width, mapped_height);
        if (g_real_gl_viewport != nullptr) {
            g_real_gl_viewport(mapped_x, mapped_y, mapped_width, mapped_height);
        }
        return;
    }
    if (g_real_gl_viewport != nullptr) {
        g_real_gl_viewport(x, y, width, height);
    }
}

class AndroidHostGLBackend final : public HostGLBackend {
public:
    AndroidHostGLBackend() {
        gles_library_ = dlopen("libGLESv2.so", RTLD_LAZY | RTLD_LOCAL);
        egl_library_ = dlopen("libEGL.so", RTLD_LAZY | RTLD_LOCAL);
    }

    ~AndroidHostGLBackend() override {
        DestroySurface();
        if (window_ != nullptr) {
            ANativeWindow_release(window_);
            window_ = nullptr;
        }
        if (context_ != EGL_NO_CONTEXT) {
            eglDestroyContext(display_, context_);
            context_ = EGL_NO_CONTEXT;
        }
        if (display_ != EGL_NO_DISPLAY) {
            eglTerminate(display_);
            display_ = EGL_NO_DISPLAY;
        }
        if (gles_library_ != nullptr) {
            dlclose(gles_library_);
        }
        if (egl_library_ != nullptr) {
            dlclose(egl_library_);
        }
    }

    bool IsSupported() const override {
        return true;
    }

    void EnsureWindow(const int width, const int height, const std::string& title) override {
        EnsureDisplay();
        EnsureContext();
        RefreshWindow();
        Resize(width, height);
        MakeCurrent();
        if (!reported_context_) {
            reported_context_ = true;
            __android_log_print(ANDROID_LOG_INFO,
                kLogTag,
                "%s[host-gl/android] content=%dx%d surface=%dx%d letterbox=%d,%d,%dx%d title=%s display=%p context=%p egl_surface=%p",
                kLogPrefix,
                content_width_,
                content_height_,
                width_,
                height_,
                letterbox_x_,
                letterbox_y_,
                letterbox_width_,
                letterbox_height_,
                title.c_str(),
                static_cast<void*>(display_),
                static_cast<void*>(context_),
                static_cast<void*>(surface_));
        }
    }

    void Resize(const int width, const int height) override {
        content_width_ = LogicalPixelsFromRequest(width);
        content_height_ = LogicalPixelsFromRequest(height);

        int surface_width = content_width_;
        int surface_height = content_height_;
        if (window_ != nullptr) {
            ANativeWindow_setBuffersGeometry(window_, 0, 0, WINDOW_FORMAT_RGBA_8888);
            surface_width = std::max(1, ANativeWindow_getWidth(window_));
            surface_height = std::max(1, ANativeWindow_getHeight(window_));
        }

        if (surface_ != EGL_NO_SURFACE && surface_width == width_ && surface_height == height_) {
            UpdateLetterbox();
            return;
        }

        DestroySurface();
        if (window_ != nullptr) {
            surface_ = eglCreateWindowSurface(display_, config_, window_, nullptr);
        } else {
            const EGLint pbuffer_attributes[] = {
                EGL_WIDTH, surface_width,
                EGL_HEIGHT, surface_height,
                EGL_NONE,
            };
            surface_ = eglCreatePbufferSurface(display_, config_, pbuffer_attributes);
        }
        if (surface_ == EGL_NO_SURFACE) {
            throw std::runtime_error(window_ != nullptr ? "eglCreateWindowSurface failed" : "eglCreatePbufferSurface failed");
        }
        width_ = surface_width;
        height_ = surface_height;
        UpdateLetterbox();
    }

    void MakeCurrent() override {
        if (display_ == EGL_NO_DISPLAY || context_ == EGL_NO_CONTEXT || surface_ == EGL_NO_SURFACE) {
            return;
        }
        if (eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE) {
            throw std::runtime_error("eglMakeCurrent failed");
        }
    }

    void Present() override {
        if (display_ != EGL_NO_DISPLAY && surface_ != EGL_NO_SURFACE) {
            ClearLetterbox();
            eglSwapBuffers(display_, surface_);
        }
    }

    void PumpEvents() override {}

    void* GetProcAddress(const char* name) override {
        if (name == nullptr) {
            return nullptr;
        }
        if (std::strcmp(name, "glBindFramebuffer") == 0 || std::strcmp(name, "glBindFramebufferOES") == 0) {
            EnsureRealProc(g_real_gl_bind_framebuffer, "glBindFramebuffer");
            return reinterpret_cast<void*>(&AndroidGlBindFramebuffer);
        }
        if (std::strcmp(name, "glScissor") == 0) {
            EnsureRealProc(g_real_gl_scissor, "glScissor");
            return reinterpret_cast<void*>(&AndroidGlScissor);
        }
        if (std::strcmp(name, "glViewport") == 0) {
            EnsureRealProc(g_real_gl_viewport, "glViewport");
            return reinterpret_cast<void*>(&AndroidGlViewport);
        }
        if (void* proc = reinterpret_cast<void*>(eglGetProcAddress(name)); proc != nullptr) {
            return proc;
        }
        if (gles_library_ != nullptr) {
            if (void* proc = dlsym(gles_library_, name); proc != nullptr) {
                return proc;
            }
        }
        if (egl_library_ != nullptr) {
            if (void* proc = dlsym(egl_library_, name); proc != nullptr) {
                return proc;
            }
        }
        return dlsym(RTLD_DEFAULT, name);
    }

private:
    void RefreshWindow() {
        ANativeWindow* latest = AcquireAndroidWindow();
        if (latest == window_) {
            if (latest != nullptr) {
                ANativeWindow_release(latest);
            }
            return;
        }

        DestroySurface();
        if (window_ != nullptr) {
            ANativeWindow_release(window_);
        }
        window_ = latest;
        width_ = 0;
        height_ = 0;
    }

    void EnsureDisplay() {
        if (display_ != EGL_NO_DISPLAY) {
            return;
        }
        display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display_ == EGL_NO_DISPLAY) {
            throw std::runtime_error("eglGetDisplay failed");
        }
        if (eglInitialize(display_, nullptr, nullptr) != EGL_TRUE) {
            throw std::runtime_error("eglInitialize failed");
        }
        if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
            throw std::runtime_error("eglBindAPI(EGL_OPENGL_ES_API) failed");
        }

        const EGLint config_attributes[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24,
            EGL_STENCIL_SIZE, 8,
            EGL_NONE,
        };
        EGLint config_count = 0;
        if (eglChooseConfig(display_, config_attributes, &config_, 1, &config_count) != EGL_TRUE || config_count == 0) {
            throw std::runtime_error("eglChooseConfig failed");
        }
    }

    void EnsureContext() {
        if (context_ != EGL_NO_CONTEXT) {
            return;
        }
        const EGLint context_attributes[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE,
        };
        context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, context_attributes);
        if (context_ == EGL_NO_CONTEXT) {
            throw std::runtime_error("eglCreateContext failed");
        }
    }

    template <typename Proc>
    void EnsureRealProc(Proc& proc, const char* name) {
        if (proc != nullptr) {
            return;
        }
        proc = reinterpret_cast<Proc>(eglGetProcAddress(name));
        if (proc == nullptr && gles_library_ != nullptr) {
            proc = reinterpret_cast<Proc>(dlsym(gles_library_, name));
        }
        if (proc == nullptr) {
            proc = reinterpret_cast<Proc>(dlsym(RTLD_DEFAULT, name));
        }
    }

    void ClearLetterbox() {
        if (width_ <= 0 || height_ <= 0
            || (letterbox_x_ == 0 && letterbox_y_ == 0 && letterbox_width_ == width_ && letterbox_height_ == height_)) {
            return;
        }

        EnsureRealProc(g_real_gl_bind_framebuffer, "glBindFramebuffer");
        EnsureRealProc(g_real_gl_clear, "glClear");
        EnsureRealProc(g_real_gl_clear_color, "glClearColor");
        EnsureRealProc(g_real_gl_disable, "glDisable");
        EnsureRealProc(g_real_gl_enable, "glEnable");
        EnsureRealProc(g_real_gl_get_floatv, "glGetFloatv");
        EnsureRealProc(g_real_gl_get_integerv, "glGetIntegerv");
        EnsureRealProc(g_real_gl_is_enabled, "glIsEnabled");
        EnsureRealProc(g_real_gl_scissor, "glScissor");
        EnsureRealProc(g_real_gl_viewport, "glViewport");
        if (g_real_gl_bind_framebuffer == nullptr
            || g_real_gl_clear == nullptr
            || g_real_gl_clear_color == nullptr
            || g_real_gl_disable == nullptr
            || g_real_gl_enable == nullptr
            || g_real_gl_get_floatv == nullptr
            || g_real_gl_get_integerv == nullptr
            || g_real_gl_is_enabled == nullptr
            || g_real_gl_scissor == nullptr
            || g_real_gl_viewport == nullptr) {
            return;
        }

        constexpr unsigned int kGlColorBufferBit = 0x00004000;
        constexpr unsigned int kGlColorClearValue = 0x0C22;
        constexpr unsigned int kGlFramebuffer = 0x8D40;
        constexpr unsigned int kGlFramebufferBinding = 0x8CA6;
        constexpr unsigned int kGlScissorBox = 0x0C10;
        constexpr unsigned int kGlScissorTest = 0x0C11;
        constexpr unsigned int kGlViewport = 0x0BA2;

        int previous_framebuffer = 0;
        int previous_viewport[4] = {0, 0, width_, height_};
        int previous_scissor[4] = {0, 0, width_, height_};
        float previous_clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        g_real_gl_get_integerv(kGlFramebufferBinding, &previous_framebuffer);
        g_real_gl_get_integerv(kGlViewport, previous_viewport);
        g_real_gl_get_integerv(kGlScissorBox, previous_scissor);
        g_real_gl_get_floatv(kGlColorClearValue, previous_clear_color);
        const bool previous_scissor_enabled = g_real_gl_is_enabled(kGlScissorTest) != 0;

        g_real_gl_bind_framebuffer(kGlFramebuffer, 0);
        g_real_gl_viewport(0, 0, width_, height_);
        g_real_gl_enable(kGlScissorTest);
        g_real_gl_clear_color(0.0f, 0.0f, 0.0f, 1.0f);

        const auto clear_rect = [&](const int x, const int y, const int w, const int h) {
            if (w <= 0 || h <= 0) {
                return;
            }
            g_real_gl_scissor(x, y, w, h);
            g_real_gl_clear(kGlColorBufferBit);
        };

        clear_rect(0, 0, letterbox_x_, height_);
        clear_rect(letterbox_x_ + letterbox_width_, 0, width_ - (letterbox_x_ + letterbox_width_), height_);
        clear_rect(0, 0, width_, letterbox_y_);
        clear_rect(0, letterbox_y_ + letterbox_height_, width_, height_ - (letterbox_y_ + letterbox_height_));

        g_real_gl_clear_color(previous_clear_color[0], previous_clear_color[1], previous_clear_color[2], previous_clear_color[3]);
        g_real_gl_scissor(previous_scissor[0], previous_scissor[1], previous_scissor[2], previous_scissor[3]);
        g_real_gl_viewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);
        if (previous_scissor_enabled) {
            g_real_gl_enable(kGlScissorTest);
        } else {
            g_real_gl_disable(kGlScissorTest);
        }
        g_real_gl_bind_framebuffer(kGlFramebuffer, static_cast<unsigned int>(previous_framebuffer));
    }

    void UpdateLetterbox() {
        const float content_aspect = static_cast<float>(content_width_) / static_cast<float>(content_height_);
        const float surface_aspect = static_cast<float>(width_) / static_cast<float>(height_);
        if (surface_aspect > content_aspect) {
            letterbox_height_ = height_;
            letterbox_width_ = std::max(1, static_cast<int>(std::lround(static_cast<float>(height_) * content_aspect)));
        } else {
            letterbox_width_ = width_;
            letterbox_height_ = std::max(1, static_cast<int>(std::lround(static_cast<float>(width_) / content_aspect)));
        }
        letterbox_x_ = (width_ - letterbox_width_) / 2;
        letterbox_y_ = (height_ - letterbox_height_) / 2;
        SetAndroidGlMapping(content_width_, content_height_, width_, height_);
    }

    void DestroySurface() {
        if (display_ != EGL_NO_DISPLAY && surface_ != EGL_NO_SURFACE) {
            if (eglGetCurrentSurface(EGL_DRAW) == surface_) {
                eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            }
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
        }
    }

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    ANativeWindow* window_ = nullptr;
    void* gles_library_ = nullptr;
    void* egl_library_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int content_width_ = 960;
    int content_height_ = 640;
    int letterbox_x_ = 0;
    int letterbox_y_ = 0;
    int letterbox_width_ = 960;
    int letterbox_height_ = 640;
    bool reported_context_ = false;
};

}  // namespace

void SetAndroidHostWindow(ANativeWindow* window) {
    std::lock_guard lock(g_window_mutex);
    if (window != nullptr) {
        ANativeWindow_acquire(window);
    }
    if (g_window != nullptr) {
        ANativeWindow_release(g_window);
    }
    g_window = window;
}

std::unique_ptr<HostGLBackend> CreateHostGLBackend() {
    return std::make_unique<AndroidHostGLBackend>();
}

#endif
