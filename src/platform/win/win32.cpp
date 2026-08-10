// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The Win32 backend — hand's native Windows window, modelling toe::App.
//
// Sibling of wayland.cpp / x11.cpp / cocoa.mm. Fully native: a raw Win32
// HWND, a DXGI FLIP swapchain, and D3D11 for the GPU path. No GLFW, no SDL, no
// ANGLE, no GL translation layer, no libepoxy.
//
// ─── the two decisions that make this fast ─────────────────────────────────
//
// 1. DXGI_SWAP_EFFECT_FLIP_DISCARD. The legacy BLT swap effects copy the whole
//    backbuffer through the DWM every frame. FLIP hands the DWM the surface
//    directly — no copy, lower latency, and it is the only path that reaches
//    true independent-flip fullscreen. We also set ALLOW_TEARING so an
//    unthrottled Present (interval 0) is not clamped to the refresh rate, which
//    is what makes flood throughput measurable rather than vsync-bound.
//
// 2. The loop NEVER calls GetMessage. GetMessage blocks on the message queue
//    alone and cannot see the PTY, which would force a reader thread. Instead
//    the single blocking point is the WinReactor's MsgWaitForMultipleObjectsEx
//    (see win_reactor.hpp), and poll_events() drains the queue with a
//    non-blocking PeekMessage loop. That keeps the terminal single-threaded and
//    lets PTY output and window input share one kernel wait, exactly as epoll
//    does on Linux.
//
// Input is translated from WM_* into toe's platform-neutral events. Text comes
// from WM_CHAR (already the OS's composed, keyboard-layout-correct result,
// including dead keys and IME), while non-text keys come from WM_KEYDOWN — the
// standard split that avoids re-implementing layout handling.

#include "hand/app.hpp"
#include "hand/platform/surface.hpp"
#include "hand/platform/win_reactor.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h> // ShellExecuteW (open_url)
#include <d3d11.h>
#include <dxgi1_5.h>

#include "hand/platform/sokol_d3d11.hpp"
#include "hand/platform/backend_base.hpp"

namespace hand::platform {

namespace {

constexpr const wchar_t *kWindowClass = L"hand.terminal.window";

// UTF-16 -> UTF-8 for text delivered by WM_CHAR.
[[nodiscard]] std::string narrow(const wchar_t *w, int len) {
    if (len <= 0) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), n, nullptr, nullptr);
    return s;
}

[[nodiscard]] std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                        nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Current modifier state. GetKeyState is the correct source inside a message
// handler: it reflects the state as of the message being processed, not now.
[[nodiscard]] Modifiers mods_now() {
    Modifiers m{};
    m.ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    m.shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
    m.alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
    return m;
}

// VK_* -> toe::SpecialKey. Returns false for keys that produce text instead
// (those arrive via WM_CHAR and must not be handled twice).
[[nodiscard]] bool special_from_vk(WPARAM vk, toe::SpecialKey &out) {
    switch (vk) {
    case VK_RETURN: out = toe::SpecialKey::Enter; return true;
    case VK_BACK: out = toe::SpecialKey::Backspace; return true;
    case VK_TAB: out = toe::SpecialKey::Tab; return true;
    case VK_ESCAPE: out = toe::SpecialKey::Escape; return true;
    case VK_UP: out = toe::SpecialKey::Up; return true;
    case VK_DOWN: out = toe::SpecialKey::Down; return true;
    case VK_LEFT: out = toe::SpecialKey::Left; return true;
    case VK_RIGHT: out = toe::SpecialKey::Right; return true;
    case VK_HOME: out = toe::SpecialKey::Home; return true;
    case VK_END: out = toe::SpecialKey::End; return true;
    case VK_PRIOR: out = toe::SpecialKey::PageUp; return true;
    case VK_NEXT: out = toe::SpecialKey::PageDown; return true;
    case VK_DELETE: out = toe::SpecialKey::Delete; return true;
    case VK_INSERT: out = toe::SpecialKey::Insert; return true;
    case VK_F1: out = toe::SpecialKey::F1; return true;
    case VK_F2: out = toe::SpecialKey::F2; return true;
    case VK_F3: out = toe::SpecialKey::F3; return true;
    case VK_F4: out = toe::SpecialKey::F4; return true;
    case VK_F5: out = toe::SpecialKey::F5; return true;
    case VK_F6: out = toe::SpecialKey::F6; return true;
    case VK_F7: out = toe::SpecialKey::F7; return true;
    case VK_F8: out = toe::SpecialKey::F8; return true;
    case VK_F9: out = toe::SpecialKey::F9; return true;
    case VK_F10: out = toe::SpecialKey::F10; return true;
    case VK_F11: out = toe::SpecialKey::F11; return true;
    case VK_F12: out = toe::SpecialKey::F12; return true;
    default: return false;
    }
}

} // namespace

class Win32Surface final : public BackendBase<Win32Surface> {
public:
    static Result<std::unique_ptr<Win32Surface>> open(std::string_view title, PixelSize initial);

    ~Win32Surface();
    Win32Surface(const Win32Surface &) = delete;
    Win32Surface &operator=(const Win32Surface &) = delete;

    // --- the platform-specific half of toe::App ----------------------------
    // Everything else (overlay panes, chords, bind_terminal, flush,
    // swap_damaged, event_fd/repeat_fd) comes from BackendBase.
    void begin_frame(toe::PixelSize px, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                     float a = 1.0f) {
        sokold3d::begin_frame(px, r, g, b, a);
    }
    void end_frame() { sokold3d::end_frame(); }

    void swap();

    [[nodiscard]] PixelSize pixel_size() const { return size_; }

    void poll_events(const toe::EventSink &sink);
    [[nodiscard]] bool should_close() const { return closed_; }

    [[nodiscard]] toe::Readiness wait_readable(int pty_fd, toe::WaitDeadline d) {
        const toe::Readiness r = reactor_.wait(pty_fd, d);
        // A config-watch wake is not a PTY/window event: service it and report
        // a spurious wake, exactly as the Linux backend does.
        if (reactor_.config_ready()) reload_config_if_watched();
        return r;
    }

    void set_title(std::string_view title) { ::SetWindowTextW(hwnd_, widen(title).c_str()); }

    void set_clipboard(std::string_view utf8);
    [[nodiscard]] std::string get_clipboard();
    void open_url(std::string_view uri);

    // Win32 also has to hand the config watcher's EVENT to its reactor, so it
    // extends (rather than replaces) the base implementation.
    void bind_terminal(toe::Terminal &term, toe::PixelSize px) {
        BackendBase::bind_terminal(term, px);
        reactor_.set_config_event(settings().config_event());
    }

private:
    Win32Surface() = default;

    static LRESULT CALLBACK wndproc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    [[nodiscard]] bool create_device();
    [[nodiscard]] bool create_swapchain();
    void create_views();
    void release_views();
    void resize_swapchain(int w, int h);

    HWND hwnd_ = nullptr;
    ID3D11Device *device_ = nullptr;
    ID3D11DeviceContext *ctx_ = nullptr;
    IDXGISwapChain1 *swap_ = nullptr;
    ID3D11RenderTargetView *rtv_ = nullptr;
    ID3D11DepthStencilView *dsv_ = nullptr;
    ID3D11Texture2D *depth_ = nullptr;

    PixelSize size_{800, 500};
    bool closed_ = false;
    bool tearing_ = false;    // driver/compositor supports ALLOW_TEARING
    bool sokol_ready_ = false;

    WinReactor reactor_{};

    // Click-count tracking for double/triple-click selection.
    DWORD last_click_ms_ = 0;
    int last_click_x_ = 0, last_click_y_ = 0;
    int clicks_ = 0;
    toe::MouseButton last_click_btn_ = toe::MouseButton::left;

    // Events produced by the wndproc are queued here, because a wndproc cannot
    // return values to poll_events' sink directly (Windows calls it re-entrantly
    // from DispatchMessage).
    std::vector<toe::win::Event> pending_{};
};

// ─────────────────────────── device + swapchain ─────────────────────────────

bool Win32Surface::create_device() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    // Try hardware first; fall back to WARP so a VM or RDP session still runs
    // (the renderer is identical, just software-rasterised).
    const D3D_DRIVER_TYPE types[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                      D3D_FEATURE_LEVEL_10_1};
    for (const D3D_DRIVER_TYPE t : types) {
        D3D_FEATURE_LEVEL got{};
        const HRESULT hr = ::D3D11CreateDevice(nullptr, t, nullptr, flags, want,
                                               ARRAYSIZE(want), D3D11_SDK_VERSION, &device_, &got,
                                               &ctx_);
        if (SUCCEEDED(hr)) return true;
    }
    return false;
}

bool Win32Surface::create_swapchain() {
    IDXGIDevice *dxgi_dev = nullptr;
    if (FAILED(device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_dev))))
        return false;
    IDXGIAdapter *adapter = nullptr;
    if (FAILED(dxgi_dev->GetAdapter(&adapter))) { dxgi_dev->Release(); return false; }
    IDXGIFactory2 *factory = nullptr;
    if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&factory)))) {
        adapter->Release();
        dxgi_dev->Release();
        return false;
    }

    // ALLOW_TEARING is what lets Present(0, …) actually run unthrottled rather
    // than being clamped to vblank by the DWM.
    IDXGIFactory5 *f5 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5),
                                          reinterpret_cast<void **>(&f5)))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow,
                                              sizeof(allow)))) {
            tearing_ = allow != FALSE;
        }
        f5->Release();
    }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = static_cast<UINT>(size_.w);
    sd.Height = static_cast<UINT>(size_.h);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // THREE buffers with FLIP_DISCARD, not two. With only two, Present must
    // wait for the display to release the front buffer before the CPU can start
    // the next frame, so a keystroke arriving just after a present waits a full
    // frame. A third buffer decouples them: the CPU always has a free surface to
    // render into, which is what keeps input->photon latency low under load.
    sd.BufferCount = 3;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;

    const HRESULT hr =
        factory->CreateSwapChainForHwnd(device_, hwnd_, &sd, nullptr, nullptr, &swap_);
    // We handle Alt+Enter ourselves; DXGI's default handler fights the terminal.
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

    // DXGI lets the driver queue up to 3 frames ahead by default. For a game
    // that hides jitter; for a terminal it is pure added latency — a keystroke's
    // echo can sit behind two already-queued frames. Ask for 1 so what we
    // present is what the next vblank shows.
    if (swap_) {
        IDXGIDevice1 *dev1 = nullptr;
        if (SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice1),
                                              reinterpret_cast<void **>(&dev1))) &&
            dev1) {
            dev1->SetMaximumFrameLatency(1);
            dev1->Release();
        }
    }

    factory->Release();
    adapter->Release();
    dxgi_dev->Release();
    return SUCCEEDED(hr);
}

void Win32Surface::create_views() {
    ID3D11Texture2D *back = nullptr;
    if (FAILED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&back))))
        return;
    device_->CreateRenderTargetView(back, nullptr, &rtv_);
    back->Release();

    D3D11_TEXTURE2D_DESC dd{};
    dd.Width = static_cast<UINT>(size_.w);
    dd.Height = static_cast<UINT>(size_.h);
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(device_->CreateTexture2D(&dd, nullptr, &depth_)))
        device_->CreateDepthStencilView(depth_, nullptr, &dsv_);

    // Publish to the sokol glue; it reads these at pass-begin.
    sokold3d::views().rtv = rtv_;
    sokold3d::views().dsv = dsv_;
}

void Win32Surface::release_views() {
    sokold3d::views().rtv = nullptr;
    sokold3d::views().dsv = nullptr;
    if (dsv_) { dsv_->Release(); dsv_ = nullptr; }
    if (depth_) { depth_->Release(); depth_ = nullptr; }
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
}

void Win32Surface::resize_swapchain(int w, int h) {
    if (!swap_ || w <= 0 || h <= 0) return;
    if (w == size_.w && h == size_.h && rtv_) return;
    size_ = PixelSize{w, h};
    // Views must be gone before ResizeBuffers — they hold references to the
    // backbuffer DXGI is about to recreate.
    release_views();
    swap_->ResizeBuffers(0, static_cast<UINT>(w), static_cast<UINT>(h), DXGI_FORMAT_UNKNOWN,
                         tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
    create_views();
}

void Win32Surface::swap() {
    if (!swap_) return;
    // Interval 0 + ALLOW_TEARING: present as fast as the terminal produces
    // frames. toe already rate-limits presents; we must not add vsync latency
    // on top of its own pacing.
    const UINT flags = tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    swap_->Present(0, flags);
}

// ───────────────────────────── window + events ──────────────────────────────

Result<std::unique_ptr<Win32Surface>> Win32Surface::open(std::string_view title,
                                                         PixelSize initial) {
    auto s = std::unique_ptr<Win32Surface>(new Win32Surface());
    s->size_ = initial;

    // Per-monitor DPI so the grid stays crisp on mixed-DPI setups. Best-effort:
    // older Windows simply keeps system DPI.
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = &Win32Surface::wndproc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    // IDC_* are MAKEINTRESOURCE values; mingw types them as char* so the -W
    // form needs the wide reinterpretation.
    wc.hCursor = ::LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_IBEAM));
    wc.lpszClassName = kWindowClass;
    ::RegisterClassExW(&wc); // benign failure if already registered

    // Size the CLIENT area to the requested size, not the outer frame.
    RECT r{0, 0, initial.w, initial.h};
    ::AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);

    s->hwnd_ = ::CreateWindowExW(0, kWindowClass, widen(title).c_str(), WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                                 nullptr, nullptr, wc.hInstance, s.get());
    if (!s->hwnd_) return toe::fail("CreateWindow failed");

    if (!s->create_device()) return toe::fail("D3D11CreateDevice failed");
    if (!s->create_swapchain()) return toe::fail("CreateSwapChainForHwnd failed");
    s->create_views();

    sokold3d::setup(s->device_, s->ctx_);
    s->sokol_ready_ = true;

    ::ShowWindow(s->hwnd_, SW_SHOW);
    ::UpdateWindow(s->hwnd_);
    return s;
}

Win32Surface::~Win32Surface() {
    if (sokol_ready_) sg_shutdown();
    release_views();
    if (swap_) swap_->Release();
    if (ctx_) ctx_->Release();
    if (device_) device_->Release();
    if (hwnd_) ::DestroyWindow(hwnd_);
}

LRESULT CALLBACK Win32Surface::wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(lp);
        auto *created = static_cast<Win32Surface *>(cs->lpCreateParams);
        ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
        // CRITICAL: adopt the HWND here. Messages (WM_NCCREATE/WM_CREATE/
        // WM_SIZE) are dispatched DURING CreateWindowExW, before it returns and
        // assigns hwnd_. Without this, handle() would forward to
        // DefWindowProcW(nullptr, ...), which fails — and a falsy WM_NCCREATE
        // result makes CreateWindowExW abort and return null.
        if (created) created->hwnd_ = h;
    }
    auto *self = reinterpret_cast<Win32Surface *>(::GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return ::DefWindowProcW(h, msg, wp, lp);
    return self->handle(msg, wp, lp);
}

LRESULT Win32Surface::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        closed_ = true;
        pending_.push_back(toe::win::CloseRequested{});
        return 0;

    case WM_DESTROY:
        closed_ = true;
        return 0;

    case WM_SIZE: {
        if (wp == SIZE_MINIMIZED) return 0;
        const int w = LOWORD(lp), h = HIWORD(lp);
        // WM_SIZE arrives during CreateWindowExW, before the D3D11 swapchain
        // exists; resize_swapchain() no-ops until then, and open() sizes the
        // first buffers itself.
        resize_swapchain(w, h);
        pending_.push_back(toe::win::Resized{PixelSize{w, h}});
        return 0;
    }

    case WM_SETFOCUS:
        pending_.push_back(toe::win::FocusChanged{true});
        return 0;
    case WM_KILLFOCUS:
        pending_.push_back(toe::win::FocusChanged{false});
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const Modifiers m = mods_now();

        // hand's reserved chords. The MAPPING (which physical keys) is
        // platform-specific and lives here; what each chord DOES is shared, in
        // BackendBase::handle_chord — so the bindings can't drift per platform.
        // VK_OEM_COMMA/VK_OEM_2 are the physical ',' and '/?' keys.
        if (m.ctrl && m.shift) {
            const Chord c = (wp == VK_OEM_COMMA) ? Chord::ToggleSettings
                            : (wp == VK_OEM_2)   ? Chord::ToggleHelp
                                                 : Chord::None;
            if (handle_chord(c)) return 0;
        }

        toe::SpecialKey sk{};
        if (special_from_vk(wp, sk)) {
            KeyEvent ke{};
            ke.key = sk;
            ke.mods = m;
            ke.kind = (lp & (1 << 30)) ? KeyEvent::Kind::repeat : KeyEvent::Kind::press;
            pending_.push_back(toe::win::KeyPressed{ke});
            return 0;
        }
        // Ctrl chords produce no WM_CHAR for many keys, and where they do the
        // control byte is what we want. Synthesise the letter and let toe's
        // keymap apply the C0/CSI-u encoding.
        if (m.ctrl && wp >= 'A' && wp <= 'Z') {
            KeyEvent ke{};
            ke.key = toe::TextInput{std::string(1, static_cast<char>(wp))};
            ke.mods = m;
            pending_.push_back(toe::win::KeyPressed{ke});
            return 0;
        }
        break; // fall through to TranslateMessage -> WM_CHAR for text
    }

    case WM_CHAR:
    case WM_SYSCHAR: {
        // The OS has already applied layout, dead keys and IME composition.
        const auto c = static_cast<wchar_t>(wp);
        // Ctrl+Shift chords are handled in WM_KEYDOWN above; TranslateMessage
        // still synthesises a control byte for them, which must not reach the
        // child (Ctrl+Shift+, would otherwise send a stray \x0C etc).
        {
            const Modifiers m = mods_now();
            if (m.ctrl && m.shift) return 0;
        }
        if (c < 0x20 && c != 0x09 && c != 0x0d) return 0; // control bytes: handled above
        wchar_t buf[2] = {c, 0};
        std::string utf8 = narrow(buf, 1);
        if (utf8.empty()) return 0;
        KeyEvent ke{};
        ke.key = toe::TextInput{std::move(utf8)};
        ke.mods = mods_now();
        pending_.push_back(toe::win::KeyPressed{ke});
        return 0;
    }

    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        ::SetCapture(hwnd_);
        const auto b = (msg == WM_LBUTTONDOWN)   ? toe::MouseButton::left
                       : (msg == WM_MBUTTONDOWN) ? toe::MouseButton::middle
                                                 : toe::MouseButton::right;
        // Windows delivers WM_*BUTTONDBLCLK for the second click, but toe wants
        // a running click_count; GetMessageTime-based tracking is what the
        // double/triple-click word/line selection depends on.
        const DWORD now = ::GetMessageTime();
        const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        // NB: not named `near` — <windows.h> defines that as a legacy macro.
        const bool close_by =
            std::abs(x - last_click_x_) <= 4 && std::abs(y - last_click_y_) <= 4;
        if (b == last_click_btn_ && close_by &&
            (now - last_click_ms_) <= ::GetDoubleClickTime()) {
            clicks_ = clicks_ >= 3 ? 1 : clicks_ + 1;
        } else {
            clicks_ = 1;
        }
        last_click_ms_ = now;
        last_click_x_ = x;
        last_click_y_ = y;
        last_click_btn_ = b;

        pending_.push_back(toe::win::MouseDown{b, x, y, clicks_, mods_now()});
        return 0;
    }
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP: {
        ::ReleaseCapture();
        const auto b = (msg == WM_LBUTTONUP)   ? toe::MouseButton::left
                       : (msg == WM_MBUTTONUP) ? toe::MouseButton::middle
                                               : toe::MouseButton::right;
        pending_.push_back(toe::win::MouseUp{b, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), mods_now()});
        return 0;
    }
    case WM_MOUSEMOVE: {
        const bool held = (wp & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON)) != 0;
        pending_.push_back(toe::win::MouseMove{GET_X_LPARAM(lp), GET_Y_LPARAM(lp), held});
        return 0;
    }

    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        // Ctrl+wheel is the universal zoom gesture.
        if (::GetKeyState(VK_CONTROL) & 0x8000) {
            pending_.push_back(toe::win::FontZoom{delta, -1});
            return 0;
        }
        pending_.push_back(toe::win::MouseWheel{0, delta}); // dy>0 = up
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; // we always repaint fully; skip the flicker-inducing erase

    default:
        break;
    }
    return ::DefWindowProcW(hwnd_, msg, wp, lp);
}

void Win32Surface::poll_events(const toe::EventSink &sink) {
    // Non-blocking drain. The BLOCKING wait is wait_readable() alone, so the
    // PTY and the window share a single kernel wait (see win_reactor.hpp).
    MSG msg;
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        ::TranslateMessage(&msg); // WM_KEYDOWN -> WM_CHAR for text keys
        ::DispatchMessageW(&msg);
        if (msg.message == WM_QUIT) closed_ = true;
    }
    // The wndproc queued events re-entrantly; deliver them now.
    for (const auto &e : pending_) sink(e);
    pending_.clear();
}

// ─────────────────────────────── clipboard ──────────────────────────────────

void Win32Surface::set_clipboard(std::string_view utf8) {
    if (!::OpenClipboard(hwnd_)) return;
    ::EmptyClipboard();
    const std::wstring w = widen(utf8);
    const std::size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL g = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void *p = ::GlobalLock(g)) {
            std::memcpy(p, w.c_str(), bytes);
            ::GlobalUnlock(g);
            ::SetClipboardData(CF_UNICODETEXT, g);
        } else {
            ::GlobalFree(g);
        }
    }
    ::CloseClipboard();
}

std::string Win32Surface::get_clipboard() {
    if (!::IsClipboardFormatAvailable(CF_UNICODETEXT)) return {};
    if (!::OpenClipboard(hwnd_)) return {};
    std::string out;
    if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
        if (auto *p = static_cast<const wchar_t *>(::GlobalLock(h))) {
            out = narrow(p, static_cast<int>(::wcslen(p)));
            ::GlobalUnlock(h);
        }
    }
    ::CloseClipboard();
    return out;
}

void Win32Surface::open_url(std::string_view uri) {
    ::ShellExecuteW(nullptr, L"open", widen(uri).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace hand::platform

#include "toe/run.hpp"

namespace hand {

// Win32App::open — construct the handle from an opened Win32 surface.
// Instantiated here where Win32Surface is complete.
template <>
toe::Result<Win32App> Win32App::open(const toe::WindowConfig &win) {
    auto s = platform::Win32Surface::open(win.title, win.size);
    if (!s) return toe::fail(s.error().message);
    return Win32App{s->release()};
}

// The backend entry: enter the fully-monomorphic loop for Win32App. toe::run<>
// is instantiated HERE, where Win32Surface is complete — every frame op inlines
// to a direct D3D11/Win32 call, no vtable, no per-op branch.
int run_win32(const toe::Config &cfg, const toe::WindowConfig &win) {
    return toe::run<Win32App>(cfg, win);
}

} // namespace hand
