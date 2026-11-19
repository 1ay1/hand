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

    rt.press_key(Key::backslash, Mods{.shift = true, .ctrl = true});
    ok(M().tab().panes().size() == 4, "ctrl-shift-\\ splits");
    rt.press_key(Key::w, Mods{.shift = true, .ctrl = true});
    ok(M().tab().panes().size() == 3, "ctrl-shift-w closes");

    rt.press_key(Key::p, Mods{.shift = true, .ctrl = true});
    ok(M().palette_open, "ctrl-shift-p opens the picker");
    rt.type("scheme: glacier");
    rt.press_key(Key::enter);
    ok(M().look.scheme == hand::schemes_by_name::glacier && !M().palette_open, "enter runs the best match");

    rt.press_key(Key::comma, Mods{.ctrl = true});
    ok(M().settings_open, "ctrl-, opens appearance");
    rt.send(App::Msg{mayag::Edit<App::Model>{[](App::Model& m) { m.section = hand::settings::Section::terminal; }}});
    rt.click(dsl::node_id("cursor.blink"));
    ok(!M().look.cursor_blink, "the mayag switch edits cursor.blink inline");
    rt.click(dsl::part_id("cursor.style", 1));
    ok(M().look.cursor == hand::CursorStyle::beam, "the mayag segmented edits cursor.style inline");
    ok(hand::settings::modified_count(M().look) == 3, "three settings modified");
    const bool found = rt.click_text("reset all");
    ok(found, "the reset button is on screen");
    ok(hand::settings::modified_count(M().look) == 0, "reset restores defaults");
    rt.press_key(Key::escape);
    ok(!M().settings_open, "esc closes");

    // ── the appearance panel: presets ───────────────────────────────────────────────
    ok(hand::scheme_count >= 900, "over 900 presets compiled in");
    rt.press_key(Key::comma, Mods{.ctrl = true});
    rt.send(App::Msg{mayag::Edit<App::Model>{[](App::Model& m) { m.section = hand::settings::Section::presets; }}});
    rt.click(dsl::node_id("preset.search"));
    ok(M().preset_search_focused, "clicking the search field focuses it");
    rt.type("dracula");
    {
        const auto ms = hand::preset_matches(M().preset_query.text, M().preset_filter);
        ok(!ms.empty() && hand::scheme(ms.front()).name.find("Dracula") != std::string_view::npos,
           "typing filters the gallery (dracula)");
        const auto card = dsl::part_id("preset", ms.front().v);
        rt.window().move_to(rt.center_of(card));
        rt.tick();
        ok(M().preview_scheme == ms.front() && M().look.scheme != ms.front(), "hovering a preset previews it, without keeping it");
        rt.click(card);
        ok(M().look.scheme == ms.front(), "clicking a preset keeps it");
    }
    rt.press_key(Key::escape);   // leave the search field
    rt.click(dsl::part_id("preset.filter", 2));
    ok(M().preset_filter == 2, "the light filter");
    {
        const auto ms = hand::preset_matches("", 2);
        bool all_light = !ms.empty();
        for (auto id : ms) all_light = all_light && hand::scheme(id).mode == mayag::Mode::light;
        ok(all_light, "shows only light presets");
    }
    rt.click(dsl::part_id("appearance.nav", static_cast<std::size_t>(hand::settings::Section::tabs)));
    ok(M().section == hand::settings::Section::tabs, "the sidebar switches section");
    rt.click(dsl::part_id("tabs.style", 3));
    ok(M().look.tabs == hand::Tabs::pill, "tabs.style edits the tab shape");
    rt.click_text("reset section");
    ok(M().look.tabs == hand::Appearance{}.tabs, "reset section restores the tab defaults");
    rt.click_text("reset all");
    rt.press_key(Key::escape);
    ok(!M().settings_open && !M().preview_scheme, "closing leaves no preview behind");

    // ── the pointer's shape follows what is under it ─────────────────────
    const auto cursor_over = [&](Vec2 p) {
        rt.window().move_to(p);
        rt.tick();
        return static_cast<CursorShape>(rt.window().cursor());
    };
    const auto center = [&](std::uint64_t id) { return rt.center_of(id); };
    ok(cursor_over(center(dsl::part_id("tabs", 1))) == CursorShape::pointer, "pointer over a tab");
    ok(cursor_over(center(dsl::part_id("tabs", dsl::part_bar))) == CursorShape::grab, "grab over the empty strip");
    ok(cursor_over(center(dsl::node_id("pane.1.grid"))) == CursorShape::text, "I-beam over a terminal grid");
    ok(cursor_over(center(dsl::part_id("split.0", dsl::part_bar))) == CursorShape::resize_h,
       "resize arrows over a vertical divider");
    ok(cursor_over(center(dsl::node_id("win.close"))) == CursorShape::pointer, "pointer over a window button");
    ok(cursor_over(center(dsl::part_id("status", 102))) == CursorShape::arrow, "arrow over a plain status readout");
    ok(cursor_over(center(dsl::part_id("status", 100))) == CursorShape::pointer, "pointer over a clickable segment");

    // dragging a divider keeps its cursor even off the 5px strip
    const Vec2 bar = center(dsl::part_id("split.0", dsl::part_bar));
    rt.window().move_to(bar);
    rt.window().press(bar);
    rt.tick();
    rt.window().move_to(Vec2{bar.x + 60.0f, bar.y});
    rt.tick();
    ok(static_cast<CursorShape>(rt.window().cursor()) == CursorShape::resize_h, "a held divider keeps resize arrows");
    rt.window().release(Vec2{bar.x + 60.0f, bar.y});
    rt.tick();

    // over a modal, its own cursors; behind it, an arrow
    rt.press_key(Key::comma, Mods{.ctrl = true});
    ok(cursor_over(center(dsl::part_id("tabs", 1))) == CursorShape::arrow, "behind a modal: arrow, not a dead hand");
    rt.press_key(Key::escape);

    // window actions from the chrome
    const int moves = rt.window().moves();
    const Vec2 g = center(dsl::part_id("tabs", dsl::part_bar));
    rt.window().press(g);
    rt.window().move_to(Vec2{g.x + 30.0f, g.y + 10.0f});
    rt.tick();
    ok(rt.window().moves() == moves + 1, "dragging the strip asks the compositor to move the window");
    rt.window().release(Vec2{g.x + 30.0f, g.y + 10.0f});
    rt.tick();
    rt.click(dsl::node_id("win.max"));
    ok(rt.window().maximized(), "the maximise button maximises");

    // ── the terminal's folder, and the editor ────────────────────────────
    rt.click(dsl::node_id("win.max"));   // back to normal size
    rt.click(dsl::node_id("pane.1.grid"));
    rt.press_key(Key::e, Mods{.shift = true, .ctrl = true});
    ok(M().files_open && M().files_focused, "ctrl-shift-e opens the folder panel, focused");
    ok(!M().files.flat().empty(), "it lists the terminal's folder");
    ok(cursor_over(center(dsl::part_id("files", 0))) == CursorShape::pointer, "pointer over a file row");
    // walk to CMakeLists.txt with the keyboard, open it
    {
        const auto flat = M().files.flat();
        int target = -1;
        for (std::size_t i = 0; i < flat.size(); ++i) if (flat[i].rel == "CMakeLists.txt") target = static_cast<int>(i);
        ok(target >= 0, "CMakeLists.txt is in the panel");
        for (int i = M().files.selected; i < target; ++i) rt.press_key(Key::down);
        rt.press_key(Key::enter);
    }
    ok(M().editing(), "enter opens it in an editor pane beside the terminal");
    ok(M().tab().panes().size() == 4, "as a new split, the terminals untouched");
    rt.type("# hello\n");
    ok(M().pane(M().tab().focus)->doc->ed.dirty(), "typing edits the buffer");
    ok(M().pane(M().tab().focus)->doc->ed.text().starts_with("# hello\n"), "at the caret");
    rt.press_key(Key::z, Mods{.ctrl = true});
    rt.press_key(Key::z, Mods{.ctrl = true});
    ok(!M().pane(M().tab().focus)->doc->ed.dirty(), "ctrl-z undoes back to the saved text");
    rt.press_key(Key::d, Mods{.ctrl = true});
    rt.press_key(Key::d, Mods{.ctrl = true});
    ok(M().pane(M().tab().focus)->doc->ed.sels.size() >= 2, "ctrl-d adds cursors at the next occurrence");
    rt.press_key(Key::escape);
    ok(M().pane(M().tab().focus)->doc->ed.sels.size() == 1, "esc drops back to one cursor");
    ok(cursor_over(center(dsl::node_id("edit." + std::to_string(M().tab().focus.v)))) == CursorShape::text,
       "I-beam over the editor");
    rt.press_key(Key::f, Mods{.shift = true, .ctrl = true});
    ok(M().palette_open && M().pick == hand::App::Pick::files, "ctrl-shift-f finds files");
    rt.type("readme");
    rt.press_key(Key::enter);
    ok(M().editing() && M().pane(M().tab().focus)->title == "README.md", "the finder opens the best match");
    ok(M().tab().panes().size() == 4, "into the same editor pane (preview replaces)");

    // ── double clicks, the Wayland way (no platform count, real timing) ─────
    {
        const auto doc = [&]() -> const hand::Doc* { return M().pane(M().tab().focus)->doc ? &*M().pane(M().tab().focus)->doc : nullptr; };
        const std::string eid = "edit." + std::to_string(M().tab().focus.v);
        const Vec2 at = rt.center_of(dsl::node_id(eid));
        rt.window().timed_clicks(at, 2);
        rt.tick();
        ok(doc() && !doc()->ed.sels.front().empty() && doc()->ed.sels.front().head.line == doc()->ed.sels.front().anchor.line,
           "double click in the editor selects a word");
        rt.window().advance(1.0);
        rt.window().timed_clicks(at, 3);
        rt.tick();
        ok(doc() && doc()->ed.sels.front().end().line == doc()->ed.sels.front().start().line + 1,
           "triple click selects the line");
        rt.window().advance(1.0);
        rt.window().timed_clicks(at, 1);
        rt.tick();
        ok(doc() && doc()->ed.sels.front().empty(), "a later single click is a caret again (the sequence ended)");
        rt.window().advance(1.0);
        rt.window().timed_clicks(at, 2, 0.8);
        rt.tick();
        ok(doc() && doc()->ed.sels.front().empty(), "two clicks 0.8 s apart are two singles");
        rt.window().advance(1.0);
        rt.window().timed_clicks(at, 1, 0.1, Mods{.alt = true});
        rt.window().timed_clicks(Vec2{at.x, at.y + 40.0f}, 1, 0.1, Mods{.alt = true});
        rt.tick();
        ok(doc() && doc()->ed.sels.size() >= 2, "alt-click adds carets");
        rt.press_key(Key::escape);

        // a folder in the panel: one double click toggles it ONCE
        const auto flat = M().files.flat();
        std::size_t dir = 0;
        while (dir < flat.size() && !flat[dir].row.dir) ++dir;
        if (dir < flat.size()) {
            const bool was = M().files.open.contains(flat[dir].rel);
            rt.window().advance(1.0);
            rt.window().timed_clicks(rt.center_of(dsl::part_id("files", dir)), 2);
            rt.tick();
            ok(M().files.open.contains(flat[dir].rel) != was, "double clicking a folder toggles it once, not twice");
        }
        // a file: single previews (focus stays in the tree), double opens
        std::size_t file = 0;
        const auto flat2 = M().files.flat();
        while (file < flat2.size() && flat2[file].row.dir) ++file;
        if (file < flat2.size()) {
            rt.window().advance(1.0);
            rt.window().timed_clicks(rt.center_of(dsl::part_id("files", file)), 1);
            rt.tick();
            ok(M().files_focused, "single click on a file previews it, the tree keeps focus");
            rt.window().advance(1.0);
            rt.window().timed_clicks(rt.center_of(dsl::part_id("files", file)), 2);
            rt.tick();
            ok(!M().files_focused && M().editing(), "double click opens it and focuses the editor");
        }
        // the split divider: double click evens it
        const auto divider = dsl::part_id("split.0", dsl::part_bar);
        if (rt.node_rect(divider)) {
            rt.window().advance(1.0);
            rt.window().timed_clicks(rt.center_of(divider), 2);
            rt.tick();
            const auto* s = std::get_if<hand::Split>(&M().tab().nodes[0]);
            ok(s && s->ratio == 0.5f, "double click on a divider evens the split");
        }
    }

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
    // hand draws its own title bar (the tab strip): no server decorations.
    AppConfig cfg{.title = "hand", .size = size, .decorated = false,
                  .theme = hand::theme_of(hand::Appearance{}), .fonts = fonts.stack};

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
                if (shot == "glass") rt.send(App::Msg{E{[](App::Model& m) { m.look.chrome = hand::Chrome::glass; m.look.scheme = hand::schemes_by_name::dusk; m.look.roundness = 1.0f; }}});
                if (shot == "paper") rt.send(App::Msg{E{[](App::Model& m) { m.look.scheme = hand::schemes_by_name::paper; m.look.chrome = hand::Chrome::standard; m.grids.clear(); }}});
                if (shot == "phosphor") rt.send(App::Msg{E{[](App::Model& m) { m.look.scheme = hand::schemes_by_name::phosphor; m.grids.clear(); }}});
                for (std::size_t k = 0; k < static_cast<std::size_t>(hand::Tabs::count_); ++k)
                    if (shot == "tabs-" + std::string{hand::labels<hand::Tabs>[k]})
                        rt.send(App::Msg{E{[k](App::Model& m) { m.look.tabs = static_cast<hand::Tabs>(k); m.look.nerd_icons = true; }}});
                if (shot == "presets" || shot == "presets-search") {
                    rt.send(App::Msg{App::ToggleSettings{}});
                    rt.send(App::Msg{E{[](App::Model& m) { m.section = hand::settings::Section::presets; }}});
                    if (shot == "presets-search") rt.send(App::Msg{E{[](App::Model& m) { m.preset_query.set("gruvbox"); }}});
                }
                if (shot == "chrome") {
                    rt.send(App::Msg{App::ToggleSettings{}});
                    rt.send(App::Msg{E{[](App::Model& m) { m.section = hand::settings::Section::chrome; }}});
                }
                if (shot == "catppuccin") {
                    rt.send(App::Msg{E{[](App::Model& m) {
                        if (auto id = hand::find_scheme("Catppuccin Mocha")) m.look.scheme = *id;
                        m.look.tabs = hand::Tabs::pill; m.look.roundness = 1.0f; m.look.chrome = hand::Chrome::standard;
                    }}});
                }
                if (shot == "nerd" || shot == "nerd-glacier") {
                    if (shot == "nerd-glacier") rt.send(App::Msg{E{[](App::Model& m) { m.look.scheme = hand::schemes_by_name::glacier; }}});
                    rt.send(App::Msg{hand::on_window::ToggleFiles{}});
                    rt.send(App::Msg{E{[](App::Model& m) {
                        m.files.toggle("include");
                        m.files.toggle("include/hand");
                        m.files.toggle("include/hand/core");
                        m.open_file("include/hand/core/theme.hpp", true);
                        if (hand::Doc* d = m.focused_doc()) { d->ed.set_cursor({51, 12}); d->ed.reveal(); }
                        m.files.selected = 6;
                    }}});
                }
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
