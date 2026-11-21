// hand — a terminal, like foot: one window, one shell. toe is the VT engine.
//
//   hand                     open a window running $SHELL
//   hand -e CMD ...          run CMD instead of the shell
//   hand --png out.png       render one frame of a canned screen (src/cli/shots.cpp)
//   hand --headless          the self-test, exit code = failures (src/cli/selftest.cpp)
//
// main only parses arguments, loads fonts and config, and picks a mode; each
// scripted mode lives in src/cli/.

#include <hand/app.hpp>
#include <hand/core/shell.hpp>
#include <hand/core/pty.hpp>

#include "cli/cli.hpp"

#include <filesystem>
#include <thread>
#include <chrono>

#include <mayag/widget/ssh.hpp>
#include <mayag/widget/file_pane.hpp>
#include <hand/config/schemes_gen.hpp>
#include <hand/ssh/sftp.hpp>

#include <mayag/backend/tiled.hpp>
#ifdef MAYAG_WITH_VULKAN
#include <mayag/backend/vulkan.hpp>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <hand/core/platform.hpp>
#include <csignal>

#if defined(_WIN32)
#  include <windows.h>   // --cat-v puts the console in raw mode
#else
#  include <termios.h>
#  include <unistd.h>
#endif

namespace {

using namespace mayag;

constexpr std::array<std::string_view, 9> kPreferredMono{
    "JetBrainsMono Nerd Font", "JetBrains Mono", "Iosevka", "Berkeley Mono", "Geist Mono",
    "CaskaydiaCove Nerd Font Mono", "Lilex Nerd Font Mono", "Fira Code", "DejaVu Sans Mono"};

/// hand's preferred monospace faces, then the platform's own (SF Mono, Menlo
/// on macOS; Cascadia, Consolas on Windows), so a machine with none of the
/// preferred ones installed still gets a monospace grid, never the UI font.
std::vector<std::string_view> mono_candidates() {
    std::vector<std::string_view> v{kPreferredMono.begin(), kPreferredMono.end()};
    for (auto f : typo::system::mono_font_families())
        if (std::find(v.begin(), v.end(), f) == v.end()) v.push_back(f);
    return v;
}

std::shared_ptr<typo::FontStack> g_stack;

/// The font stack: the first mono face leads (the chrome is mono too), and
/// every installed candidate becomes custom_family(i) so font.family switches
/// live without reading a file on the UI thread.
std::shared_ptr<typo::FontStack> load_fonts(bool system) {
    std::vector<std::string> names;
    std::shared_ptr<typo::FontStack> s;
    if (!system) {
        s = std::make_shared<typo::FontStack>();
        s->add(typo::lastresort::face());
    } else {
        const auto mono = mono_candidates();
        std::array<std::string_view, 1> lead{mono.back()};
        for (auto cand : mono)
            if (typo::system::Database::instance().first_available(std::array{cand})) { lead[0] = cand; break; }
        startup::mark("  fonts: database");
        s = typo::system::default_stack(typo::FontConfig{.mode = typo::RenderMode::hybrid, .atlas_size = 2048}, 400,
                                        false, lead);
        startup::mark("  fonts: default stack");
        (void)typo::system::attach_family(*s, FontFamily::mono, mono);
        startup::mark("  fonts: mono family");
        // the platform's UI face (SF Pro, Segoe, Inter, Cantarell...) for window
        // chrome that reads as an app, not a terminal: the settings window
        {
            std::vector<std::string_view> ui{"Inter", "Inter Variable"};
            for (auto f : typo::system::ui_font_families()) ui.push_back(f);
            for (auto f : {"Noto Sans", "Cantarell", "DejaVu Sans"}) ui.push_back(f);
            (void)typo::system::attach_family(*s, FontFamily::display, ui);
        }
        startup::mark("  fonts: display family");
        for (auto cand : mono) {
            if (names.size() >= custom_family_count) break;
            const std::array<std::string_view, 1> one{cand};
            if (auto got = typo::system::attach_family(*s, custom_family(names.size()), one))
                if (std::find(names.begin(), names.end(), got->name) == names.end()) names.push_back(got->name);
        }
    }
    hand::set_font_names(std::move(names));
    return s;
}

/// A screen exercising colours, attributes and wide glyphs, for --png/--headless.
constexpr std::string_view kDemo =
    "\x1b[1;32m~/src/hand\x1b[0m \x1b[35mmain\x1b[0m \x1b[2m+2\x1b[0m\r\n"
    "\x1b[1;34m❯\x1b[0m ls --color\r\n"
    "\x1b[1;34minclude\x1b[0m  \x1b[1;34msrc\x1b[0m  CMakeLists.txt  \x1b[32mbuild.sh\x1b[0m\r\n"
    "\x1b[1;34m❯\x1b[0m printf colours\r\n"
    "\x1b[30;40m  \x1b[41m  \x1b[42m  \x1b[43m  \x1b[44m  \x1b[45m  \x1b[46m  \x1b[47m  \x1b[0m\r\n"
    "\x1b[1mbold\x1b[0m \x1b[3mitalic\x1b[0m \x1b[4munderline\x1b[0m \x1b[4:3;58:5:1mcurly\x1b[0m "
    "\x1b[9mstrike\x1b[0m \x1b[7mreverse\x1b[0m \x1b[38;2;255;121;198mtruecolor\x1b[0m 漢字 ✓\r\n"
    "\x1b[1;34m❯\x1b[0m ";

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view{argv[1]} == "--version") { std::puts("hand"); return 0; }
    enum class Mode { window, png, headless } mode = Mode::window;
    std::string out = "hand.png";
    Vec2 size{960, 600};
    bool system_fonts = true;
    hand::Config cfg;
    std::string shot;
    if (auto r = hand::cli::fixture(argc, argv)) return *r;   // --emit-red, --cat-v
    if (auto r = hand::cli::repro(argc, argv)) return *r;     // --repro-*

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--png") { mode = Mode::png; if (i + 1 < argc && argv[i + 1][0] != '-') out = argv[++i]; }
        else if (a == "--headless") mode = Mode::headless;
        else if (a == "--last-resort") system_fonts = false;
        else if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%fx%f", &size.x, &size.y);
        else if (a == "--font-size" && i + 1 < argc) cfg.font_size = std::strtof(argv[++i], nullptr);
        else if (a == "-e" && i + 1 < argc) cfg.shell_cli = argv[++i];
        else if (a == "--shot" && i + 1 < argc) shot = argv[++i];
        else if (a == "--bench") { mode = Mode::png; shot = "bench"; }
        else if (a == "--flood") { mode = Mode::png; shot = "flood"; cfg.shell_cli = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : "yes"; }
    }
    startup::mark("hand: main");
    if (mode == Mode::window) mayag::gpu_prestart();   // ~100 ms of device bring-up, overlapped with all below
    // The window (GPU, fonts) path loads fonts on mayag's worker while the
    // window opens and the GPU comes up; scripted runs need them now.
    static bool s_system_fonts;   // read by the captureless loader below
    s_system_fonts = system_fonts;
    if (mode != Mode::window) g_stack = load_fonts(system_fonts);
    if (mode == Mode::window) {
        // the user's hand.vibe; scripted runs use defaults so they are reproducible
        const std::string problems = hand::config_load(cfg, hand::config_path());
        if (!problems.empty()) std::fprintf(stderr, "hand: %s:\n%s", hand::config_path().c_str(), problems.c_str());
    }
    startup::mark("hand: config loaded");
    cfg.demo = mode != Mode::window && shot != "flood";
    // Scripted shots in any colour scheme: HAND_SCHEME="Clu" (by name).
    if (const char* sch = std::getenv("HAND_SCHEME"); sch && *sch && mode != Mode::window)
        for (std::size_t k = 0; k < hand::presets::all.size(); ++k)
            if (hand::presets::all[k].name == sch) { cfg.scheme = static_cast<int>(k); break; }
    hand::App::config = cfg;

    hand::App::cell_width = [](float s, int family) {
        // g_stack is set by the time the app measures (run() collects the
        // font worker before init); bound on first use.
        static typo::StackMeasurer measurer{*g_stack};
        TextStyle st; st.size = s;
        st.family = hand::font_names().empty() ? FontFamily::mono
                  : custom_family(static_cast<std::size_t>(std::clamp(family, 0, static_cast<int>(hand::font_names().size()) - 1)));
        const float w = measurer.advance("MMMMMMMMMM", st) / 10.0f;
        return w > 0.0f ? w : s * 0.6f;
    };

    AppConfig ac{.title = "hand", .size = size, .decorated = cfg.decorations, .transparent = true, .option_as_alt = cfg.option_as_alt, .theme_morph = false, .fonts = g_stack, .log_renderer = false};
    if (mode == Mode::window)
        ac.font_loader = [] {
            g_stack = load_fonts(s_system_fonts);
            // App::config is read only once run() has collected this job
            const std::string p = hand::resolve_font_family(hand::App::config);
            if (!p.empty()) std::fprintf(stderr, "hand: %s:\n%s", hand::config_path().c_str(), p.c_str());
            startup::mark("hand: fonts loaded (worker)");
            return g_stack;
        };
    switch (mode) {
        case Mode::window: return mayag::run<hand::App>(std::move(ac));
        case Mode::headless:
        case Mode::png:
            return mayag::run_headless<hand::App>(std::move(ac), [&](auto& rt) {
                rt.send(hand::App::Msg{hand::App::Output{std::string{kDemo}}});
                rt.tick();
                if (mode == Mode::headless) return hand::cli::selftest(rt, size);
                return hand::cli::shoot(rt, size, shot, out);
            });
    }
    return 0;
}
