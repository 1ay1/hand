// SPDX-License-Identifier: LGPL-2.0-or-later
//
// opacity_test — verifies window opacity actually lands in the framebuffer
// ALPHA channel. Clears an FBO to a transparent bg (alpha = opacity via the
// pass clear, exactly like the host run loop's begin_frame), renders a frame,
// and reads back alpha for (a) an empty default-bg pixel and (b) a glyph pixel.
// Guards the "opacity only works on the pane" regression.
#include <array>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "toe/gfx/font.hpp"
#include "toe/gfx/renderer.hpp"
#include "hand/platform/sokol_gl.hpp"
#include "hand/platform/testing.hpp"
#include "toe/term/screen.hpp"
#include "toe/vt/parser.hpp"

using namespace toe;

int main() {
    if (!hand::platform::make_offscreen_context_current(PixelSize{64, 64})) {
        std::fprintf(stderr, "offscreen context unavailable\n");
        return 77;
    }
    hand::platform::sokolgl::setup();

    constexpr int W = 240, H = 80;
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return 1;
    glViewport(0, 0, W, H);

    std::string fpath = "/usr/share/fonts/TTF/JetBrainsMono-Regular.ttf";
    if (!std::filesystem::exists(fpath)) {
        namespace fs = std::filesystem;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator("/usr/share/fonts",
                 fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const auto ext = it->path().extension().string();
            if (ext == ".ttf" || ext == ".otf") { fpath = it->path().string(); break; }
        }
    }
    auto atlas = gfx::FontAtlas::create(fpath, 18);
    if (!atlas) return 1;
    auto renderer = gfx::Renderer::create(std::move(*atlas));
    if (!renderer) return 1;

    // Window opacity 0.5 — the renderer scales bg alpha to this.
    renderer->set_opacity(0.5f);

    Extent grid = renderer->cells_for(PixelSize{W, H});
    term::Screen screen{grid};
    vt::Parser parser;
    std::string_view input = "AAAA"; // some glyphs, rest default bg
    parser.feed(std::span<const char>{input.data(), input.size()},
                [&](const vt::Action &a) { toe::Cmds out; screen.apply(a, out); });

    // Clear the pass to the bg colour at alpha=opacity, exactly like the host.
    hand::platform::sokolgl::begin_frame_fbo(fbo, PixelSize{W, H}, 23, 23, 28, 0.5f);
    renderer->draw(screen, PixelSize{W, H}, /*cursor_on=*/false);
    hand::platform::sokolgl::end_frame();
    glFinish();

    std::vector<unsigned char> px(static_cast<std::size_t>(W) * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    auto alpha_at = [&](int x, int y) {
        return static_cast<int>(px[(static_cast<std::size_t>(y) * W + x) * 4 + 3]);
    };

    // An empty region near the bottom-right is default bg: its alpha must equal
    // the window opacity (~128), NOT 255. That IS window transparency.
    const int bg_alpha = alpha_at(W - 3, H - 3);

    // The densest alpha anywhere (a glyph stem) should be ~opaque.
    int max_alpha = 0;
    for (int i = 3; i < static_cast<int>(px.size()); i += 4)
        max_alpha = std::max(max_alpha, static_cast<int>(px[static_cast<std::size_t>(i)]));

    std::printf("default-bg alpha=%d (want ~128), max glyph alpha=%d (want ~255)\n",
                bg_alpha, max_alpha);

    int fails = 0;
    if (bg_alpha > 160) { std::printf("FAIL default bg is opaque (opacity ignored)\n"); ++fails; }
    if (bg_alpha < 90) { std::printf("FAIL default bg too transparent\n"); ++fails; }
    if (max_alpha < 200) { std::printf("FAIL glyphs not opaque\n"); ++fails; }

    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    std::printf(fails ? "%d OPACITY CHECK(S) FAILED\n" : "OPACITY OK\n", fails);
    return fails ? 1 : 0;
}
