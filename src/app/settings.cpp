// hand::App — settings: the panel over the terminal, and its edits.

#include "detail.hpp"
#include "fx/fx.hpp"

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
    m.settings.open = !m.settings.open;
    m.settings.ui.filtering = false;
    if (m.settings.ui.menu >= 0) dsl::settings_ops::close_menu(m.settings.ui, true);
    for (auto& e : std::exchange(m.settings.ui.out, {})) { const Config before = m.cfg; if (setting_apply(m.cfg, e)) apply(m, before); }
    if (!m.settings.open && !m.cfg.demo) return Cmd::run(fx::save_config, m.cfg, m.env.config_path);   // closing is "done"
    return {};
}

App::Cmd App::update(Model& m, Panel p) {
    p.op(m.settings.ui);
    for (auto& e : std::exchange(m.settings.ui.out, {})) {
        const Config before = m.cfg;
        if (setting_apply(m.cfg, e)) apply(m, before);
    }
    if (std::exchange(m.settings.ui.close, false)) return update(m, ToggleSettings{});
    return {};
}

}  // namespace hand
