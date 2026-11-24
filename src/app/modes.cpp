// hand::App — the modes that take the keyboard from the shell: copy mode,
// hints, the host picker's query, and the file pane's focus.

#include "detail.hpp"

#include <string_view>

namespace hand {

using mayag::dsl::Toast;

App::Cmd App::update(Model& m, ToggleCopyMode) {
    Session& se = m.tab();
    se.grid_key = 0;
    if (se.copy) {
        se.copy.reset();
        se.term->select_clear();
        return {};
    }
    // from the selection's end, else from the live cursor
    const auto si = se.term->selection_info();
    se.copy = si.any ? CopyMode{.row = si.r1, .col = si.c1} : CopyMode{.row = se.term->cursor_abs(), .col = se.term->cursor().col};
    return {};
}

App::Cmd App::update(Model& m, OpenHints) {
    Session& se = m.tab();
    HintMode h{.hints = se.term->hints()};
    // home row first, then the rest; two letters once those run out
    static constexpr std::string_view keys = "asdfjklghqwertyuiopzxcvbnm";
    const std::size_t n = h.hints.size();
    for (std::size_t i = 0; i < n; ++i)
        h.labels.push_back(n <= keys.size() ? std::string(1, keys[i])
                                            : std::string{keys[i / keys.size() % keys.size()], keys[i % keys.size()]});
    se.grid_key = 0;
    if (!h.hints.empty()) {
        se.hint = std::move(h);
    } else {
        m.toast.card = Toast{.title = "nothing to pick", .body = "no links, paths, hashes or IPs on screen"};
        ++m.toast.serial;
    }
    return {};
}

App::Cmd App::update(Model& m, CloseHints) {
    m.tab().hint.reset();
    m.tab().grid_key = 0;
    return {};
}

App::Cmd App::update(Model& m, HintBackspace) {
    Session& se = m.tab();
    if (se.hint && !se.hint->typed.empty()) se.hint->typed.pop_back();
    se.grid_key = 0;
    return {};
}

App::Cmd App::update(Model& m, HintTyped h) {
    Session& se = m.tab();
    if (se.hint) se.hint->typed = std::move(h.typed);
    se.grid_key = 0;
    return {};
}

App::Cmd App::update(Model& m, PickerText t) {
    m.ssh.picker.query.insert(t.text);
    m.ssh.picker.sel = 0;
    return {};
}

App::Cmd App::update(Model& m, PickerClose) {
    m.ssh.picker.open = false;
    return {};
}

App::Cmd App::update(Model& m, PaneBlur) {
    m.pane.ui.focused = false;
    return {};
}

}  // namespace hand
