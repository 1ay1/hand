// hand::App — settings: the panel over the terminal, and its edits.

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

App::Cmd App::update(Model& m, ToggleSettings) {
    m.settings_open = !m.settings_open;
    m.panel.filtering = false;
    if (m.panel.menu >= 0) dsl::settings_ops::close_menu(m.panel, true);
    for (auto& e : std::exchange(m.panel.out, {})) { const Config before = m.cfg; if (setting_apply(m.cfg, e)) apply(m, before); }
    if (!m.settings_open && !m.cfg.demo) (void)config_save(m.cfg, config_path());   // closing is "done"
    return {};
}

App::Cmd App::update(Model& m, Panel p) {
    p.op(m.panel);
    for (auto& e : std::exchange(m.panel.out, {})) {
        const Config before = m.cfg;
        if (setting_apply(m.cfg, e)) apply(m, before);
    }
    if (std::exchange(m.panel.close, false)) return update(m, ToggleSettings{});
    return {};
}

}  // namespace hand
