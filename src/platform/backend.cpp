// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand::run — the ONE runtime decision, then a fully-monomorphic loop.
//
// Linux ships all backends in one binary. This function makes the single
// Wayland-vs-X11-vs-offscreen choice from the environment, then enters that
// backend's own `toe::run<...>` instantiation (via run_wayland / run_x11 /
// run_offscreen, each in its backend TU). That loop inlines to the backend's
// direct calls — no vtable, no per-op branch. The choice costs one `switch` per
// process; every frame op after it is statically known.
//
// If the preferred display backend fails to open (rc < 0), fall through to the
// next, so a headless box still lands on offscreen.

#include "hand/app.hpp"
#include "hand/platform/posix_pty.hpp"
#include "hand/platform/fonts.hpp"

#include "toe/gfx/font.hpp" // FontAtlas::probe_cell_size

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // GetDpiForSystem
#endif

namespace hand {

namespace {

// The backend the environment asks for (before trying to open anything).
[[nodiscard]] Backend choose(Backend force) {
    if (force != Backend::automatic) return force;
    if (const char *h = std::getenv("TOE_HEADLESS"); h && *h) return Backend::offscreen;
#if defined(_WIN32)
    // Windows has exactly one native window system.
    return Backend::win32;
#elif defined(__APPLE__)
    // macOS has exactly one native window system.
    return Backend::cocoa;
#else
    if (const char *wl = std::getenv("WAYLAND_DISPLAY"); wl && *wl) return Backend::wayland;
    if (const char *x = std::getenv("DISPLAY"); x && *x) return Backend::x11;
    return Backend::offscreen;
#endif
}

} // namespace

int run(const toe::Config &cfg_in, const toe::WindowConfig &win, Backend force,
        const std::vector<std::string> &child_argv) {
    // Process creation is the HOST's job: the engine never forks (see
    // posix_pty.hpp). NOTE the ordering — the child is spawned near the END of
    // this function, AFTER font/DPI resolution, because the initial pty grid is
    // derived from the cell size and a pty born at the wrong size makes ConPTY
    // repaint (see SpawnCommand::cols).
    toe::Config cfg = cfg_in;
#if defined(_WIN32)
    // Font is host policy on Windows too. The config size is a LOGICAL POINT
    // size; convert to pixels using the ACTUAL system DPI rather than assuming
    // 96, so the grid is correctly sized on scaled displays (the common laptop
    // case at 125%/150%). GetDpiForSystem is the per-monitor-aware query.
    {
        const double dpi = static_cast<double>(::GetDpiForSystem());
        const double px = static_cast<double>(cfg.font_pixel_size) * (dpi / 72.0);
        cfg.font_pixel_size = static_cast<int>(px + 0.5);
    }
    // Resolve a concrete font file here (host policy) via DirectWrite, so the
    // engine needs no Windows font branch.
    if (cfg.font_file.empty()) {
        std::string f = resolve_font_file(cfg.font_family);
        if (!f.empty()) cfg.font_file = std::move(f);
    }
    {
        FontStyleFiles sf = resolve_font_styles(cfg.font_family, cfg.font_file);
        if (cfg.font_file_bold.empty()) cfg.font_file_bold = std::move(sf.bold);
        if (cfg.font_file_italic.empty()) cfg.font_file_italic = std::move(sf.italic);
        if (cfg.font_file_bold_italic.empty())
            cfg.font_file_bold_italic = std::move(sf.bold_italic);
    }
#elif defined(__APPLE__)
    // Font is host policy on macOS. Interpret the size as a LOGICAL point size;
    // run_cocoa multiplies it by the display's backing scale factor. Note toe
    // sizes by ascent+descent (stbtt_ScaleForPixelHeight), and SF Mono's
    // vertical metrics run ~1.18x its em, so a given pixel_size renders glyphs
    // ~15% smaller than the same "pt" in Terminal.app. 16 logical (=> 32px @2x)
    // lands the em at ~13pt (32 * 2048/2412 = 27.2px em, floor 13pt), matching
    // a comfortable Terminal.app default.
    // Only set when the user hasn't overridden (still at toe's default of 18).
    if (cfg.font_pixel_size == 18) cfg.font_pixel_size = 16;
    // Font discovery is host policy: resolve a concrete macOS face and hand it
    // to toe via font_file, so the engine needs no macOS font-path branch.
    if (cfg.font_file.empty()) {
        std::string f = resolve_font_file(cfg.font_family);
        if (!f.empty()) cfg.font_file = std::move(f);
    }
#else
    // Font is host policy on Linux too. The config size is a LOGICAL POINT
    // size, but toe wants PIXELS — convert at 96 DPI (pt * 96/72 = pt * 4/3),
    // the conversion the config documents but the engine never performs. Then
    // honour a HiDPI scale so text isn't tiny on scaled displays: GDK_SCALE is
    // the near-universal signal set by Wayland/GTK sessions; default 1.
    {
        double dpi_scale = 1.0;
        if (const char *g = std::getenv("GDK_SCALE"); g && *g) {
            double v = std::atof(g);
            if (v >= 1.0 && v <= 8.0) dpi_scale = v;
        }
        // points -> pixels @96dpi, then HiDPI scale. Round to nearest.
        const double px = static_cast<double>(cfg.font_pixel_size) * (96.0 / 72.0) * dpi_scale;
        cfg.font_pixel_size = static_cast<int>(px + 0.5);
    }
    // Resolve a concrete font file here (host policy), so the engine needs no
    // font-discovery branch. Skips if the user gave an explicit file.
    if (cfg.font_file.empty()) {
        std::string f = resolve_font_file(cfg.font_family);
        if (!f.empty()) cfg.font_file = std::move(f);
    }
    // Resolve the family's REAL bold / italic / bold-italic variants so styled
    // text renders from actual faces instead of synthesized embolden/shear.
    // Only fill fields the user didn't set explicitly.
    {
        FontStyleFiles sf = resolve_font_styles(cfg.font_family, cfg.font_file);
        if (cfg.font_file_bold.empty()) cfg.font_file_bold = std::move(sf.bold);
        if (cfg.font_file_italic.empty()) cfg.font_file_italic = std::move(sf.italic);
        if (cfg.font_file_bold_italic.empty()) cfg.font_file_bold_italic = std::move(sf.bold_italic);
    }
#endif

    // --- spawn the child, at the RIGHT grid size ----------------------------
    // Done here, after font/DPI resolution, so we can predict the cell geometry
    // toe will choose. Getting this right matters on Windows: ConPTY repaints
    // its whole viewport whenever the pseudoconsole is resized, so a pty created
    // at a placeholder 80x24 and corrected a moment later makes the shell print
    // its banner (and every command's output) twice.
    //
    // We ask toe for the cell size of the resolved font — the same computation
    // the renderer performs — rather than guessing, so the grid matches what
    // Session::resize() will independently arrive at and the first resize is a
    // no-op.
    SpawnCommand sc;
    sc.argv = child_argv; // empty -> $SHELL / %COMSPEC% (the spawner resolves it)
    // sc.term keeps its default (xterm-256color); TERM is host policy and is not
    // carried in toe::Config.
    {
        // Mirrors toe::gfx::Renderer::cells_for(): reserve the padding on every
        // edge, then integer-divide by the cell. Same inputs, same result, so
        // the grid toe computes once the window exists matches this one and the
        // first Session::resize() is a no-op.
        const toe::PixelSize cell =
            toe::gfx::FontAtlas::probe_cell_size(cfg.font_file, cfg.font_pixel_size);
        if (cell.w > 0 && cell.h > 0) {
            const int w = std::max(0, win.size.w - 2 * cfg.padding);
            const int h = std::max(0, win.size.h - 2 * cfg.padding);
            sc.cols = std::max(1, w / cell.w);
            sc.rows = std::max(1, h / cell.h);
        }
    }

    auto fd = spawn_pty(sc);
    if (!fd) {
        std::fprintf(stderr, "hand: %s\n", fd.error().message.c_str());
        return 1;
    }
    cfg.source = *fd;

    switch (choose(force)) {
#if defined(_WIN32)
    case Backend::win32:
    default:
        return run_win32(cfg, win);
#elif defined(__APPLE__)
    case Backend::cocoa:
    default:
        return run_cocoa(cfg, win);
#else
    case Backend::wayland: {
        const int rc = run_wayland(cfg, win);
        if (rc >= 0) return rc; // else fall back
        [[fallthrough]];
    }
    case Backend::x11: {
        // HAND_TABS=1 selects the multi-terminal Activity-Tabs loop (Elm/actor
        // GUI); default is the tuned single-terminal loop.
        const char *tabs = std::getenv("HAND_TABS");
        const int rc = (tabs && *tabs) ? run_x11_tabbed(cfg, win) : run_x11(cfg, win);
        if (rc >= 0) return rc;
        [[fallthrough]];
    }
    case Backend::offscreen:
    default:
        return run_offscreen(cfg, win);
#endif
    }
}

} // namespace hand
