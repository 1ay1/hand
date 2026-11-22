// hand::App — tabs: open, close, switch, reorder.

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

App::Cmd App::update(Model& m, NewTab) {
    const std::string cwd = m.tab().term ? m.tab().term->cwd() : std::string{};   // the new tab opens where you are
    const bool first_split = m.tab_count() == 1 && !m.cfg.strip_always();
    const std::size_t i = m.tabs.push_back(spawn_session(m, cwd));
    m.strip.switch_dir = 1;
    activate(m, i);
    m.strip.switch_t = m.cfg.motion ? 0.0f : 1.0f;
    if (first_split && !m.cfg.motion) m.strip.t = 1.0f;
    fit(m);
    return SetTitle{m.tab().title};
}

App::Cmd App::update(Model& m, CloseTab c) {
    const std::size_t i = c.tab.get();
    if (i >= m.tab_count()) return {};
    if (c.confirm == Confirm::asking && m.cfg.close_confirm != CloseConfirm::never && !m.dialog.ask) {
        const bool busy = !busy_of(m, m.at(i), nullptr).empty();
        if (busy || m.cfg.close_confirm == CloseConfirm::always) {
            m.dialog.ask = Ask{.kind = m.tab_count() == 1 ? Ask::Kind::quit : Ask::Kind::close_tab, .tab = c.tab, .focus = 1};
            return {};
        }
    }
    if (m.tab_count() == 1) {   // the last one: like foot, the window goes with it
        if (!m.cfg.demo) return Cmd::batch(Cmd::run(fx::save_config, m.cfg, m.env.config_path), Cmd::quit());
        return Cmd::quit();
    }
    if (i == m.active()) {
        const std::size_t to = i + 1 < m.tab_count() ? i + 1 : i - 1;   // the neighbour on the right, else left
        m.strip.switch_dir = to > i ? 1 : -1;
        activate(m, to);
        m.strip.switch_t = m.cfg.motion ? 0.0f : 1.0f;
    }
    // the closing session's pty dies with its Session (SIGHUP to the shell)
    m.tabs.erase(i);   // the active tab stays in front
    if (m.tab_count() == 1 && !m.cfg.strip_always() && !m.cfg.motion) m.strip.t = 0.0f;
    fit(m);
    return SetTitle{m.tab().title};
}

App::Cmd App::update(Model& m, SelectTab st) {
    const std::size_t i = st.tab.get();
    if (i >= m.tab_count() || i == m.active()) return {};
    m.strip.switch_dir = i > m.active() ? 1 : -1;
    activate(m, i);
    m.strip.switch_t = m.cfg.motion ? 0.0f : 1.0f;
    std::vector<Cmd> cmds;
    cmds.push_back(SetTitle{m.tab().title});
    cmds.push_back(pane_sync(m));
    return Cmd::batch(std::move(cmds));
}

App::Cmd App::update(Model& m, StepTab st) {
    if (m.tab_count() < 2) return {};
    return update(m, SelectTab{TabIndex{wrap(m.active(), st.step, m.tab_count())}});
}

App::Cmd App::update(Model& m, MoveTab mv) {
    (void)m.tabs.move_active(mv.step);   // no-op at either end
    return {};
}



// ── reordering by drag ─────────────────────────────────────────────────────

App::Cmd App::update(Model& m, TabPress p) {
    (void)update(m, SelectTab{p.tab});   // instantly, like a browser
    m.strip.drag_tab = static_cast<int>(m.active());
    m.strip.drag_dx = m.strip.drag_shift = 0.0f;
    return {};
}

App::Cmd App::update(Model& m, TabDrag d) {
    if (m.strip.drag_tab < 0) return {};
    // by position: the pointer's travel since the press, less the slots the
    // tab has already moved (so the tab stays under the pointer)
    const float tab_w = std::clamp(m.win.x / static_cast<float>(m.tab_count()), 96.0f, 236.0f);
    m.strip.drag_dx = d.travel.x() - m.strip.drag_shift;
    // past half a tab: swap with the neighbour (the pill slides after it)
    while (m.strip.drag_dx > tab_w * 0.5f && m.active() + 1 < m.tab_count()) {
        (void)update(m, MoveTab{Step::forward});
        m.strip.drag_shift += tab_w;
        m.strip.drag_dx -= tab_w;
    }
    while (m.strip.drag_dx < -tab_w * 0.5f && m.active() > 0) {
        (void)update(m, MoveTab{Step::backward});
        m.strip.drag_shift -= tab_w;
        m.strip.drag_dx += tab_w;
    }
    m.strip.drag_tab = static_cast<int>(m.active());
    return {};
}

App::Cmd App::update(Model& m, TabRelease) {
    m.strip.drag_tab = -1;
    m.strip.drag_dx = m.strip.drag_shift = 0.0f;
    return {};
}

}  // namespace hand
