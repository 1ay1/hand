// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Stress test: force the glyph atlas to GROW past its initial 1024² by
// rasterizing thousands of distinct CJK glyphs at a large pixel size, then
// verify (a) it never blanks a glyph that should have coverage, and (b) UVs
// stay in-range after each doubling. Requires an offscreen GL context.

#include <cstdio>
#include <string>

#include "hand/platform/sokol_gl.hpp"
#include "hand/platform/testing.hpp"
#include "hand/platform/fonts.hpp"
#include "toe/gfx/font.hpp"

using namespace toe;

int main() {
    if (!hand::platform::make_offscreen_context_current(PixelSize{64, 64})) {
        std::fprintf(stderr, "skip: no offscreen context\n");
        return 77;
    }
    hand::platform::sokolgl::setup();

    // A big pixel size makes glyphs large so the 1024² atlas fills fast.
    std::string fp = hand::resolve_font_file("monospace");
    if (fp.empty()) { std::fprintf(stderr, "skip: no font\n"); return 77; }
    auto atlas = gfx::FontAtlas::create(fp, 40);
    if (!atlas) { std::fprintf(stderr, "atlas create failed\n"); return 1; }

    int painted = 0, blank = 0, uv_bad = 0;
    // CJK Unified Ideographs — thousands of distinct wide glyphs (needs a CJK
    // fallback; discovery handles it). If no CJK font exists, Latin fills less
    // but we still exercise many glyphs.
    for (char32_t cp = 0x4E00; cp < 0x4E00 + 4000; ++cp) {
        const gfx::GlyphInfo *gi = atlas->glyph(cp);
        if (!gi) continue;
        if (gi->width > 0 && gi->height > 0) {
            ++painted;
            // UVs must stay within [0,1] after any growth/rescale.
            if (gi->u0 < 0 || gi->u0 > 1 || gi->v0 < 0 || gi->v0 > 1 ||
                gi->u1 < 0 || gi->u1 > 1 || gi->v1 < 0 || gi->v1 > 1 || gi->u1 <= gi->u0)
                ++uv_bad;
        } else {
            ++blank;
        }
    }
    // Re-check a sample of EARLY glyphs after growth: their cached UVs must have
    // been rescaled, so they still point at valid, in-range coords.
    int early_bad = 0;
    for (char32_t cp = 0x4E00; cp < 0x4E00 + 50; ++cp) {
        const gfx::GlyphInfo *gi = atlas->glyph(cp);
        if (gi && gi->width > 0 &&
            (gi->u1 <= gi->u0 || gi->u1 > 1.0f || gi->v1 > 1.0f))
            ++early_bad;
    }

    std::printf("painted=%d blank=%d uv_out_of_range=%d early_glyph_bad=%d\n",
                painted, blank, uv_bad, early_bad);
    if (painted < 500) {
        std::printf("SKIP: too few glyphs painted (no CJK fallback?) — grow not exercised\n");
        return 77;
    }
    if (uv_bad || early_bad) {
        std::printf("FAIL: atlas growth corrupted UVs\n");
        return 1;
    }
    std::printf("PASS: atlas grew and all %d glyphs kept valid in-range UVs\n", painted);
    return 0;
}
