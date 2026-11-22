// hand::cli::checks — every cache draws exactly what a fresh frame draws.
//
// The terminal rows are reused across frames on toe's row versions, the
// minimap's history bins on toe's history epoch, the paint and the raster
// tiles on their own keys. A key that misses one input draws stale pixels
// and nothing else notices; a key that changes when nothing did (it hashed
// Theme's padding bytes, once) silently caches nothing. So: run a mutation,
// draw it through every cache, then draw the same model again with memo()
// off -- from scratch -- and the two images must be identical, byte for
// byte; and a one-row change must reuse the other rows.

#include "suites.hpp"

#include <hand/app.hpp>

#include <string>
#include <utility>

namespace hand::cli::checks {

using namespace mayag;

void caches_exact([[maybe_unused]] Runtime& rt, [[maybe_unused]] Vec2 size, Check& ok) {
    const auto out = [&](std::string s) { rt.send(App::Msg{App::Output{std::move(s)}}); };
    // warm the caches, apply one mutation, draw it through them; then draw the
    // same model from scratch. Also: the mutation frame must actually REUSE
    // rows (hits > 0), or the comparison proves nothing.
    const auto same = [&](const char* what, std::string bytes, bool reuses = false) {
        rt.tick();
        rt.tick();
        const auto h0 = rt.host().memo_stats();
        rt.post(App::Msg{App::Output{std::move(bytes)}});
        rt.tick();
        const auto h1 = rt.host().memo_stats();
        if (reuses)   // one row changed: the rest come back from the cache (a key that churns caches nothing)
            ok(h1.hits + 2 >= h0.hits + static_cast<std::uint64_t>(rt.model().tab().term->rows()) - 1,
               (std::string{"the frame after "} + what + " reuses the unchanged rows (" + std::to_string(h1.hits - h0.hits) + " of " +
                std::to_string(rt.model().tab().term->rows()) + ")").c_str());
        const auto cached = rt.window().read_pixels();
        rt.host().set_memo(false);
        rt.tick();
        const auto fresh = rt.window().read_pixels();
        rt.host().set_memo(true);
        rt.tick();
        std::size_t diff = 0;
        for (std::size_t i = 0; i < cached.size() && i < fresh.size(); ++i) diff += cached[i] != fresh[i] ? 1 : 0;
        if (diff != 0) {   // where: a box in window px, so a failure says which widget
            const int W = static_cast<int>(rt.window().size().x * rt.window().dpi_scale());
            int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
            for (std::size_t i = 0; i < cached.size() && i < fresh.size(); ++i)
                if (cached[i] != fresh[i]) {
                    const int p = static_cast<int>(i / 4), x = p % W, y = p / W;
                    x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
                }
            const float s = rt.window().dpi_scale();
            std::printf("    differs in [%g,%g]-[%g,%g] (window px)\n", x0 / s, y0 / s, x1 / s, y1 / s);
        }
        ok(cached.size() == fresh.size() && diff == 0,
           (std::string{"cached frame == fresh frame: "} + what + (diff ? " (" + std::to_string(diff) + " bytes differ)" : "")).c_str());
    };
    std::string hist;
    for (int i = 0; i < 300; ++i) hist += "\x1b[3" + std::to_string(1 + i % 6) + "m" + std::to_string(i) + "\x1b[0m " + std::string(20 + i % 40, 'x') + "\r\n";
    out(hist);
    same("typing on the live row", "typed", true);
    same("erasing it", "\b\b\b\b\b\x1b[K", true);
    same("rewriting a row in the middle", "\x1b[5;1H\x1b[2K\x1b[41mred row\x1b[0m", true);
    same("an OSC 4 palette edit (same cells, new colour)", "\x1b]4;1;rgb:00/ff/00\x1b\\");
    same("an OSC 104 palette reset", "\x1b]104\x1b\\");
    same("a scroll region", "\x1b[3;12r\x1b[12;1H\r\nscrolled in a region\x1b[r");
    same("a newline scrolling history", "\x1b[999;1H\r\nnext line");
    same("the alternate screen", "\x1b[?1049h\x1b[2J\x1b[Halt screen");
    same("back from the alternate screen", "\x1b[?1049l");
    same("clearing the scrollback", "\x1b[3J");
    same("a full reset", "\x1b" "c");
    // zoom: every glyph re-laid at a new size, the grid re-fitted
    out("zoom me\r\n");
    for (int i = 0; i < 6; ++i) {
        rt.tick(); rt.tick();
        const auto before = rt.window().read_pixels();
        rt.press_key(i % 2 == 0 ? Key::equal : Key::minus, Mods{.ctrl = true});
        const auto after = rt.window().read_pixels();
        ok(before != after, (std::string{"zoom "} + (i % 2 == 0 ? "in" : "out") + " changes the very next frame").c_str());
        same(i % 2 == 0 ? "after zooming in" : "after zooming out", "");
    }
    {   // an empty screen with the cursor in its blink's off phase: the zoom is still seen at once
        out("\x1b[2J\x1b[H");
        rt.send(App::Msg{mayag::Edit<App::Model>{[](App::Model& mm) { mm.cursor_on = false; mm.tab().cur_a = 0.0f; }}});
        rt.tick(); rt.tick();
        const auto before = rt.window().read_pixels();
        rt.press_key(Key::equal, Mods{.ctrl = true});
        ok(rt.window().read_pixels() != before, "zooming a blank screen mid-blink shows at once (the cursor, at its new size)");
        rt.press_key(Key::minus, Mods{.ctrl = true});
    }
}

}  // namespace hand::cli::checks
