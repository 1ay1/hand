// hand — the terminal.
//
//   hand                     open a window
//   hand --png out.png       render a frame through the real host
//                            (HAND_SHOT=palette|settings|type|glass|paper|phosphor)
//   hand --headless          run scripted, assert, exit (CI)

#include <hand/app.hpp>

#include <mayag/backend/tiled.hpp>
#ifdef MAYAG_WITH_VULKAN
#include <mayag/backend/vulkan.hpp>
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

using namespace mayag;

/// Monospace candidates, best first. Each found becomes custom_family(i),
/// so the settings view previews every face in itself and choosing one is a
/// style change, not a reload.
constexpr std::array<std::string_view, 8> kMono{
    "JetBrainsMono Nerd Font", "JetBrains Mono", "Berkeley Mono", "Iosevka", "Geist Mono",
    "CaskaydiaCove Nerd Font Mono", "Fira Code", "DejaVu Sans Mono"};

struct Fonts {
    std::shared_ptr<typo::FontStack> stack;
    std::vector<std::string>         names;
};

[[nodiscard]] Fonts load_fonts(bool system) {
    Fonts f;
    if (!system) {
        f.stack = std::make_shared<typo::FontStack>();
        f.stack->add(typo::lastresort::face());
        return f;
    }
    // The chrome is set in the monospace face too: under looks::terminal
    // every label is a column of a grid, which is the look.
    std::array<std::string_view, 1> lead{kMono.front()};
    for (auto cand : kMono) if (typo::system::Database::instance().first_available(std::array{cand})) { lead[0] = cand; break; }
    f.stack = typo::system::default_stack(typo::FontConfig{.mode = typo::RenderMode::hybrid, .atlas_size = 2048}, 400,
                                          false, lead);
    for (std::string_view cand : kMono) {
        if (f.names.size() >= custom_family_count) break;
        const std::array<std::string_view, 1> one{cand};
        if (auto got = typo::system::attach_family(*f.stack, custom_family(f.names.size()), one)) {
            if (std::ranges::find(f.names, got->name) != f.names.end()) continue;
            f.names.push_back(got->name);
            if (f.names.size() == 1) f.stack->select_family(FontFamily::mono, got->face);
        }
    }
    return f;
}

struct Hand : hand::App {
    static inline const Fonts* fonts = nullptr;
    static Cmd init(Model& m) {
        Cmd c = hand::App::init(m);
        if (fonts) m.faces = fonts->names;
        return c;
    }
};

int script(auto& rt) {
    int fails = 0;
    const auto ok = [&](bool c, const char* what) {
        std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what);
        if (!c) ++fails;
    };
    const auto& M = [&]() -> const hand::App::Model& { return rt.model(); };
    using App = hand::App;
    rt.tick();
    ok(M().tabs.size() == 3, "three tabs");
    ok(M().tab().panes().size() == 3, "first tab split three ways");

    rt.click(dsl::part_id("tabs", 1));
    ok(M().active == 1, "clicking a term_tabs tab selects it");
    rt.click(dsl::part_id("tabs", 0));

    rt.press_key(Key::d, Mods{.shift = true, .ctrl = true});
    ok(M().tab().panes().size() == 4, "ctrl-shift-d splits");
    rt.press_key(Key::w, Mods{.shift = true, .ctrl = true});
    ok(M().tab().panes().size() == 3, "ctrl-shift-w closes");

    rt.press_key(Key::p, Mods{.shift = true, .ctrl = true});
    ok(M().palette_open, "ctrl-shift-p opens the picker");
    rt.type("glac");
    rt.press_key(Key::enter);
    ok(M().look.scheme == hand::SchemeId::glacier && !M().palette_open, "enter runs the best match");

    rt.press_key(Key::comma, Mods{.ctrl = true});
    ok(M().settings_open, "ctrl-, opens appearance");
    rt.send(App::Msg{mayag::Edit<App::Model>{[](App::Model& m) { m.section = hand::settings::Section::terminal; }}});
    rt.click(dsl::node_id("cursor.blink"));
    ok(!M().look.cursor_blink, "the mayag switch edits cursor.blink inline");
    rt.click(dsl::part_id("cursor.style", 1));
    ok(M().look.cursor == hand::CursorStyle::beam, "the mayag segmented edits cursor.style inline");
    ok(hand::settings::modified_count(M().look) == 3, "three settings modified");
    const bool found = rt.click_text("reset");
    ok(found, "the reset button is on screen");
    ok(hand::settings::modified_count(M().look) == 0, "reset restores defaults");
    rt.press_key(Key::escape);
    ok(!M().settings_open, "esc closes");

    std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
    return fails;
}

}  // namespace

int main(int argc, char** argv) {
    enum class Mode { window, png, headless } mode = Mode::window;
    std::string out = "hand.png";
    Vec2 size{1440, 900};
    bool system_fonts = true;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--png") { mode = Mode::png; if (i + 1 < argc && argv[i + 1][0] != '-') out = argv[++i]; }
        else if (a == "--headless") mode = Mode::headless;
        else if (a == "--last-resort") system_fonts = false;
        else if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%fx%f", &size.x, &size.y);
    }

    const Fonts fonts = load_fonts(system_fonts);
    Hand::fonts = &fonts;
    AppConfig cfg{.title = "hand", .size = size, .theme = hand::theme_of(hand::Appearance{}), .fonts = fonts.stack};

    switch (mode) {
        case Mode::window:
            return mayag::run<Hand>(std::move(cfg));
        case Mode::headless:
            return mayag::run_headless<Hand>(std::move(cfg), [](auto& rt) { return script(rt); });
        case Mode::png: {
            const std::string shot = std::getenv("HAND_SHOT") ? std::getenv("HAND_SHOT") : "";
            const float dpi = 2.0f;
            return mayag::run_headless<Hand>(std::move(cfg), [&](auto& rt) {
                using App = hand::App;
                using E = mayag::Edit<App::Model>;
                rt.window().resize(size, dpi);
                rt.tick();
                if (shot == "palette") { rt.send(App::Msg{App::TogglePalette{}}); rt.type("sp"); }
                if (shot == "settings") rt.send(App::Msg{App::ToggleSettings{}});
                if (shot == "type") {
                    rt.send(App::Msg{App::ToggleSettings{}});
                    rt.send(App::Msg{E{[](App::Model& m) { m.section = hand::settings::Section::type; }}});
                }
                if (shot == "glass") rt.send(App::Msg{E{[](App::Model& m) { m.look.chrome = hand::Chrome::glass; m.look.scheme = hand::SchemeId::dusk; m.look.roundness = 1.0f; }}});
                if (shot == "paper") rt.send(App::Msg{E{[](App::Model& m) { m.look.scheme = hand::SchemeId::paper; m.look.chrome = hand::Chrome::standard; m.grids.clear(); }}});
                if (shot == "phosphor") rt.send(App::Msg{E{[](App::Model& m) { m.look.scheme = hand::SchemeId::phosphor; m.grids.clear(); }}});
                rt.window().drive_frames(true);
                for (int i = 0; i < 90; ++i) rt.tick();
                rt.window().drive_frames(false);
                rt.tick();
                const auto px = rt.window().read_pixels();
                const int W = static_cast<int>(size.x * dpi);
                const int H = static_cast<int>(px.size() / 4 / static_cast<std::size_t>(W));
                if (!imageio::write_png(out, px, W, H)) return 1;
                std::printf("hand: wrote %s (%dx%d)\n", out.c_str(), W, H);
                return 0;
            });
        }
    }
    return 0;
}
