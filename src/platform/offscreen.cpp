// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Headless / offscreen surface. Creates a valid EGL GL context with NO window
// and NO compositor — via EGL_PLATFORM_SURFACELESS_MESA when available, else a
// tiny pbuffer on the default display. This is what lets the render/cache tests
// (and any batch/CI job) exercise the real GL renderer without a Wayland or X11
// server. Event, clipboard and title methods are inert stubs.

#include "hand/app.hpp"
#include "hand/platform/surface.hpp"
#include "hand/reactor.hpp"

#include <cstdio>
#include <memory>

// Fully-native GL: sokol GLCORE owns the GL entry points (and gives us the
// <GL/gl.h> prototypes via the helper); real EGL headers create the headless
// context. eglext.h provides EGL_PLATFORM_SURFACELESS_MESA. No epoxy.
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "hand/platform/sokol_gl.hpp"

namespace hand::platform {

class OffscreenSurface final {
public:
    static Result<std::unique_ptr<OffscreenSurface>> open(PixelSize size) {
        auto s = std::unique_ptr<OffscreenSurface>(new OffscreenSurface());
        s->size_ = size;

        // Use the default display + a pbuffer draw surface. This works on every
        // EGL implementation (Mesa's swrast included) without needing the EGL
        // 1.5 eglGetPlatformDisplay entry point, which epoxy will abort on if
        // the driver doesn't provide it.
        EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (dpy == EGL_NO_DISPLAY) {
            return fail("offscreen: no EGL display (need a working libEGL, e.g. Mesa)");
        }
        if (!eglInitialize(dpy, nullptr, nullptr)) {
            return fail("offscreen: eglInitialize failed");
        }
        s->dpy_ = dpy;

        eglBindAPI(EGL_OPENGL_API);
        const EGLint cfg_attribs[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_NONE,
        };
        EGLConfig cfg{};
        EGLint num = 0;
        if (!eglChooseConfig(dpy, cfg_attribs, &cfg, 1, &num) || num == 0) {
            return fail("offscreen: no matching EGL config");
        }

        // Match the on-screen backends: request 4.4 core, fall back to 4.1
        // (the glsl410 shader floor; a 3.3 context would fail sg_make_pipeline).
        const EGLint ctx44[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 4,
                                EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
        const EGLint ctx41[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 1,
                                EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
        s->ctx_ = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx44);
        if (s->ctx_ == EGL_NO_CONTEXT) {
            s->ctx_ = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx41);
        }
        if (s->ctx_ == EGL_NO_CONTEXT) {
            return fail("offscreen: eglCreateContext failed");
        }

        // A pbuffer draw surface sized to the request.
        {
            const EGLint pb[] = {EGL_WIDTH, size.w > 0 ? size.w : 1,
                                 EGL_HEIGHT, size.h > 0 ? size.h : 1, EGL_NONE};
            s->surf_ = eglCreatePbufferSurface(dpy, cfg, pb);
            if (s->surf_ == EGL_NO_SURFACE) {
                return fail("offscreen: eglCreatePbufferSurface failed");
            }
        }

        if (!eglMakeCurrent(dpy, s->surf_, s->surf_, s->ctx_)) {
            return fail("offscreen: eglMakeCurrent failed");
        }
        // Set sokol up while the context is current — toe::run builds the
        // Renderer (sg_make_pipeline) before the first begin_frame.
        sokolgl::setup();
        s->sokol_ready_ = true;
        return s;
    }

    ~OffscreenSurface() {
        if (dpy_ != EGL_NO_DISPLAY) {
            if (sokol_ready_) sg_shutdown();
            eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (surf_ != EGL_NO_SURFACE) eglDestroySurface(dpy_, surf_);
            if (ctx_ != EGL_NO_CONTEXT) eglDestroyContext(dpy_, ctx_);
            eglTerminate(dpy_);
        }
    }

    void swap() {
        if (surf_ != EGL_NO_SURFACE) eglSwapBuffers(dpy_, surf_);
        else glFlush();
    }
    void begin_frame(toe::PixelSize px, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                     float a = 1.0f) {
        sokolgl::begin_frame(px, r, g, b, a);
    }
    void end_frame() { sokolgl::end_frame(); }
    void swap_damaged(toe::DamageRect) { swap(); }
    [[nodiscard]] PixelSize pixel_size() const { return size_; }
    void set_title(std::string_view) {}
    void window_action(int) {}
    void set_clipboard(std::string_view) {}
    [[nodiscard]] std::string get_clipboard() { return {}; }
    void open_url(std::string_view) {}
    [[nodiscard]] bool overlay_active() const { return false; }
    bool overlay_event(const toe::win::Event &) { return false; }
    void overlay_render(toe::Terminal &, toe::PixelSize) {}
    void bind_terminal(toe::Terminal &, toe::PixelSize) {}
    void poll_events(const toe::EventSink &) {}
    [[nodiscard]] int event_fd() const { return -1; }
    [[nodiscard]] int repeat_fd() const { return -1; }
    [[nodiscard]] bool should_close() const { return false; }
    void flush() {}
    [[nodiscard]] toe::Readiness wait_readable(int pty_fd, toe::WaitDeadline d) {
        return wait_.wait(pty_fd, d); // no window/repeat fd on the headless backend
    }

private:
    OffscreenSurface() = default;
    hand::TerminalWait wait_{}; // headless: no window/repeat fds, only the PTY
    PixelSize size_{};
    EGLDisplay dpy_ = EGL_NO_DISPLAY;
    EGLContext ctx_ = EGL_NO_CONTEXT;
    EGLSurface surf_ = EGL_NO_SURFACE;
    bool sokol_ready_ = false; // sokol_gfx set up in open() while ctx is current
};

} // namespace hand::platform

#include "toe/run.hpp"

namespace hand {

// OffscreenApp::open — construct the handle from an opened offscreen surface.
// Instantiated here where OffscreenSurface is complete.
template <>
toe::Result<OffscreenApp> OffscreenApp::open(const toe::WindowConfig &win) {
    auto s = platform::OffscreenSurface::open(win.size);
    if (!s) return toe::fail(s.error().message);
    return OffscreenApp{s->release()};
}

// The backend entry: enter the fully-monomorphic loop for OffscreenApp. Called
// by hand::run after it makes the one runtime backend choice. toe::run<> is
// instantiated HERE, where OffscreenSurface is complete — so its direct calls
// inline and nothing leaks.
int run_offscreen(const toe::Config &cfg, const toe::WindowConfig &win) {
    return toe::run<OffscreenApp>(cfg, win);
}

} // namespace hand

namespace hand::platform {
// Test/tooling support: create an offscreen EGL context and make it current on
// the calling thread. Returns true on success. The context is intentionally
// leaked for the process lifetime (callers are short-lived test binaries that
// issue GL until exit); this keeps the helper free of the anonymous-namespace
// OffscreenSurface type in its public signature.
bool make_offscreen_context_current(PixelSize size) {
    auto s = OffscreenSurface::open(size);
    if (!s) {
        std::fprintf(stderr, "hand: %s\n", s.error().message.c_str());
        return false;
    }
    // Leak the surface for the process lifetime so its EGL context stays
    // current; short-lived test binaries issue GL until exit.
    (void)s->release(); // s is Result<unique_ptr<...>>; s-> is the unique_ptr
    return true;
}

} // namespace hand::platform
