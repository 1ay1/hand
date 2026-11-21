// hand::App — the modes that take the keyboard from the shell: copy mode,
// hints, the host picker's query, and the file pane's focus.

#include "detail.hpp"

#include <string_view>

namespace hand {

using mayag::dsl::Toast;

App::Cmd App::update(Model& m, ToggleCopyMode) {
    m.grid_key = 0;
    if (m.copy) {
        m.copy.reset();
        m.term->select_clear();
        return {};
    }
    // from the selection's end, else from the live cursor
    const auto si = m.term->selection_info();
    m.copy = si.any ? CopyMode{.row = si.r1, .col = si.c1} : CopyMode{.row = m.term->cursor_abs(), .col = m.term->cursor().col};
    return {};
}

App::Cmd App::update(Model& m, OpenHints) {
    HintMode h{.hints = m.term->hints()};
    // home row first, then the rest; two letters once those run out
    static constexpr std::string_view keys = "asdfjklghqwertyuiopzxcvbnm";
    const std::size_t n = h.hints.size();
    for (std::size_t i = 0; i < n; ++i)
        h.labels.push_back(n <= keys.size() ? std::string(1, keys[i])
                                            : std::string{keys[i / keys.size() % keys.size()], keys[i % keys.size()]});
    m.grid_key = 0;
    if (!h.hints.empty()) {
        m.hint = std::move(h);
    } else {
        m.toast.card = Toast{.title = "nothing to pick", .body = "no links, paths, hashes or IPs on screen"};
        m.toast.at = m.time;
    }
    return {};
}

App::Cmd App::update(Model& m, CloseHints) {
    m.hint.reset();
    m.grid_key = 0;
    return {};
}

App::Cmd App::update(Model& m, HintBackspace) {
    if (m.hint && !m.hint->typed.empty()) m.hint->typed.pop_back();
    m.grid_key = 0;
    return {};
}

App::Cmd App::update(Model& m, HintTyped h) {
    if (m.hint) m.hint->typed = std::move(h.typed);
    m.grid_key = 0;
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
