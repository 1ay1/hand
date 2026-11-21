// hand::cli::checks — visual settings, checked on actual pixels.

#include "suites.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace hand::cli::checks {

using namespace mayag;

void visual_settings([[maybe_unused]] Runtime& rt, [[maybe_unused]] Vec2 size, Check& ok) {
    // ── the visual settings, checked on actual PIXELS ────────────────────
    using E = mayag::Edit<hand::App::Model>;
    const auto px_at = [&](float x, float y) {
        rt.tick();
        const auto px = rt.window().read_pixels();
        const int W = static_cast<int>(rt.window().size().x * rt.window().dpi_scale());
        const auto i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)) * 4;
        return std::array<int, 4>{px[i], px[i + 1], px[i + 2], px[i + 3]};
    };
    const float bx = size.x - 4.0f, by = size.y - 4.0f;   // bare background, bottom-right
    const auto opaque = px_at(bx, by);
    rt.send(hand::App::Msg{E{[](hand::App::Model& m) { m.cfg.opacity = 0.5f; }}});
    const auto half = px_at(bx, by);
    std::printf("    alpha: opaque=%d  opacity 0.5=%d\n", opaque[3], half[3]);
    ok(opaque[3] == 255 && half[3] > 110 && half[3] < 145, "window.opacity reaches the pixels (alpha ~50%)");
    rt.send(hand::App::Msg{E{[](hand::App::Model& m) { m.cfg.opacity = 1.0f; m.cfg.dim_unfocused = 0.6f; }}});
    const auto lit = px_at(bx, by);
    rt.send(hand::App::Msg{E{[](hand::App::Model& m) { m.focused = false; }}});
    const auto dim = px_at(bx, by);
    ok(dim[0] + dim[1] + dim[2] < lit[0] + lit[1] + lit[2], "colors.dim_unfocused darkens an unfocused window");
    rt.send(hand::App::Msg{E{[](hand::App::Model& m) { m.focused = true; m.cfg.dim_unfocused = 0.0f; }}});
    rt.send(hand::App::Msg{hand::App::Output{std::string{"\a"}}});
    const auto flash = px_at(bx, by);
    ok(flash[0] + flash[1] + flash[2] > lit[0] + lit[1] + lit[2], "bell.visual flashes the screen on BEL");
    rt.send(hand::App::Msg{E{[](hand::App::Model& m) { m.cfg.visual_bell = false; m.bell_at = -1.0; }}});
    rt.send(hand::App::Msg{hand::App::Output{std::string{"\a"}}});
    const auto quiet = px_at(bx, by);
    ok(quiet == lit, "and stays dark with it off");
    rt.send(hand::App::Msg{E{[](hand::App::Model& m) { m.cfg.visual_bell = true; }}});
}

}  // namespace hand::cli::checks
