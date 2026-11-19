// hand — a terminal, like foot: one window, one shell. toe is the VT engine.
//
//   hand                     open a window running $SHELL
//   hand -e CMD ...          run CMD instead of the shell
//   hand --png out.png       render one frame of a canned screen (no pty)
//   hand --headless          scripted self-test (no pty), exit code = failures

#include <hand/app.hpp>
#include <hand/shell.hpp>

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
#include <unistd.h>

namespace {

using namespace mayag;

constexpr std::array<std::string_view, 8> kMono{
    "JetBrainsMono Nerd Font", "JetBrains Mono", "Iosevka", "Berkeley Mono", "Geist Mono",
    "CaskaydiaCove Nerd Font Mono", "Fira Code", "DejaVu Sans Mono"};

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
        std::array<std::string_view, 1> lead{kMono.back()};
        for (auto cand : kMono)
            if (typo::system::Database::instance().first_available(std::array{cand})) { lead[0] = cand; break; }
        s = typo::system::default_stack(typo::FontConfig{.mode = typo::RenderMode::hybrid, .atlas_size = 2048}, 400,
                                        false, lead);
        (void)typo::system::attach_family(*s, FontFamily::mono, kMono);
        for (auto cand : kMono) {
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
    enum class Mode { window, png, headless } mode = Mode::window;
    std::string out = "hand.png";
    Vec2 size{960, 600};
    bool system_fonts = true;
    hand::Config cfg;
    std::string shot;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--repro-resize") {
            // Real fish (your config + hand's integration) through a drag of resizes.
            // Prints the screen after each step. Bounded: every read is ≤ 50 ms.
            hand::Pty pty;
            const auto si = hand::shell_integration("/usr/bin/fish");
            int cols = 80, rows = 24;
            hand::Term t{cols, rows, hand::palette_of(hand::Config{})};
            if (!pty.spawn(cols, rows, "/usr/bin/fish", si.env, si.args)) return 1;
            const bool show_bytes = false;
            const auto pump = [&](int ms) {
                std::string buf;
                for (int k = 0; k < ms / 50 && hand::pty_read(pty.fd(), buf, 50); ++k) {
                    if (show_bytes && !buf.empty()) {
                        std::string e;
                        for (unsigned char ch : buf) {
                            if (ch == 0x1b) e += "\\e"; else if (ch == '\r') e += "\\r"; else if (ch == '\n') e += "\\n\n      ";
                            else if (ch == 7) e += "\\a"; else if (ch < 0x20) { char b[8]; std::snprintf(b, 8, "^%c", ch + 64); e += b; }
                            else e += static_cast<char>(ch);
                        }
                        std::printf("  bytes: %s\n", e.c_str());
                    }
                    const auto fx = t.feed(buf);
                    buf.clear();
                    if (!fx.reply.empty()) pty.write(fx.reply);
                }
            };
            const auto dump = [&](const char* what) {
                std::printf("\n=== %s  (%dx%d, cursor %d,%d)\n", what, t.cols(), t.rows(), t.cursor().col, t.cursor().row);
                const auto g = t.snapshot();
                for (int r = 0; r < g.rows; ++r) {
                    std::string line;
                    for (const auto& c : g.lines[static_cast<std::size_t>(r)].cells) line += c.text.empty() ? "" : c.text;
                    while (!line.empty() && line.back() == ' ') line.pop_back();
                    const char mk = g.lines[static_cast<std::size_t>(r)].mark == mayag::dsl::TermMark::none ? ' ' : '#';
                    std::printf("%c%2d|%s\n", mk, r, line.c_str());
                }
            };
            const auto resize = [&](int c, int r) {
                t.resize(c, r);
                pty.resize(c, r);
                pump(400);
            };
            pump(1500);
            pty.write("seq 1 12\r");
            pump(800);
            dump("start");
            const int steps[][2] = {{80, 18}, {80, 12}, {60, 12}, {50, 10}, {70, 20}, {90, 30}, {80, 24}};
            for (const auto& st : steps) {
                char what[32];
                std::snprintf(what, sizeof what, "-> %dx%d", st[0], st[1]);
                resize(st[0], st[1]);
                if (&st == &steps[3] || &st == &steps[6]) dump(what);
            }
            // a real drag: many sizes in a burst, the shell answering whenever it can
            for (int k = 0; k < 40; ++k) {
                const int c = 30 + (k * 37) % 70, r = 6 + (k * 13) % 26;
                t.resize(c, r);
                pty.resize(c, r);
                if (k % 3 == 0) pump(50);
            }
            resize(80, 24);
            pump(600);
            dump("after a 40-step drag burst (down to 30 cols: the prompt wraps)");
            // the verdict: every number once, in order; exactly one live prompt line
            const auto g = t.snapshot();
            std::vector<std::string> lines;
            for (const auto& l : g.lines) {
                std::string line;
                for (const auto& c : l.cells) line += c.text;
                while (!line.empty() && line.back() == ' ') line.pop_back();
                lines.push_back(line);
            }
            int prompts = 0, blank_run = 0, worst_gap = 0;
            for (const auto& l : lines) {
                prompts += l.rfind("\u276F", 0) == 0 && l.size() <= 4 ? 1 : 0;
                blank_run = l.empty() ? blank_run + 1 : 0;
                worst_gap = std::max(worst_gap, blank_run);
            }
            std::printf("\nverdict: %d empty prompt line(s) on screen, longest blank run %d\n", prompts, worst_gap);
            pty.write("exit\r");
            pump(300);
            return prompts == 1 && worst_gap <= 2 ? 0 : 1;
        }
    }
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--png") { mode = Mode::png; if (i + 1 < argc && argv[i + 1][0] != '-') out = argv[++i]; }
        else if (a == "--headless") mode = Mode::headless;
        else if (a == "--last-resort") system_fonts = false;
        else if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%fx%f", &size.x, &size.y);
        else if (a == "--font-size" && i + 1 < argc) cfg.font_size = std::strtof(argv[++i], nullptr);
        else if (a == "-e" && i + 1 < argc) cfg.shell_cli = argv[++i];
        else if (a == "--shot" && i + 1 < argc) shot = argv[++i];
    }
    g_stack = load_fonts(system_fonts);
    if (mode == Mode::window) {
        // the user's hand.vibe; scripted runs use defaults so they are reproducible
        const std::string problems = hand::config_load(cfg, hand::config_path());
        if (!problems.empty()) std::fprintf(stderr, "hand: %s:\n%s", hand::config_path().c_str(), problems.c_str());
    }
    cfg.demo = mode != Mode::window;
    hand::App::config = cfg;

    static typo::StackMeasurer measurer{*g_stack};
    hand::App::cell_width = [](float s, int family) {
        TextStyle st; st.size = s;
        st.family = hand::font_names().empty() ? FontFamily::mono
                  : custom_family(static_cast<std::size_t>(std::clamp(family, 0, static_cast<int>(hand::font_names().size()) - 1)));
        const float w = measurer.advance("MMMMMMMMMM", st) / 10.0f;
        return w > 0.0f ? w : s * 0.6f;
    };

    AppConfig ac{.title = "hand", .size = size, .transparent = true, .theme_morph = false, .fonts = g_stack, .log_renderer = false};
    switch (mode) {
        case Mode::window: return mayag::run<hand::App>(std::move(ac));
        case Mode::headless:
        case Mode::png:
            return mayag::run_headless<hand::App>(std::move(ac), [&](auto& rt) {
                rt.send(hand::App::Msg{hand::App::Output{std::string{kDemo}}});
                rt.tick();
                if (mode == Mode::headless) {
                    int fails = 0;
                    const auto ok = [&](bool c, const char* what) { std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what); fails += !c; };
                    const auto& m = rt.model();
                    ok(m.term->cols() > 20 && m.term->rows() > 5, "the grid fits the window");
                    const auto g = m.term->snapshot();
                    ok(g.lines[0].cells[0].text == "~" && !g.lines[0].cells[0].default_fg, "toe parsed colour output");
                    ok(g.cursor_row == 6, "the cursor follows the output");
                    rt.window().resize(Vec2{size.x / 2.0f, size.y / 2.0f});
                    rt.tick();
                    ok(rt.model().term->cols() < g.cols, "a resize reflows the grid");
                    {
                        // a real child on a real pty, through toe (bounded: ≤ 2 s)
                        hand::Pty pty;
                        hand::Term t{40, 5, hand::palette_of(hand::Config{})};
                        const bool up = pty.spawn(40, 5, "/bin/sh");
                        ok(up, "forkpty starts a shell");
                        if (up) {
                            pty.write("printf '\\033[31mred\\033[0m'; exit 7\n");
                            std::string buf;
                            for (int i = 0; i < 40 && hand::pty_read(pty.fd(), buf, 50); ++i) {}
                            (void)t.feed(buf);
                            bool red = false;
                            const auto sg = t.snapshot();
                            for (const auto& l : sg.lines)
                                for (const auto& c : l.cells) red = red || (c.text == "r" && c.fg == 0xE06C75);
                            ok(red, "shell output reaches the grid in the palette's red");
                            int code = -1;
                            for (int i = 0; i < 40 && code < 0; ++i) { code = hand::pty_reap(pty.pid()); if (code < 0) ::usleep(25000); }
                            ok(code == 7, "the exit code comes back");
                        }
                    }
                    {
                        // ── the settings panel ──────────────────────────────────────────
                        using K = mayag::Key;
                        rt.window().resize(size);
                        rt.tick();
                        // ── links + search first, on a known screen ──────────────────
                        rt.send(hand::App::Msg{hand::App::Output{std::string{
                            "\x1b[2J\x1b[Hsee https://example.com/docs ok\r\n"
                            "\x1b]8;;https://toe.dev\x1b\\osc8 link\x1b]8;;\x1b\\ and needle, needle\r\n"}}});
                        rt.tick();
                        ok(rt.model().term->link_at(8, 0) == "https://example.com/docs", "a bare URL is detected under the pointer");
                        ok(rt.model().term->link_at(1, 1) == "https://toe.dev", "an OSC 8 hyperlink resolves to its target");
                        ok(rt.model().term->link_at(0, 0).empty(), "plain text is not a link");
                        rt.press_key(K::f, Mods{.shift = true, .ctrl = true});
                        ok(rt.model().search_open, "ctrl+shift+f opens search");
                        rt.type("needle");
                        ok(rt.model().term->search_count() == 2, "typing finds every match in the scrollback");
                        rt.tick();
                        ok(rt.model().grid && rt.model().grid->matches.size() == 2 && rt.model().grid->current_match >= 0,
                           "matches are highlighted, the current one marked");
                        const auto cur = rt.model().term->search_current();
                        rt.press_key(K::enter);
                        ok(rt.model().term->search_current() != cur, "enter steps to the next match");
                        rt.press_key(K::escape);
                        ok(!rt.model().search_open && rt.model().term->search_count() == 0, "esc closes and clears it");
                        rt.press_key(K::comma, Mods{.ctrl = true});
                        ok(rt.model().settings_open, "ctrl+, opens settings");
                        ok(rt.node_rect(dsl::node_id("settings.rows")).has_value(), "the mayag settings_panel is on screen");
                        rt.type("j");   // vi keys: font.family -> font.size
                        ok(rt.model().panel.selected == 1, "j selects the next setting");
                        const float before = rt.model().font_size;
                        const int cols = rt.model().term->cols();
                        rt.press_key(K::right, Mods{.shift = true});
                        ok(rt.model().cfg.font_size == before + 5.0f && rt.model().font_size == before + 5.0f,
                           "shift+right steps font.size ×10, live");
                        ok(rt.model().term->cols() < cols, "and the grid refits to the bigger cells");
                        rt.type("r");
                        ok(rt.model().cfg.font_size == hand::Config{}.font_size, "r resets it");
                        rt.type("/");
                        rt.type("scheme");
                        rt.press_key(K::enter);
                        ok(rt.model().panel.query == "scheme" && !rt.model().panel.filtering, "/ filters, enter keeps the filter");
                        const auto bg0 = rt.model().palette.bg;
                        const int scheme0 = rt.model().cfg.scheme;
                        rt.press_key(K::enter);   // the scheme row: open its select
                        rt.tick();
                        ok(rt.model().panel.menu >= 0, "enter opens the scheme dropdown");
                        ok(rt.node_rect(dsl::part_id("settings", dsl::kSetMenuPart)).has_value() ||
                           rt.node_rect(dsl::node_id("settings.colors.scheme.select")).has_value(), "the menu is on screen");
                        rt.press_key(K::down);
                        ok(rt.model().palette.bg != bg0, "↓ in the menu previews the next scheme live");
                        rt.press_key(K::escape);
                        ok(rt.model().panel.menu < 0 && rt.model().palette.bg == bg0 && rt.model().cfg.scheme == scheme0,
                           "esc closes the menu and reverts the preview");
                        rt.press_key(K::enter);
                        rt.type("dracula");
                        rt.press_key(K::enter);
                        ok(rt.model().palette.bg == 0x282A36, "typing filters 913 schemes, enter keeps one");
                        rt.press_key(K::right);
                        ok(rt.model().palette.bg != 0x282A36, "→ steps to the next scheme without the menu");
                        rt.press_key(K::escape);   // clears the filter
                        rt.press_key(K::escape);   // closes
                        ok(!rt.model().settings_open, "esc clears the filter, then closes");
                        {
                            // ── the dropdown with the mouse ────────────────────────────────────
                            rt.press_key(K::comma, Mods{.ctrl = true});
                            rt.tick();
                            const auto trig = dsl::node_id("settings.colors.scheme.select");
                            rt.click(trig);
                            ok(rt.model().panel.menu >= 0, "clicking the dropdown opens it");
                            rt.click(trig);
                            ok(rt.model().panel.menu < 0, "clicking it again closes it");
                            rt.click(trig);
                            rt.click(dsl::node_id("settings.search"));
                            ok(rt.model().panel.menu < 0 && rt.model().settings_open,
                               "clicking elsewhere in the panel closes it (and the panel stays)");
                            rt.click(trig);
                            ok(rt.model().panel.menu >= 0, "and it opens again after that");
                            rt.press_key(K::escape);
                            rt.press_key(K::escape);
                            if (rt.model().settings_open) rt.press_key(K::escape);
                        }
                    }
                    {
                        // ── EVERY setting: step it through the panel, check it took effect ──
                        using K = mayag::Key;
                        using SE = dsl::SettingEdit;
                        const auto specs = hand::setting_specs(hand::Config{});
                        int changed = 0, total = 0;
                        std::string stuck;
                        for (std::size_t i = 0; i < specs.size(); ++i) {
                            const auto& s = specs[i];
                            if (s.kind == dsl::SettingSpec::Kind::cycle && s.options.size() < 2) continue;   // one font
                            ++total;
                            const hand::Config before = rt.model().cfg;
                            const bool at_top = s.kind == dsl::SettingSpec::Kind::number && s.value >= s.hi;
                            const int delta = (s.kind == dsl::SettingSpec::Kind::choice || s.kind == dsl::SettingSpec::Kind::cycle) ? 1 : at_top ? -3 : 3;
                            rt.send(hand::App::Msg{hand::App::Panel{dsl::SettingsOp{[i, delta](dsl::SettingsPanelState& x) {
                                x.out.push_back({SE::Op::step, i, 0.0f, delta});
                            }}}});
                            rt.tick();
                            const auto after = hand::setting_specs(rt.model().cfg)[i];
                            if (after.modified && after.value != hand::setting_specs(before)[i].value) ++changed;
                            else stuck += " " + s.section + "." + s.key;
                        }
                        if (!stuck.empty()) std::printf("    did not change:%s\n", stuck.c_str());
                        ok(changed == total, "every setting changes through the panel and reads back as modified");
                        const auto& mm = rt.model();
                        const hand::Config d{};
                        ok(mm.font_size == mm.cfg.font_size && mm.font_size != d.font_size, "font.size reaches the grid");
                        ok(mm.metrics.cell_h == std::round(mm.font_size * mm.cfg.line_height), "font.line_height sets the row height");
                        ok(mm.metrics.origin_x == mm.cfg.padding + (mm.cfg.shell_marks ? 10.0f : 0.0f) && mm.cfg.padding != d.padding, "window.padding moves the grid");
                        ok(mm.palette.bg != hand::palette_of(d).bg, "colors.scheme recolours the terminal");
                        ok(mm.grid && mm.grid->cursor_shape != dsl::TermCursorShape::block, "cursor.style reaches the cursor");
                        ok(!mm.cfg.cursor_blink && !mm.cfg.visual_bell && mm.cfg.copy_on_select && !mm.cfg.links_need_ctrl,
                           "the toggles flip");
                        // reset all: back to every default
                        rt.send(hand::App::Msg{hand::App::Panel{dsl::SettingsOp{[](dsl::SettingsPanelState& x) {
                            x.out.push_back({SE::Op::reset_all, 0, 0.0f, 0});
                        }}}});
                        rt.tick();
                        std::size_t mods = 0;
                        for (const auto& s : hand::setting_specs(rt.model().cfg)) mods += s.modified;
                        ok(mods == 0 && rt.model().font_size == d.font_size && rt.model().palette.bg == hand::palette_of(d).bg,
                           "reset all restores every default, live");
                        (void)K::escape;
                    }
                    {
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
                        rt.send(hand::App::Msg{E{[](hand::App::Model& m) { m.cfg.visual_bell = false; m.bell_at = -1.0; }}});
                        rt.send(hand::App::Msg{hand::App::Output{std::string{"\a"}}});
                        const auto quiet = px_at(bx, by);
                        ok(quiet == lit, "and stays dark with it off");
                        rt.send(hand::App::Msg{E{[](hand::App::Model& m) { m.cfg.visual_bell = true; }}});
                    }
                    {
                        // ── the cool stuff ───────────────────────────────────────────────────
                        using K = mayag::Key;
                        using E = mayag::Edit<hand::App::Model>;
                        const auto feed = [&](std::string s) { rt.send(hand::App::Msg{hand::App::Output{std::move(s)}}); rt.tick(); };
                        feed("\x1b[2J\x1b[H");
                        // two commands through OSC 133, as the injected script emits them
                        feed("\x1b]133;A\a\x1b[32m\u276F\x1b[0m \x1b]133;B\amake\r\n\x1b]133;C\abuilding...\r\nerror: oops\r\n\x1b]133;D;2\a");
                        feed("\x1b]133;A\a\u276F \x1b]133;B\agit log -1\r\n\x1b]133;C\acommit 3f9a2c1d see https://toe.dev/x and ~/src/hand/main.cpp\r\n\x1b]133;D;0\a");
                        feed("\x1b]133;A\a\u276F \x1b]133;B\a");
                        const auto cmds = rt.model().term->commands();
                        int ran = 0, failed = 0;
                        for (const auto& c : cmds) { ran += c.ran; failed += c.finished && c.exit_code != 0; }
                        ok(ran == 2 && failed == 1, "OSC 133 commands are recorded, with exit codes");
                        ok(rt.model().term->command_text(cmds[0]) == "make", "the typed command line is recovered");
                        const auto sg = rt.model().term->snapshot();
                        ok(sg.lines[0].mark == dsl::TermMark::error && sg.lines[3].mark == dsl::TermMark::ok,
                           "failed and succeeded prompts get their marks");
                        rt.press_key(K::g, Mods{.shift = true, .ctrl = true});
                        ok(rt.window().get_clipboard().find("commit 3f9a2c1d") != std::string::npos, "ctrl+shift+g copies the last output");
                        // hints
                        rt.press_key(K::space, Mods{.shift = true, .ctrl = true});
                        bool url = false, path = false, hash = false;
                        for (const auto& h : rt.model().hints) {
                            url = url || h.text == "https://toe.dev/x";
                            path = path || h.text == "~/src/hand/main.cpp";
                            hash = hash || h.text == "3f9a2c1d";
                        }
                        ok(rt.model().hinting && url && path && hash, "ctrl+shift+space labels the URL, the path and the hash");
                        std::size_t hi = 0;
                        while (hi < rt.model().hints.size() && rt.model().hints[hi].text != "3f9a2c1d") ++hi;
                        const std::string label = hi < rt.model().hint_labels.size() ? rt.model().hint_labels[hi] : "?";
                        rt.type(label);
                        ok(!rt.model().hinting && rt.window().get_clipboard() == "3f9a2c1d", "typing its label copies it");
                        // smooth cursor
                        rt.send(hand::App::Msg{E{[](hand::App::Model& m) { m.cfg.cursor_smooth = true; m.cur_known = true; m.cur_x = 0; m.cur_y = 0; }}});
                        rt.window().drive_frames(true);   // the frame clock runs, as on screen
                        feed("\x1b[10;30H");
                        const float mid = rt.model().cur_x;
                        for (int i = 0; i < 40; ++i) rt.tick();
                        rt.window().drive_frames(false);
                        ok(mid < 29.0f && std::fabs(rt.model().cur_x - 29.0f) < 0.05f && std::fabs(rt.model().cur_y - 9.0f) < 0.05f,
                           "the cursor glides to its cell and settles");
                        // minimap + prompt jumps
                        std::string many;
                        for (int i = 0; i < 200; ++i) many += "\x1b]133;A\a$ \x1b]133;B\aecho " + std::to_string(i) + "\r\n\x1b]133;C\a" + std::to_string(i) + "\r\n\x1b]133;D;0\a";
                        feed(many);
                        ok(rt.node_rect(dsl::node_id("minimap")).has_value(), "the minimap is on screen");
                        rt.window().drive_frames(true);
                        const auto settle = [&] { for (int i = 0; i < 60; ++i) rt.tick(); };
                        rt.press_key(K::z, Mods{.shift = true, .ctrl = true});
                        rt.press_key(K::z, Mods{.shift = true, .ctrl = true});
                        rt.tick();
                        const float smid = rt.model().scroll_d;
                        settle();
                        ok(rt.model().term->scroll_offset() > 0 && rt.model().scroll_d == rt.model().scroll_goal,
                           "ctrl+shift+z glides back to earlier prompts");
                        ok(smid > 0.0f && smid < rt.model().scroll_goal, "...through every pixel in between");
                        ok(rt.node_rect(dsl::node_id("tolive")).has_value(), "scrolled back, a pill offers the way home");
                        ok(!rt.node_rect(dsl::node_id("sticky")).has_value(), "on a prompt, no header (the prompt is right there)");
                        rt.press_key(K::down, Mods{.shift = true, .ctrl = true});   // one line into its output
                        settle();
                        ok(rt.model().sticky_t == 1.0f && rt.node_rect(dsl::node_id("sticky")).has_value(),
                           "the command you are inside slides in as a sticky header");
                        {
                            const float before = rt.model().scroll_goal;
                            rt.click(dsl::node_id("sticky"));
                            settle();
                            ok(rt.model().scroll_goal != before, "clicking the header flies to that command's prompt");
                        }
                        {
                            const auto g2 = rt.model().grid;   // mid-glide frames carry the row above
                            rt.press_key(K::z, Mods{.shift = true, .ctrl = true});
                            rt.tick();
                            ok(rt.model().scroll_d != std::floor(rt.model().scroll_d) && rt.model().grid->rows == rt.model().term->rows() + 1,
                               "mid-glide the grid is a row taller and slides by sub-row amounts");
                            (void)g2;
                            settle();
                        }
                        rt.send(hand::App::Msg{hand::App::Output{std::string{"\r\nnew 1\r\nnew 2\r\nnew 3"}}});
                        rt.tick();
                        ok(rt.model().new_below >= 3, "output arriving while you read is counted, not shoved at you");
                        rt.click(dsl::node_id("tolive"));
                        settle();
                        ok(rt.model().term->scroll_offset() == 0 && rt.model().new_below == 0, "the pill glides you back to live");
                        for (int i = 0; i < 3; ++i) rt.press_key(K::z, Mods{.shift = true, .ctrl = true});
                        settle();
                        for (int i = 0; i < 5; ++i) rt.press_key(K::x, Mods{.shift = true, .ctrl = true});
                        settle();
                        ok(rt.model().term->scroll_offset() == 0, "ctrl+shift+x walks forward to the live prompt");
                        // the live tail: scrolled back, the last live rows slide in at the bottom
                        rt.press_key(K::z, Mods{.shift = true, .ctrl = true});
                        rt.press_key(K::z, Mods{.shift = true, .ctrl = true});
                        rt.press_key(K::z, Mods{.shift = true, .ctrl = true});
                        settle();
                        ok(rt.model().tail_t == 1.0f && rt.node_rect(dsl::node_id("tail")).has_value(),
                           "scrolled back, the live tail slides in at the bottom");
                        rt.click(dsl::node_id("tail"));
                        settle();
                        ok(rt.model().term->scroll_offset() == 0 && rt.model().tail_t == 0.0f, "clicking it glides home and it slides away");
                        // the peek: hover the minimap over an old command
                        rt.window().move_to(Vec2{size.x - 6.0f, size.y * 0.2f});
                        settle();
                        ok(rt.model().mm_hover_y > 0.0f && rt.model().peek_t == 1.0f, "hovering the minimap brings up the peek");
                        ok(rt.node_rect(dsl::node_id("peek.grid")).has_value(), "with the lines from that part of history");
                        rt.window().move_to(Vec2{size.x * 0.5f, size.y * 0.5f});
                        settle();
                        ok(rt.model().peek_t == 0.0f, "and it fades when the pointer leaves");
                        {
                            // scrolling back WHILE output streams (a build, `yes`, tail -f)
                            int line = 0;
                            const auto stream = [&](int n) {
                                for (int k = 0; k < n; ++k) {
                                    std::string chunk;
                                    for (int j = 0; j < 3; ++j) chunk += "stream " + std::to_string(line++) + "\r\n";
                                    rt.send(hand::App::Msg{hand::App::Output{std::move(chunk)}});
                                    rt.tick();
                                }
                            };
                            stream(10);
                            for (int k = 0; k < 30; ++k) {   // a trackpad: small deltas, output between every one
                                rt.window().scroll(Vec2{size.x * 0.5f, size.y * 0.5f}, Vec2{0.0f, 3.0f});
                                stream(1);
                            }
                            stream(20);
                            const int off = rt.model().term->scroll_offset();
                            ok(off >= 5, "the wheel scrolls back while output keeps streaming");
                            const auto top = [&] {
                                std::string s;
                                const auto g = rt.model().term->snapshot();
                                for (const auto& c : g.lines[0].cells) s += c.text;
                                return s;
                            };
                            const std::string a = top();
                            stream(20);
                            ok(top() == a, "and what you are reading stays still as more arrives");
                            ok(rt.model().new_below > 0, "...counted as new below");
                            // drag-select past the top edge: the view scrolls with the selection
                            const int off2 = rt.model().term->scroll_offset();
                            rt.window().press(Vec2{60.0f, size.y * 0.5f});
                            rt.tick();
                            rt.window().move_to(Vec2{80.0f, 2.0f});
                            for (int k = 0; k < 40; ++k) rt.tick();
                            rt.window().release(Vec2{80.0f, 2.0f});
                            rt.tick();
                            ok(rt.model().term->scroll_offset() > off2 && rt.model().term->has_selection(),
                               "dragging a selection past the top edge scrolls history with it");
                            rt.press_key(K::up, Mods{.shift = true, .ctrl = true});
                            settle();
                            rt.press_key(K::end, Mods{.shift = true});
                            settle();
                            ok(rt.model().term->scroll_offset() == 0, "shift+end lands back on the live view");
                        }
                        {
                            // a full-screen app (alternate screen): no prompt marks, no minimap
                            rt.send(hand::App::Msg{hand::App::Output{std::string{"\x1b[?1049h\x1b[2J\x1b[H"}}});
                            for (int r = 0; r < 20; ++r)
                                rt.send(hand::App::Msg{hand::App::Output{"\x1b[" + std::to_string(r + 1) + ";1Hmatrix " + std::to_string(r)}});
                            rt.tick();
                            int marks = 0;
                            for (const auto& l : rt.model().grid->lines) marks += l.mark != dsl::TermMark::none ? 1 : 0;
                            ok(marks == 0, "a full-screen app (cmatrix, vim) gets no prompt marks");
                            ok(!rt.node_rect(dsl::node_id("minimap")).has_value(), "...and no minimap");
                            rt.send(hand::App::Msg{hand::App::Output{std::string{"\x1b[?1049l"}}});
                            rt.tick();
                            ok(rt.node_rect(dsl::node_id("minimap")).has_value(), "back at the shell, both return");
                        }
                        rt.window().drive_frames(false);
                    }
                    {
                        // ── the injected script, in a REAL fish (bounded) ───────────────────
                        if (::access("/usr/bin/fish", X_OK) == 0) {
                            hand::Pty pty;
                            const auto si = hand::shell_integration("/usr/bin/fish");
                            hand::Term t{80, 10, hand::palette_of(hand::Config{})};
                            const bool up = pty.spawn(80, 10, "/usr/bin/fish", si.env, si.args);
                            std::string buf;
                            for (int i = 0; i < 40 && hand::pty_read(pty.fd(), buf, 50); ++i) {
                                const auto fx = t.feed(buf); buf.clear();
                                if (!fx.reply.empty()) pty.write(fx.reply);
                                if (i == 10) pty.write("false\r");
                                if (i == 20) pty.write("exit\r");
                            }
                            (void)t.feed(buf);
                            int ran = 0, failed = 0;
                            for (const auto& c : t.commands()) { ran += c.ran; failed += c.finished && c.exit_code == 1; }
                            std::printf("    fish: %d command(s) recorded, %d failed\n", ran, failed);
                            ok(up && ran >= 1 && failed >= 1, "fish is integrated without touching its config");
                        }
                    }
                    {
                        // ── resize never corrupts: real fish, a drag of sizes (bounded) ───────
                        if (::access("/usr/bin/fish", X_OK) == 0) {
                            hand::Pty pty;
                            const auto si = hand::shell_integration("/usr/bin/fish");
                            hand::Term t{80, 24, hand::palette_of(hand::Config{})};
                            const bool up = pty.spawn(80, 24, "/usr/bin/fish", si.env, si.args);
                            const auto pump = [&](int ms) {
                                std::string buf;
                                for (int k = 0; k < ms / 50 && hand::pty_read(pty.fd(), buf, 50); ++k) {
                                    const auto fx = t.feed(buf); buf.clear();
                                    if (!fx.reply.empty()) pty.write(fx.reply);
                                }
                            };
                            pump(1200);
                            pty.write("clear; seq 1 12\r");   // fish redraws its prompt after; then wait for idle
                            pump(700);
                            const auto text = [&] {
                                std::vector<std::string> v;
                                for (const auto& l : t.snapshot().lines) {
                                    std::string s;
                                    for (const auto& c : l.cells) s += c.text;
                                    while (!s.empty() && s.back() == ' ') s.pop_back();
                                    v.push_back(s);
                                }
                                return v;
                            };
                            const auto before = text();
                            for (int k = 0; k < 30; ++k) {   // a drag: grow and shrink in both directions, stay readable (>= 50 cols)
                                const int c = 50 + (k * 37) % 50, r = 6 + (k * 13) % 26;
                                t.resize(c, r);
                                pty.resize(c, r);
                                if (k % 3 == 0) pump(50);
                            }
                            t.resize(80, 24);
                            pty.resize(80, 24);
                            pump(700);
                            const auto after = text();
                            // what counts: the 12 numbers once each, ONE prompt, and no blank gap
                            // between content (empty screen below the prompt is just screen)
                            const auto audit = [](const std::vector<std::string>& v, int& nums, int& prompts, int& gap) {
                                nums = prompts = gap = 0;
                                std::size_t last = 0;
                                for (std::size_t i = 0; i < v.size(); ++i) if (!v[i].empty()) last = i;
                                int run = 0;
                                for (std::size_t i = 0; i <= last && i < v.size(); ++i) {
                                    const auto& l = v[i];
                                    bool num = !l.empty();
                                    for (char ch : l) num = num && ch >= '0' && ch <= '9';
                                    nums += num ? 1 : 0;
                                    prompts += l.rfind("\u276F", 0) == 0 ? 1 : 0;
                                    run = l.empty() ? run + 1 : 0;
                                    gap = std::max(gap, run);
                                }
                            };
                            int seq_lines = 0, prompt_cmds = 0, worst = 0;
                            audit(after, seq_lines, prompt_cmds, worst);
                            if (seq_lines != 12 || prompt_cmds != 1 || worst > 2)
                                for (std::size_t i = 0; i < after.size(); ++i) std::printf("    %2zu|%s\n", i, after[i].c_str());
                            ok(up && seq_lines == 12 && prompt_cmds == 1, "a 30-step resize drag keeps every line, once");
                            ok(worst <= 2, "no blank gaps open up between output and prompt");
                            // narrower than the prompt: it wraps, then must come back whole
                            for (int c : {28, 22, 36, 80}) { t.resize(c, 24); pty.resize(c, 24); pump(250); }
                            const auto narrow = text();
                            int nseq = 0, nprompt = 0, ngap = 0;
                            audit(narrow, nseq, nprompt, ngap);
                            if (nseq != 12 || nprompt != 1 || ngap > 2)
                                for (std::size_t i = 0; i < narrow.size(); ++i) std::printf("    %2zu|%s\n", i, narrow[i].c_str());
                            ok(nseq == 12 && nprompt == 1 && ngap <= 2, "squeezed narrower than the prompt and back: output intact");
                            int same = 0;
                            for (std::size_t i = 0; i < std::min(before.size(), after.size()); ++i) same += before[i] == after[i] ? 1 : 0;
                            ok(same >= static_cast<int>(before.size()) - 3, "back at the same size, the screen reads the same");
                            int marks = 0;
                            for (const auto& c : t.commands()) marks += c.ran && c.prompt_row >= 0 ? 1 : 0;
                            ok(marks >= 1, "command marks follow the rewrap");
                            pty.write("exit\r");
                            pump(200);
                        }
                    }
                    {
                        // ── toe: a real space at a wrap point survives a rewrap ─────────────
                        hand::Term t{10, 5, hand::palette_of(hand::Config{})};
                        (void)t.feed("12345 789 abcdefg");   // wraps right after a space
                        t.resize(40, 5);
                        std::string row;
                        const auto snap = t.snapshot();
                        for (const auto& c : snap.lines[0].cells) row += c.text;
                        while (!row.empty() && row.back() == ' ') row.pop_back();
                        ok(row == "12345 789 abcdefg", "spaces at wrap points are text, not padding");
                    }
                    {
                        // ── hand.vibe: every row survives save + load ──────────────────────────
                        using SE = dsl::SettingEdit;
                        hand::Config a;
                        a.url_launcher = "firefox";
                        const auto n = hand::setting_specs(a).size();
                        for (std::size_t i = 0; i < n; ++i) (void)hand::setting_apply(a, {SE::Op::step, i, 0.0f, 2});
                        const std::string path = "/tmp/hand-test-" + std::to_string(::getpid()) + ".vibe";
                        const bool saved = hand::config_save(a, path);
                        hand::Config b;
                        const std::string problems = hand::config_load(b, path);
                        ::unlink(path.c_str());
                        const auto sa = hand::setting_specs(a), sb = hand::setting_specs(b);
                        std::string diff;
                        for (std::size_t i = 0; i < n; ++i)
                            if (std::fabs(sa[i].value - sb[i].value) > 1e-3f) diff += " " + sa[i].section + "." + sa[i].key;
                        if (!problems.empty()) std::printf("    load problems: %s", problems.c_str());
                        if (!diff.empty()) std::printf("    differs after reload:%s\n", diff.c_str());
                        ok(saved && problems.empty() && diff.empty() && b.url_launcher == "firefox",
                           "hand.vibe round-trips every setting");
                        // a broken or partial file is reported, never fatal
                        const std::string bad = path + ".bad";
                        { std::FILE* f = std::fopen(bad.c_str(), "w"); std::fputs("font { size 15.5  nope 3 }\ncolors { scheme \"Nord\" bold_is_bright yes }\n", f); std::fclose(f); }
                        hand::Config c;
                        const std::string p2 = hand::config_load(c, bad);
                        ::unlink(bad.c_str());
                        ok(c.font_size == 15.5f && hand::palette_of(c).bg == 0x2E3440 && p2.find("font.nope") != std::string::npos &&
                           p2.find("bold_is_bright") != std::string::npos,
                           "good keys apply; unknown keys and wrong types are reported");
                        { std::FILE* f = std::fopen(bad.c_str(), "w"); std::fputs("font { size ", f); std::fclose(f); }
                        hand::Config e;
                        const std::string p3 = hand::config_load(e, bad);
                        ::unlink(bad.c_str());
                        ok(!p3.empty() && e.font_size == hand::Config{}.font_size, "a syntax error keeps the defaults");
                    }
                    std::printf(fails ? "FAIL\n" : "PASS\n");
                    return fails;
                }
                rt.window().resize(size, 2.0f);
                if (shot == "cool" || shot == "hints") {
                    std::string s = "\x1b[2J\x1b[H";
                    const char* cmds[][3] = {{"cargo build --release", "   Compiling hand v0.1.0\r\n    Finished release in 41.2s", "0"},
                                             {"make test", "\x1b[31merror:\x1b[0m 2 tests failed (see build/log.txt)", "2"},
                                             {"git log --oneline -2", "\x1b[33m3f9a2c1d\x1b[0m terminal: shell integration\r\n\x1b[33m9b1e07aa\x1b[0m docs: https://github.com/1ay1/toe", "0"},
                                             {"ping -c1 192.168.1.1", "64 bytes from 192.168.1.1: icmp_seq=1 ttl=64 time=0.41 ms", "0"}};
                    for (auto& c : cmds)
                        s += std::string{"\x1b]133;A\a\x1b[1;34m~/src/hand\x1b[0m \x1b[35m\u276F\x1b[0m \x1b]133;B\a"} + c[0] + "\r\n\x1b]133;C\a" +
                             c[1] + "\r\n\x1b]133;D;" + c[2] + "\a";
                    s += "\x1b]133;A\a\x1b[1;34m~/src/hand\x1b[0m \x1b[35m\u276F\x1b[0m \x1b]133;B\a";
                    rt.send(hand::App::Msg{hand::App::Output{std::move(s)}});
                    rt.tick();
                    if (shot == "hints") rt.press_key(mayag::Key::space, mayag::Mods{.shift = true, .ctrl = true});
                    else rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) {
                        m.cur_known = true; m.cur_x = 3.0f; m.cur_y = 3.0f; }}});   // the comet, mid-flight
                }
                if (shot == "scrollback") {
                    std::string s = "\x1b[2J\x1b[H";
                    s += "\x1b]7;file://host" + std::string{std::getenv("HOME") ? std::getenv("HOME") : ""} + "/src/hand\a";
                    s += "\x1b]133;A\a\x1b[1;34m~/src/hand\x1b[0m \x1b[35m\u276F\x1b[0m \x1b]133;B\amake -j8\r\n\x1b]133;C\a";
                    for (int i = 0; i < 90; ++i)
                        s += "[" + std::to_string(i + 1) + "/90] \x1b[32mBuilding CXX\x1b[0m object src/" + std::string{"term"} + std::to_string(i) + ".cpp.o\r\n";
                    if (!std::getenv("HAND_RUNNING")) {
                        s += "\x1b[31merror:\x1b[0m linking failed\r\n\x1b]133;D;1\a";
                        s += "\x1b]133;A\a\x1b[1;34m~/src/hand\x1b[0m \x1b[35m\u276F\x1b[0m \x1b]133;B\a";
                    }
                    rt.send(hand::App::Msg{hand::App::Output{std::move(s)}});
                    rt.tick();
                    rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) {
                        hand::App::scroll_by(m, 40.0f);
                        m.scroll_d = 39.5f; m.scroll_applied = 39; m.term->set_scroll_offset(39);   // caught mid-glide
                        m.new_below = 0; m.sticky_t = 1.0f; m.clock = 3; }}});
                }
                if (shot == "minimap") {
                    std::string s;
                    const auto prompt = [&](const std::string& cmd) {
                        s += "\x1b]133;A\a\x1b[1;34m~/src/hand\x1b[0m \x1b[35m\u276F\x1b[0m \x1b]133;B\a" + cmd + "\r\n\x1b]133;C\a";
                    };
                    for (int round = 0; round < 6; ++round) {
                        prompt("git log --oneline");
                        for (int i = 0; i < 14; ++i)
                            s += "\x1b[33m" + std::to_string(1000000 + i * 7919 + round * 31) + "a\x1b[0m " +
                                 std::string("fix: render the minimap for every line ").substr(0, static_cast<std::size_t>(14 + (i * 7) % 26)) + "\r\n";
                        s += "\x1b]133;D;0\a";
                        prompt("cargo test");
                        for (int i = 0; i < 22; ++i)
                            s += std::string("test ") + (i % 7 == 3 ? "\x1b[31mFAILED\x1b[0m" : "\x1b[32mok\x1b[0m") +
                                 " tests::case_" + std::to_string(i) + std::string(static_cast<std::size_t>(i * 3 % 30), '.') + "\r\n";
                        s += round % 2 ? "\x1b]133;D;101\a" : "\x1b]133;D;0\a";
                        prompt("ls -la");
                        for (int i = 0; i < 9; ++i)
                            s += "drwxr-xr-x  ayush  \x1b[1;34mdir" + std::to_string(i) + "\x1b[0m  \x1b[36m" + std::string(static_cast<std::size_t>(4 + i * 4), '#') + "\x1b[0m\r\n";
                        s += "\x1b]133;D;0\a";
                    }
                    prompt("");
                    rt.send(hand::App::Msg{hand::App::Output{std::move(s)}});
                    rt.tick();
                    rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) {
                        (void)m.term->search("FAILED");
                        m.term->set_scroll_offset(120); m.scroll_d = m.scroll_goal = 120.0f; m.scroll_applied = 120;
                        m.mm_open = true; m.mm_w = 96.0f;
                        m.tail_t = 1.0f; m.new_below = 3; }}});
                    if (std::getenv("HAND_PEEK"))
                        rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) {
                            m.mm_hover_y = std::strtof(std::getenv("HAND_PEEK"), nullptr); m.peek_t = 1.0f; }}});
                }
                if (shot == "find") {
                    rt.send(hand::App::Msg{hand::App::Output{std::string{"build ok\r\nerror: oops\r\nwarning: unused\r\nerror: again\r\n"}}});
                    rt.tick();
                    rt.press_key(mayag::Key::f, mayag::Mods{.shift = true, .ctrl = true});
                    rt.type("error");
                    rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) {
                        m.toast = mayag::dsl::Toast{.title = "\u2713 done in 41.2s", .body = "cargo build --release",
                                                    .kind = mayag::dsl::Toast::Kind::success};
                        m.toast_at = m.time; }}});
                }
                if (shot == "minimap-short")
                    rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) { m.mm_open = true; m.mm_w = 96.0f; }}});
                if (shot == "resize")
                    rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) { m.resized_at = m.time; }}});
                if (shot == "settings")
                    rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) { m.settings_open = true; m.panel.selected = 1; }}});
                if (shot == "settings-menu") {
                    rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) { m.settings_open = true; }}});
                    rt.tick();
                    for (int i = 0; i < 3; ++i) rt.press_key(mayag::Key::down);
                    rt.press_key(mayag::Key::enter);
                    rt.press_key(mayag::Key::down);
                }
                rt.tick();
                const auto px = rt.window().read_pixels();
                const int W = static_cast<int>(size.x * 2.0f);
                const int H = static_cast<int>(px.size() / 4 / static_cast<std::size_t>(W));
                if (!imageio::write_png(out, px, W, H)) return 1;
                std::printf("hand: wrote %s (%dx%d)\n", out.c_str(), W, H);
                return 0;
            });
    }
    return 0;
}
