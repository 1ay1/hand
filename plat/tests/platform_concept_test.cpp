// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Concept-conformance test for the platform capability contracts. A complete
// MockPlatform models Platform/Clipboard/Sys (static_assert proves it); a
// DeficientPlatform that's missing window_action must NOT model Platform. The
// negative half is a compile-fail case (PLAT_PC_CASE=1) driven from CMake.

#include "plat/platform.hpp"

#include <cstdio>
#include <span>
#include <string>

using namespace plat;

// A minimal FrameOps + Gpu so the platform can render.
struct Ops {
    void draw(std::span<const QuadInstance>, ResourceId) {}
    void finish() {}
};
struct MockGpu {
    Size sz{640, 480};
    [[nodiscard]] Size size() const noexcept { return sz; }
    Texture create_texture(Size, std::span<const std::uint8_t>) { return Texture{ResourceId{1}}; }
    void update_texture(ResourceId, Size, std::span<const std::uint8_t>) {}
    Frame<Ops> begin_frame(Color) { return Frame<Ops>{Ops{}}; }
};

struct MockPlatform {
    MockGpu gpu_;
    // Window
    [[nodiscard]] Size size() const noexcept { return gpu_.size(); }
    [[nodiscard]] float scale() const noexcept { return 1.0f; }
    [[nodiscard]] bool should_close() const noexcept { return false; }
    void set_title(std::string_view) {}
    void set_decorations(Decorations) {}
    void window_action(WinAction) {}
    // Waker
    Woke wait(std::span<const int>, Deadline) { return {}; }
    // Gpu access
    MockGpu &gpu() noexcept { return gpu_; }
    Frame<Ops> begin_frame(Color c) { return gpu_.begin_frame(c); }
    // Input
    template <class Sink> void poll_events(Sink &&) {}
    // Clipboard
    [[nodiscard]] std::string get_clipboard() { return {}; }
    void set_clipboard(std::string_view) {}
    [[nodiscard]] std::string get_primary() { return {}; }
    void set_primary(std::string_view) {}
    // Sys
    void open_url(std::string_view) {}
    void notify(std::string_view, std::string_view) {}
};

static_assert(Gpu<MockGpu>, "MockGpu models Gpu");
static_assert(Window<MockPlatform>, "MockPlatform models Window");
static_assert(Waker<MockPlatform>, "MockPlatform models Waker");
static_assert(Platform<MockPlatform>, "MockPlatform models the composed Platform");
static_assert(Clipboard<MockPlatform>, "MockPlatform models the Clipboard refinement");
static_assert(Sys<MockPlatform>, "MockPlatform models the Sys refinement");

#if PLAT_PC_CASE == 1
// A backend missing window_action() must be REJECTED by the Window concept and
// therefore by Platform — this must not compile.
struct Deficient {
    [[nodiscard]] Size size() const noexcept { return {}; }
    [[nodiscard]] float scale() const noexcept { return 1.0f; }
    [[nodiscard]] bool should_close() const noexcept { return false; }
    void set_title(std::string_view) {}
    void set_decorations(Decorations) {}
    // window_action MISSING
    Woke wait(std::span<const int>, Deadline) { return {}; }
    MockGpu &gpu() noexcept { static MockGpu g; return g; }
    Frame<Ops> begin_frame(Color) { return Frame<Ops>{Ops{}}; }
};
static_assert(Platform<Deficient>, "must fail: Deficient is missing window_action");
#endif

int main() {
    std::printf("ALL PLATFORM CONCEPT TESTS PASS\n");
    return 0;
}
