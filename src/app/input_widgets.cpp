// hand::App — the window and its widgets into messages: resize, focus, and
// the gestures mayag resolved on a node (Clicked / Pressed / Dragged /
// Released / Left on the tab strip, the dialog, the selection chip, search).

#include "detail.hpp"

#include <hand/core/shell.hpp>

#include <mayag/widget/ssh.hpp>
#include <mayag/widget/term_dialog.hpp>
#include <mayag/widget/toast.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace hand {

using namespace mayag;
using namespace mayag::dsl;
using namespace hand::detail;

std::optional<App::Msg> App::on(const Model&, const ResizeEvent& r) {
    return Resized{r.size};
}

std::optional<App::Msg> App::on(const Model& m, const FocusEvent& f) {
    return Focused{f.focused, m.tab().term->encode_focus(f.focused)};
}

std::optional<App::Msg> App::on(const Model& m, const Left& l) {
    if (m.ssh.picker.open && l.node == part_id("hosts", part_menu)) return PickerClose{};
    if (m.settings.open)
        if (auto op = settings_panel_left(m.settings.ui, "settings", l.node)) return Panel{std::move(*op)};
    return std::nullopt;
}

/// The tab strip: press selects (instantly, like a browser), drag reorders.
std::optional<App::Msg> App::on(const Model& m, const Pressed& p) {
    // no system title bar: the strip's empty space is the title bar. Decided on
    // the PRESS -- a window drag swallows the release, so no click follows it.
    if (!m.cfg.decorations && p.node == node_id("tabs") && p.button == MouseButton::left)
        return WindowAct{p.count >= 2 ? WindowAction::Kind::toggle_maximize : WindowAction::Kind::move};
    for (std::size_t i = 0; i < m.tab_count(); ++i)
        if (p.node == part_id("tabs", i) && p.button == MouseButton::left)
            return TabPress{TabIndex{i}};
    return std::nullopt;
}

std::optional<App::Msg> App::on(const Model& m, const Dragged& d) {
    if (m.strip.drag_tab < 0) return std::nullopt;
    return TabDrag{d.delta.x};
}

std::optional<App::Msg> App::on(const Model& m, const Released&) {
    if (m.strip.drag_tab < 0) return std::nullopt;
    return TabRelease{};
}

/// The tab strip's buttons, and the search bar's (mayag term_search parts).
std::optional<App::Msg> App::on(const Model& m, const Clicked& c) {
    if (m.ssh.picker.open) {
        if (c.node == part_id("hosts", part_inc)) return ConnectTyped{};
        const auto ranked = picker_rank(m);
        for (std::size_t k = 0; k < ranked.size(); ++k)
            if (c.node == part_id("hosts", k)) return ConnectHost{m.ssh.book.hosts[static_cast<std::size_t>(ranked[k])]};
    }
    if (c.node == node_id("ssh.reconnect")) return Reconnect{};
    if (c.node == node_id("ssh.close")) return CloseTab{TabIndex{m.active()}, Confirm::given};
    for (std::size_t i = 0; i < m.tab_count(); ++i) {
        if (c.node == part_id("tabs", tab_part_close + i)) return CloseTab{TabIndex{i}};
        if (c.node == part_id("tabs", i) && c.button == MouseButton::middle) return CloseTab{TabIndex{i}};
        if (c.node == part_id("tabs", i) && c.count == 2) return NewTab{};   // double-click a tab: a new one beside it
    }
    // the window's own buttons (no system title bar)
    for (const char* bar : {"tabs", "corner"}) {
        if (c.node == part_id(bar, tab_part_window + 0)) return WindowAct{WindowAction::Kind::minimize};
        if (c.node == part_id(bar, tab_part_window + 1)) return WindowAct{WindowAction::Kind::toggle_maximize};
        if (c.node == part_id(bar, tab_part_window + 2)) return RequestQuit{};
    }
    if (c.node == part_id("tabs", part_inc) || (c.node == node_id("tabs") && c.count == 2 && m.cfg.decorations)) return NewTab{};
    if (c.node == part_id("tabs", part_menu)) return OpenHosts{};   // the ⌄ beside + : connect somewhere
    if (m.dialog.ask.has_value()) {
        for (std::size_t i = 0; i < 3; ++i) if (c.node == part_id("ask", i)) return Answer::choose(i);
        if (c.node == part_id("ask", 100)) return RememberToggle{};
    }
    if (c.node == node_id("sel.copy")) return Copy{};
    if (c.node == node_id("sel.find")) return SearchToggle{};
    if (c.node == node_id("sel.open")) return OpenUrl{m.tab().term->selection_text()};
    if (c.node == node_id("sel.clear")) return SelectClear{};
    if (!m.tab().search_open) return std::nullopt;
    if (c.node == part_id("search", 5)) return SearchStep{Step::backward};
    if (c.node == part_id("search", 6)) return SearchStep{Step::forward};
    if (c.node == part_id("search", 9)) return SearchToggle{};
    return std::nullopt;
}

}  // namespace hand
