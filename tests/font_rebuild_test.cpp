// SPDX-License-Identifier: LGPL-2.0-or-later
//
// font_rebuild_test — guards against the "shader pool exhausted" (sokol id 154)
// leak that hit on repeated font changes. Each font/ligature change move-assigns
// a fresh Renderer over the live one; if the old GL shader isn't destroyed, the
// sokol shader pool (default 128) fills and floods stderr. Here we rebuild the
// renderer FAR more times than the pool size and assert sokol logged no error.
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <string>

#include "toe/gfx/font.hpp"
#include "toe/gfx/renderer.hpp"
#include "hand/platform/sokol_gl.hpp"
#include "hand/platform/testing.hpp"

using namespace toe;

namespace {
int g_errors = 0;
int g_last_item = 0;
// Sokol log callback: log_level 0 = panic, 1 = error. Count those.
void log_cb(const char *tag, uint32_t level, uint32_t item_id, const char *msg,
            uint32_t line, const char *file, void *user) {
    (void)tag; (void)msg; (void)line; (void)file; (void)user;
    if (level <= 1) { ++g_errors; g_last_item = static_cast<int>(item_id); }
}

std::string find_font() {
    namespace fs = std::filesystem;
    for (const char *p : {"/usr/share/fonts/TTF/DejaVuSansMono.ttf",
                          "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"})
        if (fs::exists(p)) return p;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator("/usr/share/fonts",
             fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        auto ext = it->path().extension().string();
        if (ext == ".ttf" || ext == ".otf") return it->path().string();
    }
    return {};
}
} // namespace

int main() {
    if (!hand::platform::make_offscreen_context_current(PixelSize{64, 64})) {
        std::fprintf(stderr, "offscreen context unavailable\n");
        return 77; // skip when no display / EGL
    }

    // Set sokol up with our own logger so we can detect pool-exhaustion errors.
    sg_desc desc{};
    desc.environment.defaults.color_format = SG_PIXELFORMAT_RGBA8;
    desc.environment.defaults.depth_format = SG_PIXELFORMAT_NONE;
    desc.environment.defaults.sample_count = 1;
    desc.logger.func = log_cb;
    sg_setup(&desc);

    const std::string fpath = find_font();
    if (fpath.empty()) { std::fprintf(stderr, "no font found\n"); return 77; }

    auto atlas = gfx::FontAtlas::create(fpath, 16);
    if (!atlas) { std::fprintf(stderr, "atlas failed\n"); return 1; }
    auto renderer = gfx::Renderer::create(std::move(*atlas));
    if (!renderer) { std::fprintf(stderr, "renderer failed\n"); return 1; }

    // Simulate 300 font changes: each rebuilds the atlas + Renderer and
    // move-assigns it over the live one (exactly what Session::set_font does).
    // 300 >> the 128-entry shader pool, so a per-rebuild shader leak fails fast.
    constexpr int kRebuilds = 300;
    int made = 0;
    for (int i = 0; i < kRebuilds; ++i) {
        const int size = 12 + (i % 20); // vary size so the atlas really rebuilds
        auto a = gfx::FontAtlas::create(fpath, size);
        if (!a) { std::fprintf(stderr, "atlas rebuild %d failed\n", i); return 1; }
        auto r = gfx::Renderer::create(std::move(*a));
        if (!r) {
            std::fprintf(stderr, "renderer rebuild %d failed (pool exhausted?)\n", i);
            return 1;
        }
        renderer = std::move(*r); // <- the move-assign that must free old GL objs
        ++made;
    }

    std::printf("rebuilt renderer %d times; sokol errors=%d (last item=%d)\n",
                made, g_errors, g_last_item);

    int fails = 0;
    if (made != kRebuilds) { std::printf("FAIL not all rebuilds succeeded\n"); ++fails; }
    if (g_errors != 0) {
        std::printf("FAIL sokol logged %d error(s); last item id=%d "
                    "(154 = shader pool exhausted)\n", g_errors, g_last_item);
        ++fails;
    }

    renderer = gfx::Renderer::create(gfx::FontAtlas::create(fpath, 16).value()).value();
    // The renderer destructs at return (freeing its GL objects); shut sokol down
    // after via atexit-like ordering is unnecessary here — leak-checked above.
    (void)renderer;

    std::printf(fails ? "%d FONT-REBUILD CHECK(S) FAILED\n" : "FONT REBUILD OK\n", fails);
    return fails ? 1 : 0;
}
