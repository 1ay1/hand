#pragma once
// hand::App — the terminal's program.
//
// The view is ONE widget call -- window_view -- plus overlays; everything it
// draws is a mayag widget or a hand widget composed of them:
//
//   window_view     term_tabs (+ grab handle), chrome buttons, status_bar
//     workspace_view  pane_split / pane_frame / terminal_view
//   overlays        picker (palette), settings_view, toasts, resize_hud
//
// The look is mayag's theme system: theme(model) = looks::<chrome>(seed of
// the Appearance), so a settings change morphs the whole window, and every
// widget restyles through the theme's Sheet. Controls carry their messages
// inline (mayag handlers / Edit<Model>); the few id gestures left (tabs,
// palette rows) are decoded in on(Clicked) against mayag's part ids.

#include "core/settings.hpp"
#include "core/theme.hpp"
#include "core/workspace.hpp"
#include "demo.hpp"
#include "widget/settings_view.hpp"
#include "widget/window_view.hpp"
#include "widget/workspace_view.hpp"

#include <mayag/mayag.hpp>
#include <mayag/widget/picker.hpp>
#include <mayag/widget/resize_hud.hpp>
#include <mayag/widget/toast.hpp>

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hand {

struct App {
    struct Pane {
        PaneId       id;
        std::string  title, cwd, process;
        demo::Screen screen = demo::Screen::shell;
        PaneState    state = PaneState::idle;
        int          exit_code = 0;
    };

    struct Model {
        Appearance               look{};
        std::vector<std::string> faces;
        std::vector<Tab>         tabs;
        std::size_t              active = 0;
        std::vector<Pane>        panes;
        std::uint32_t            next_pane = 1;

        bool                     settings_open = false;
        settings::Section        section = settings::Section::colour;

        bool                     palette_open = false;
        mayag::TextEditState     query;
        int                      selected = 0;

        std::vector<mayag::dsl::Toast> toasts;
        std::vector<double>      toast_born;
        double                   time = 0.0;
        double                   resized_at = -10.0;
        bool                     window_focused = true;
        mayag::Vec2              size{1440, 900};

        // ── what workspace_view needs ────────────────────────────────────
        [[nodiscard]] const Tab& tab() const { return tabs[active]; }
        [[nodiscard]] Tab& tab() { return tabs[active]; }
        [[nodiscard]] const Pane* pane(PaneId id) const {
            for (const auto& p : panes) if (p.id == id) return &p;
            return nullptr;
        }
        /// Sample grids, generated from the CURRENT scheme so a scheme switch
        /// recolours the panes like a palette change would. A std::map: its
        /// nodes never move, so the pointers PaneView hands out stay valid
        /// for the whole frame.
        mutable std::map<std::pair<std::uint32_t, SchemeId>, mayag::dsl::TermGrid> grids;
        [[nodiscard]] PaneView pane_view(PaneId id) const {
            const Pane* p = pane(id);
            if (p == nullptr) return {};
            auto key = std::pair{id.v, look.scheme};
            auto it = grids.find(key);
            if (it == grids.end()) it = grids.emplace(key, demo::screen(p->screen, scheme(look.scheme))).first;
            return {p->title, &it->second, p->state == PaneState::failed, p->exit_code};
        }
        void focus(PaneId id) { tab().focus = id; }
        void nudge_split(std::uint32_t at, float delta) {
            if (at >= tab().nodes.size()) return;
            if (auto* s = std::get_if<Split>(&tab().nodes[at])) {
                const float extent = std::max(240.0f, s->axis == mayag::Axis::horizontal ? size.x : size.y);
                s->ratio = std::clamp(s->ratio + delta / extent, 0.1f, 0.9f);
            }
        }
        void even_split(std::uint32_t at) {
            if (at < tab().nodes.size())
                if (auto* s = std::get_if<Split>(&tab().nodes[at])) s->ratio = 0.5f;
        }
        void toast(mayag::dsl::Toast t) {
            toasts.push_back(std::move(t));
            toast_born.push_back(time);
        }
    };

    // ── messages ─────────────────────────────────────────────────────────
    struct Tick { double dt; };
    struct Blink {};
    struct SplitPane { mayag::Axis axis; };
    struct ClosePane {};
    struct Zoom {};
    struct Cycle { int d; };
    struct SelectTab { std::size_t i; };
    struct CloseTab { std::size_t i; };
    struct NewTab {};
    struct TogglePalette {};
    struct PaletteKey { mayag::Key key; mayag::Mods mods; };
    struct PaletteText { std::string text; };
    struct Run { int index; };
    struct ToggleSettings {};
    struct DismissToast { std::size_t i; };
    struct Resized { mayag::Vec2 size; };
    struct Focused { bool on; };
    struct Quit {};
    using Msg = std::variant<Tick, Blink, SplitPane, ClosePane, Zoom, Cycle, SelectTab, CloseTab, NewTab,
                             TogglePalette, PaletteKey, PaletteText, Run, ToggleSettings, DismissToast, Resized,
                             Focused, Quit, on_window::Move, on_window::ToggleMaximize, on_window::Minimize,
                             on_window::Close, on_window::OpenPalette, on_window::OpenSettings, mayag::Edit<Model>>;
    using Cmd = mayag::Cmd<Msg, mayag::set_title, mayag::window_action>;
    using Sub = mayag::Sub<Msg>;

    // ── the palette's commands: data, not code ───────────────────────────
    struct Command { mayag::dsl::PickerItem item; Msg msg; };
    [[nodiscard]] static std::vector<Command> commands() {
        using E = mayag::Edit<Model>;
        std::vector<Command> c{
            {{"split right", "pane", "ctrl shift d"}, SplitPane{mayag::Axis::horizontal}},
            {{"split down", "pane", "ctrl shift e"}, SplitPane{mayag::Axis::vertical}},
            {{"zoom pane", "pane", "ctrl shift z"}, Zoom{}},
            {{"close pane", "pane", "ctrl shift w"}, ClosePane{}},
            {{"next pane", "pane", "ctrl shift ]"}, Cycle{1}},
            {{"previous pane", "pane", "ctrl shift ["}, Cycle{-1}},
            {{"new tab", "tab", "ctrl shift t"}, NewTab{}},
            {{"appearance", "settings", "ctrl ,"}, ToggleSettings{}},
            {{"toggle cursor blink", "settings", ""}, E{[](Model& m) { m.look.cursor_blink = !m.look.cursor_blink; }}},
            {{"toggle pane titles", "settings", ""}, E{[](Model& m) { m.look.pane_titles = !m.look.pane_titles; }}},
            {{"bigger text", "settings", "ctrl ="}, E{[](Model& m) { m.look.font_size = m.look.font_size.get() + 1.0f; }}},
            {{"smaller text", "settings", "ctrl -"}, E{[](Model& m) { m.look.font_size = m.look.font_size.get() - 1.0f; }}},
            {{"reset appearance", "settings", ""}, E{[](Model& m) { const auto f = m.look.mono; m.look = {}; m.look.mono = f; }}},
            {{"maximise window", "window", ""}, on_window::ToggleMaximize{}},
            {{"quit", "window", "ctrl q"}, Quit{}},
        };
        for (std::size_t s = 0; s < scheme_count; ++s)
            c.push_back({{"scheme: " + std::string{schemes[s].name}, "colour", ""},
                         E{[s](Model& m) { m.look.scheme = static_cast<SchemeId>(s); }}});
        for (std::size_t k = 0; k < static_cast<std::size_t>(Chrome::count_); ++k)
            c.push_back({{"chrome: " + std::string{labels<Chrome>[k]}, "colour", ""},
                         E{[k](Model& m) { m.look.chrome = static_cast<Chrome>(k); }}});
        for (std::size_t k = 0; k < static_cast<std::size_t>(CursorStyle::count_); ++k)
            c.push_back({{"cursor: " + std::string{labels<CursorStyle>[k]}, "terminal", ""},
                         E{[k](Model& m) { m.look.cursor = static_cast<CursorStyle>(k); }}});
        return c;
    }
    [[nodiscard]] static std::vector<mayag::dsl::PickerItem> picker_items() {
        std::vector<mayag::dsl::PickerItem> items;
        for (const auto& c : commands()) items.push_back(c.item);
        return items;
    }

    // ── init ─────────────────────────────────────────────────────────────
    static PaneId add(Model& m, std::string title, std::string cwd, std::string proc, demo::Screen s,
                      PaneState st = PaneState::idle, int code = 0) {
        const PaneId id{m.next_pane++};
        m.panes.push_back({id, std::move(title), std::move(cwd), std::move(proc), s, st, code});
        return id;
    }
    static Cmd init(Model& m) {
        const PaneId a = add(m, "zsh \u2014 ~/src/hand", "~/src/hand", "zsh", demo::Screen::shell);
        const PaneId b = add(m, "nvim theme.hpp", "~/src/hand", "nvim", demo::Screen::editor);
        const PaneId c = add(m, "ctest", "~/src/hand", "ctest", demo::Screen::tests, PaneState::failed, 8);
        Tab t = Tab::single(a);
        t.split(a, b, mayag::Axis::horizontal);
        t.split(b, c, mayag::Axis::vertical);
        if (auto* s = std::get_if<Split>(&t.nodes[t.root])) s->ratio = 0.54f;
        t.focus = a;
        m.tabs.push_back(std::move(t));
        m.tabs.push_back(Tab::single(add(m, "cmake \u2014 toe", "~/src/toe", "cmake", demo::Screen::build, PaneState::running)));
        m.tabs.push_back(Tab::single(add(m, "deploy@prod-01", "/var/log", "ssh", demo::Screen::logs, PaneState::done)));
        return mayag::SetTitle{"hand"};
    }

    // ── update ───────────────────────────────────────────────────────────
    static constexpr double toast_life = 6.0;

    static Cmd update(Model& m, Tick t) {
        m.time += t.dt;
        for (std::size_t i = m.toasts.size(); i-- > 0;)
            if (m.time - m.toast_born[i] > toast_life) {
                m.toasts.erase(m.toasts.begin() + static_cast<long>(i));
                m.toast_born.erase(m.toast_born.begin() + static_cast<long>(i));
            }
        return {};
    }
    static Cmd update(Model& m, Blink) { m.time += 0.55; return {}; }
    static Cmd update(Model& m, SplitPane s) {
        const PaneId f = m.tab().focus;
        const Pane* src = m.pane(f);
        const PaneId n = add(m, "zsh", src ? src->cwd : "~", "zsh", demo::Screen::shell);
        m.tab().split(f, n, s.axis);
        return {};
    }
    static Cmd update(Model& m, ClosePane) {
        if (m.tab().panes().size() <= 1) return update(m, CloseTab{m.active});
        const PaneId f = m.tab().focus;
        m.tab().close(f);
        std::erase_if(m.panes, [&](const Pane& p) { return p.id == f; });
        return {};
    }
    static Cmd update(Model& m, Zoom) {
        auto& t = m.tab();
        if (t.zoom) t.zoom.reset(); else if (t.panes().size() > 1) t.zoom = t.focus;
        return {};
    }
    static Cmd update(Model& m, Cycle c) {
        const auto ps = m.tab().panes();
        for (std::size_t i = 0; i < ps.size(); ++i)
            if (ps[i] == m.tab().focus) {
                const auto n = static_cast<long>(ps.size());
                m.tab().focus = ps[static_cast<std::size_t>(((static_cast<long>(i) + c.d) % n + n) % n)];
                break;
            }
        return {};
    }
    static Cmd update(Model& m, SelectTab s) { if (s.i < m.tabs.size()) m.active = s.i; return {}; }
    static Cmd update(Model& m, CloseTab c) {
        if (c.i >= m.tabs.size()) return {};
        for (PaneId p : m.tabs[c.i].panes()) std::erase_if(m.panes, [&](const Pane& x) { return x.id == p; });
        m.tabs.erase(m.tabs.begin() + static_cast<long>(c.i));
        if (m.tabs.empty()) return Cmd::quit();
        m.active = std::min(m.active, m.tabs.size() - 1);
        return {};
    }
    static Cmd update(Model& m, NewTab) {
        m.tabs.push_back(Tab::single(add(m, "zsh", "~", "zsh", demo::Screen::shell)));
        m.active = m.tabs.size() - 1;
        return {};
    }
    static Cmd update(Model& m, TogglePalette) {
        m.palette_open = !m.palette_open;
        m.settings_open = false;
        m.query.clear();
        m.selected = 0;
        return {};
    }
    static Cmd update(Model& m, PaletteKey k) {
        const auto items = picker_items();
        const auto ranked = mayag::dsl::rank_items(items, m.query.text);
        using A = mayag::dsl::PickerIntent::Act;
        const auto in = mayag::dsl::picker_key(m.query, k.key, k.mods, m.selected, static_cast<int>(ranked.order.size()));
        if (in.act == A::close) m.palette_open = false;
        if (in.act == A::choose && in.index >= 0 && in.index < static_cast<int>(ranked.order.size()))
            return update(m, Run{ranked.order[static_cast<std::size_t>(in.index)]});
        return {};
    }
    static Cmd update(Model& m, PaletteText t) { m.query.insert(t.text); m.selected = 0; return {}; }
    static Cmd update(Model& m, Run r) {
        auto cmds = commands();
        m.palette_open = false;
        if (r.index < 0 || r.index >= static_cast<int>(cmds.size())) return {};
        Msg msg = std::move(cmds[static_cast<std::size_t>(r.index)].msg);
        return std::visit([&](auto&& x) -> Cmd { return update(m, std::move(x)); }, std::move(msg));
    }
    static Cmd update(Model& m, ToggleSettings) {
        m.settings_open = !m.settings_open;
        m.palette_open = false;
        return {};
    }
    static Cmd update(Model& m, DismissToast d) {
        if (d.i < m.toasts.size()) {
            m.toasts.erase(m.toasts.begin() + static_cast<long>(d.i));
            m.toast_born.erase(m.toast_born.begin() + static_cast<long>(d.i));
        }
        return {};
    }
    static Cmd update(Model& m, Resized r) {
        if (r.size.x != m.size.x || r.size.y != m.size.y) m.resized_at = m.time;
        m.size = r.size;
        return {};
    }
    static Cmd update(Model& m, Focused f) { m.window_focused = f.on; return {}; }
    static Cmd update(Model&, Quit) { return Cmd::quit(); }
    static Cmd update(Model&, on_window::Move) { return mayag::WindowAction{mayag::WindowAction::Kind::move}; }
    static Cmd update(Model&, on_window::ToggleMaximize) { return mayag::WindowAction{mayag::WindowAction::Kind::toggle_maximize}; }
    static Cmd update(Model&, on_window::Minimize) { return mayag::WindowAction{mayag::WindowAction::Kind::minimize}; }
    static Cmd update(Model&, on_window::Close) { return Cmd::quit(); }
    static Cmd update(Model& m, on_window::OpenPalette) { return update(m, TogglePalette{}); }
    static Cmd update(Model& m, on_window::OpenSettings) { return update(m, ToggleSettings{}); }
    static Cmd update(Model& m, mayag::Edit<Model> e) {
        const SchemeId before = m.look.scheme;
        e(m);
        if (m.look.scheme != before)
            m.toast({.title = "scheme \u00B7 " + std::string{scheme(m.look.scheme).name},
                     .body = "the chrome follows the palette", .kind = mayag::dsl::Toast::Kind::info});
        return {};
    }

    // ── input ────────────────────────────────────────────────────────────
    static std::optional<Msg> on(const Model& m, const mayag::Clicked& c) {
        using namespace mayag;
        for (std::size_t i = 0; i < m.tabs.size(); ++i) {
            if (is(c, "tabs", i)) return SelectTab{i};
            if (is(c, "tabs", dsl::term_part_close + i)) return CloseTab{i};
        }
        if (is(c, "tabs", dsl::part_inc)) return NewTab{};
        if (is(c, "tabs", dsl::part_menu)) return TogglePalette{};
        if (m.palette_open) {
            const auto ranked = dsl::rank_items(picker_items(), m.query.text);
            for (std::size_t i = 0; i < ranked.order.size(); ++i)
                if (is(c, "palette", i)) return Run{ranked.order[i]};
        }
        for (std::size_t i = 0; i < m.toasts.size(); ++i)
            if (is(c, "toasts", i)) return DismissToast{i};
        if (is(c, "status", 100)) return ToggleSettings{};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model& m, const mayag::Left& l) {
        if (m.palette_open && l.node == mayag::dsl::part_id("palette", mayag::dsl::part_menu)) return TogglePalette{};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model& m, const mayag::TextEvent& t) {
        if (m.palette_open) return PaletteText{t.text};
        return std::nullopt;
    }
    static std::optional<Msg> on(const Model& m, const mayag::KeyEvent& k) {
        using mayag::Key;
        const bool chord = k.mods.primary() && k.mods.shift;
        if (chord && k.key == Key::p) return TogglePalette{};
        if (k.mods.primary() && !k.mods.shift && k.key == Key::comma) return ToggleSettings{};
        if (k.mods.primary() && !k.mods.shift && k.key == Key::q) return Quit{};
        if (m.palette_open) return PaletteKey{k.key, k.mods};
        if (m.settings_open && k.key == Key::escape) return ToggleSettings{};
        if (k.mods.primary() && !k.mods.shift && (k.key == Key::equal || k.key == Key::minus)) {
            const float d = k.key == Key::equal ? 1.0f : -1.0f;
            return mayag::Edit<Model>{[d](Model& mm) { mm.look.font_size = mm.look.font_size.get() + d; }};
        }
        if (!chord) return std::nullopt;
        switch (k.key) {
            case Key::d: return SplitPane{mayag::Axis::horizontal};
            case Key::e: return SplitPane{mayag::Axis::vertical};
            case Key::w: return ClosePane{};
            case Key::z: return Zoom{};
            case Key::t: return NewTab{};
            case Key::bracket_right: return Cycle{1};
            case Key::bracket_left:  return Cycle{-1};
            default: return std::nullopt;
        }
    }
    static std::optional<Msg> on(const Model&, const mayag::ResizeEvent& r) { return Resized{r.size}; }
    static std::optional<Msg> on(const Model&, const mayag::FocusEvent& f) { return Focused{f.focused}; }
    static std::optional<Msg> on(const Model&, const mayag::CloseRequest&) { return Quit{}; }

    static Sub subscribe(const Model& m) {
        std::vector<Sub> s;
        if (m.look.cursor_blink && m.window_focused) s.push_back(Sub::every(std::chrono::milliseconds(550), Msg{Blink{}}));
        bool live = m.time - m.resized_at < 0.9 || !m.toasts.empty();
        for (const auto& p : m.panes) live = live || p.state == PaneState::running;
        if (live) s.push_back(Sub::on_frame([](double dt) -> Msg { return Tick{dt}; }));
        return Sub::batch(std::move(s));
    }

    static mayag::Theme theme(const Model& m) { return theme_of(m.look); }

    static mayag::Node view(const Model& m);
};

inline mayag::Node App::view(const Model& m) {
    using namespace mayag;
    using namespace mayag::dsl;
    const Ctx& c = ctx();

    // ── what the chrome shows ────────────────────────────────────────────
    WindowViewProps w{.active = static_cast<int>(m.active), .focused = m.window_focused, .time = m.time};
    for (const auto& tab : m.tabs) {
        TermTab tt;
        const Pane* f = m.pane(tab.focus);
        tt.title = f ? f->title : "zsh";
        tt.process = f ? f->process : "";
        for (PaneId pid : tab.panes())
            if (const Pane* p = m.pane(pid)) {
                if (p->state == PaneState::running) tt.running = true;
                if (p->state == PaneState::failed) tt.exit_code = p->exit_code;
            }
        if (tt.process == "ssh") tt.host = SshHost{.alias = "prod-01", .env = SshHost::Env::prod};
        w.tabs.push_back(std::move(tt));
    }
    if (const Pane* f = m.pane(m.tab().focus)) {
        using T = StatusItem::Tone;
        const auto g = m.pane_view(f->id);
        w.status_left = {
            {.text = f->cwd, .icon = "\u25C6", .interactive = false},
            {.text = "main", .icon = "\u2387", .tone = T::accent},
        };
        if (m.tab().zoom) w.status_left.push_back({.text = "ZOOM", .tone = T::warning, .interactive = false});
        if (f->state == PaneState::failed)
            w.status_left.push_back({.text = "exit " + std::to_string(f->exit_code), .tone = T::error, .interactive = false});
        w.status_right = {
            {.text = std::string{scheme(m.look.scheme).name} + " \u00B7 " +
                     std::string{labels<Chrome>[static_cast<std::size_t>(m.look.chrome)]}, .icon = "\u25D0"},
            {.text = f->process, .tone = T::dim, .interactive = false},
            {.text = g.grid ? std::to_string(g.grid->cols) + "\u00D7" + std::to_string(g.grid->rows) : "",
             .tone = T::dim, .interactive = false},
            {.text = "utf-8", .tone = T::dim, .interactive = false},
        };
        // status_bar names right items part 100+i; the scheme is item 0.
    }

    Node root = window_view(c, m, w);

    // ── overlays ─────────────────────────────────────────────────────────
    if (m.palette_open) {
        const auto items = picker_items();
        picker(c, "palette", m.query, items, rank_items(items, m.query.text), m.selected,
               PickerStyle{.placeholder = "run a command\u2026", .prefix = ">", .width = 640.0f, .max_rows = 11,
                           .footer = {{"\u2191\u2193", "move"}, {"\u21B5", "run"}, {"esc", "close"}}});
    }
    if (m.settings_open)
        c.overlay(Overlay{.content = settings_view(c, m, {.section = m.section, .faces = m.faces}),
                          .id = part_id("settings", part_menu), .placement = Placement::centered,
                          .dismiss = mayag::Dismiss::modal, .layer = 15, .scrim = 0.4f});
    toasts(c, "toasts", m.toasts, c.size);
    if (m.time - m.resized_at < 0.8) {
        const auto g = m.pane_view(m.tab().focus);
        if (g.grid) resize_hud(c, "resize", g.grid->cols, g.grid->rows);
    }
    return root;
}

}  // namespace hand
