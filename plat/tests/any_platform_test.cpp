// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Test the AnyPlatform erasure seam: wrap a concrete mock, drive it through the
// erased API (poll, render-with-scoped-frame, clipboard refinement query), and
// assert the concrete Frame's linear lifecycle is preserved behind the seam.
// The negative case (PLAT_AP_CASE=1) proves a non-Platform is rejected by the
// concept-gated constructor.

#include "plat/any_platform.hpp"

#include <cstdio>
#include <span>
#include <string>
#include <vector>

using namespace plat;

static int fails = 0;
static void ck(bool ok, const char *n) {
    if (!ok) { std::printf("FAIL %s\n", n); ++fails; }
}

// Deleters (this TU is "the backend").
void plat::TextureDeleter::operator()(ResourceId) const noexcept {}
void plat::PipelineDeleter::operator()(ResourceId) const noexcept {}

static int g_finishes = 0;
static int g_draws = 0;

struct Ops {
    void draw(std::span<const QuadInstance>, ResourceId) { ++g_draws; }
    void finish() { ++g_finishes; }
};
struct MockGpu {
    [[nodiscard]] Size size() const noexcept { return {320, 200}; }
    Texture create_texture(Size, std::span<const std::uint8_t>) { return Texture{ResourceId{7}}; }
    void update_texture(ResourceId, Size, std::span<const std::uint8_t>) {}
    Frame<Ops> begin_frame(Color) { return Frame<Ops>{Ops{}}; }
};

// A complete platform WITH the Clipboard refinement.
struct FullPlatform {
    MockGpu g;
    bool closed = false;
    int polled = 0;

    [[nodiscard]] Size size() const noexcept { return g.size(); }
    [[nodiscard]] float scale() const noexcept { return 2.0f; }
    [[nodiscard]] bool should_close() const noexcept { return closed; }
    void set_title(std::string_view) {}
    void set_decorations(Decorations) {}
    void window_action(WinAction) {}
    template <class Sink> void poll_events(Sink &&sink) {
        sink(Event{Resized{Size{320, 200}}}); // emit one event
    }
    Woke wait(std::span<const int>, Deadline) { return {true, 0}; }
    MockGpu &gpu() noexcept { return g; }
    Frame<Ops> begin_frame(Color c) { return g.begin_frame(c); }
    [[nodiscard]] std::string get_clipboard() { return "clip"; }
    void set_clipboard(std::string_view) {}
    [[nodiscard]] std::string get_primary() { return "prim"; }
    void set_primary(std::string_view) {}
};
static_assert(Platform<FullPlatform>);
static_assert(Clipboard<FullPlatform>);

// A platform WITHOUT Clipboard/Sys, to prove the refinement query is runtime.
struct BarePlatform {
    MockGpu g;
    [[nodiscard]] Size size() const noexcept { return g.size(); }
    [[nodiscard]] float scale() const noexcept { return 1.0f; }
    [[nodiscard]] bool should_close() const noexcept { return false; }
    void set_title(std::string_view) {}
    void set_decorations(Decorations) {}
    void window_action(WinAction) {}
    template <class Sink> void poll_events(Sink &&) {}
    Woke wait(std::span<const int>, Deadline) { return {}; }
    MockGpu &gpu() noexcept { return g; }
    Frame<Ops> begin_frame(Color c) { return g.begin_frame(c); }
};
static_assert(Platform<BarePlatform>);
static_assert(!Clipboard<BarePlatform>);

#if PLAT_AP_CASE == 1
// Wrapping a non-Platform must be rejected by the concept-gated constructor.
struct NotAPlatform { int x; };
void bad() {
    AnyPlatform any{NotAPlatform{}}; // <-- must fail: constraint not satisfied
}
#endif

int main() {
    g_draws = g_finishes = 0;

    // Erase a full platform; drive it through the seam.
    AnyPlatform any{FullPlatform{}};
    ck(any.size() == Size{320, 200}, "erased size forwards");
    ck(any.scale() == 2.0f, "erased scale forwards");

    // poll through the erasure: the emitted Resized event reaches our sink.
    int events = 0;
    any.poll_events([&](const Event &e) {
        ++events;
        ck(std::holds_alternative<Resized>(e), "erased event round-trips as Resized");
    });
    ck(events == 1, "one event delivered through the seam");

    // Scoped render: the concrete linear Frame lives inside render(); we draw
    // through the erased AnyFrame, and it must auto-finish exactly once.
    std::vector<QuadInstance> cells(2);
    any.render(Color{10, 10, 12}, [&](AnyFrame &f) {
        f.draw(cells, ResourceId{7});
        f.draw(cells, ResourceId{});
    });
    ck(g_draws == 2, "two erased draws forwarded to the concrete frame");
    ck(g_finishes == 1, "concrete linear frame auto-finished once behind the seam");

    // Clipboard refinement is present at runtime for FullPlatform.
    ck(any.has_clipboard(), "full platform reports clipboard capability");
    ck(any.get_clipboard() == "clip", "erased clipboard get forwards");
    ck(any.get_primary() == "prim", "erased primary get forwards");

    // A bare platform reports NO clipboard, and the calls are safe no-ops.
    AnyPlatform bare{BarePlatform{}};
    ck(!bare.has_clipboard(), "bare platform reports no clipboard capability");
    ck(bare.get_clipboard().empty(), "bare clipboard get is a safe empty no-op");

    std::printf(fails ? "%d ANYPLATFORM TEST(S) FAILED\n" : "ALL ANYPLATFORM TESTS PASS\n", fails);
    return fails ? 1 : 0;
}
