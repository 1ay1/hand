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


#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <span>
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

/// The font stack: the first mono face leads (the chrome is mono too), and
/// every installed candidate becomes custom_family(i) so font.family switches
/// live without reading a file on the UI thread.
std::shared_ptr<typo::FontStack> load_fonts(bool system) {
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
        // 1024, not the 2048 maximum: the atlas doubles by itself when its
        // working set needs it (Atlas::maintain), and a terminal's glyphs fit
        // in a fraction of 1024². Starting at 2048 cost 4 MB of coverage and,
        // with one emoji in a prompt, 16 MB of colour plane -- twice, CPU and GPU.
        s = typo::system::default_stack(typo::FontConfig{.mode = typo::RenderMode::hybrid, .atlas_size = 1024}, 400,
                                        false, lead);
        startup::mark("  fonts: default stack");
        (void)typo::system::attach_family(*s, FontFamily::mono, mono);
        startup::mark("  fonts: mono family");
        // every installed candidate into its own role (config.hpp), once per
        // DESIGN: the choices font.family and font.ui offer. A menu of fonts is
        // a menu of looks, so it leaves out what isn't one: a second build of
        // a face already listed ("Inter Variable" after "Inter") and the
        // metric-compatible stand-ins distros install so documents keep their
        // layout (Liberation, Nimbus, Free*: Arial and Courier clones).
        const auto offered = [](std::string_view fam, const std::vector<std::string>& got) {
            for (std::string_view clone : {"Liberation ", "Nimbus ", "FreeMono", "FreeSans", "Arimo", "Cousine", "Tinos"})
                if (fam.starts_with(clone)) return false;
            for (const auto& g : got)   // the same design under another build name
                if (fam.size() > g.size() && fam.starts_with(g) && fam[g.size()] == ' ') return false;
            return true;
        };
        const auto preload = [&](std::span<const std::string_view> cands, std::size_t cap, auto role) {
            const auto& db = typo::system::Database::instance();
            std::vector<std::string> got;
            for (auto cand : cands) {
                if (got.size() >= cap) break;
                const typo::system::FontEntry* e = db.first_available(std::array{cand});
                if (e == nullptr || std::find(got.begin(), got.end(), e->family) != got.end() || !offered(e->family, got)) continue;
                const std::array<std::string_view, 1> one{cand};
                if (auto a = typo::system::attach_family(*s, role(got.size()), one)) got.push_back(a->name);
            }
        };
        preload(mono, hand::kMonoRoles, hand::mono_role);
        startup::mark("  fonts: mono candidates");
        std::vector<std::string_view> ui{"Inter", "Inter Variable", "Geist", "IBM Plex Sans", "Roboto"};
        for (auto f : typo::system::ui_font_families())
            if (std::find(ui.begin(), ui.end(), f) == ui.end()) ui.push_back(f);
        preload(ui, hand::kUiRoles, hand::ui_role);
    }
    return s;   // the family names live on the stack (family_name): App::init reads them
}

/// A screen exercising colours, attributes and wide glyphs, for --png/--headless.

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
        else if (a == "--echo") { mode = Mode::png; shot = "echo"; cfg.shell_cli = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : "cat"; }
        else if (a == "--flood") { mode = Mode::png; shot = "flood"; cfg.shell_cli = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : "yes"; }
    }
    startup::mark("hand: main");
    if (mode == Mode::window) mayag::gpu_prestart();   // ~100 ms of device bring-up, overlapped with all below
    // The window (GPU, fonts) path loads fonts on mayag's worker while the
    // window opens and the GPU comes up; scripted runs need them now.
    // Scripted runs load fonts now; a window loads them on mayag's worker
    // while it opens (each mode gets its fonts as data, see App::Flags).
    std::shared_ptr<typo::FontStack> scripted_fonts;
    if (mode != Mode::window) scripted_fonts = load_fonts(system_fonts);
    if (mode == Mode::window) {
        // the user's hand.vibe; scripted runs use defaults so they are reproducible
        const std::string problems = hand::config_load(cfg, hand::config_path());
        if (!problems.empty()) std::fprintf(stderr, "hand: %s:\n%s", hand::config_path().c_str(), problems.c_str());
    }
    startup::mark("hand: config loaded");
    cfg.demo = mode != Mode::window && shot != "flood" && shot != "echo";
    // Scripted shots in any colour scheme: HAND_SCHEME="Clu" (by name).
    if (const char* sch = std::getenv("HAND_SCHEME"); sch && *sch && mode != Mode::window)
        for (std::size_t k = 0; k < hand::presets::all.size(); ++k)
            if (hand::presets::all[k].name == sch) { cfg.scheme = static_cast<int>(k); break; }

    AppConfig ac{.title = "hand", .size = size, .decorated = cfg.decorations, .transparent = true, .option_as_alt = cfg.option_as_alt, .theme_morph = false, .fonts = scripted_fonts, .log_renderer = false};
    switch (mode) {
        case Mode::window:
            // the fonts load on mayag's worker while the window opens; the
            // App starts from the configuration and those fonts (its Flags)
            ac.font_loader = system_fonts ? +[] { return load_fonts(true); } : +[] { return load_fonts(false); };
            return mayag::run<hand::App>(std::move(ac), [cfg](std::shared_ptr<typo::FontStack> fonts) {
                startup::mark("hand: fonts loaded");
                return hand::App::Flags{cfg, std::move(fonts), hand::cli::read_env(false)};
            });
        case Mode::headless:
            return hand::cli::run_scripted(std::move(ac), {cfg, scripted_fonts, hand::cli::read_env(true)}, size, {});
        case Mode::png:
            return hand::cli::run_scripted(std::move(ac), {cfg, scripted_fonts, hand::cli::read_env(true)}, size, hand::cli::Shot{shot, out});
    }
    return 0;
}
