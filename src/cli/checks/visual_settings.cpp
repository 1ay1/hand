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
    rt.send(hand::App::Msg{E{[](hand::App::Model& m) { m.cfg.visual_bell = false; m.tab().bell_at = -1.0; }}});
    rt.send(hand::App::Msg{hand::App::Output{std::string{"\a"}}});
    const auto quiet = px_at(bx, by);
    ok(quiet == lit, "and stays dark with it off");
    rt.send(hand::App::Msg{E{[](hand::App::Model& m) { m.cfg.visual_bell = true; }}});

    {   // colors.min_contrast: a pale yellow on white is lifted to readable,
        // keeping its hue; off, the pixels are the program's own
        const auto darkest = [&]() {   // the darkest pixel in the window's top half (the one text row)
            rt.tick();
            const auto px = rt.window().read_pixels();
            const int W = static_cast<int>(rt.window().size().x * rt.window().dpi_scale());
            const int H = static_cast<int>(rt.window().size().y * rt.window().dpi_scale());
            int best = 255 * 3;
            for (int y = 0; y < H / 2; ++y)
                for (int x = 8; x < W / 2; ++x) {
                    const auto i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)) * 4;
                    best = std::min(best, px[i] + px[i + 1] + px[i + 2]);
                }
            return best;
        };
        rt.send(hand::App::Msg{E{[](hand::App::Model& m) {
            m.palette.bg = 0xFAFAFA; m.palette.fg = 0x383A42; m.cursor_on = false; m.tab().cur_a = 0.0f; }}});
        rt.send(hand::App::Msg{hand::App::Output{std::string{"\x1b[2J\x1b[H\x1b[38;2;240;220;130mMMMMMMMMMMMMMMMMMMMM\x1b[0m"}}});
        const int off = darkest();
        rt.send(hand::App::Msg{E{[](hand::App::Model& m) { m.cfg.min_contrast = 4.5f; }}});
        const int on = darkest();
        std::printf("    min_contrast: darkest text pixel off=%d  4.5:1=%d (of 765)\n", off, on);
        ok(on < off - 150, "colors.min_contrast darkens pale text on a light background");
        rt.send(hand::App::Msg{E{[](hand::App::Model& m) { m.cfg.min_contrast = 1.0f; }}});
        ok(darkest() == off, "...and off, the program's colour is drawn exactly");
        rt.send(hand::App::Msg{E{[](hand::App::Model& m) { m.palette = hand::palette_of(m.cfg); }}});
    }
}

}  // namespace hand::cli::checks
