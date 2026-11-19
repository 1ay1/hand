// hand::App — tabs: open, close, switch, reorder.

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

App::Cmd App::update(Model& m, NewTab) {
    const std::string cwd = m.term ? m.term->cwd() : std::string{};   // the new tab opens where you are
    const bool first_split = m.tab_count() == 1 && !m.cfg.strip_always();
    m.tabs.push_back(spawn_session(m, cwd));
    const std::size_t i = m.tabs.size() - 1;
    m.switch_dir = 1;
    activate(m, i);
    m.switch_t = m.cfg.motion ? 0.0f : 1.0f;
    if (first_split && !m.cfg.motion) m.strip_t = 1.0f;
    fit(m);
    return SetTitle{m.title};
}

App::Cmd App::update(Model& m, CloseTab c) {
    if (c.i >= m.tab_count()) return {};
    if (!c.confirmed && m.cfg.close_confirm != CloseConfirm::never && !m.ask) {
        const bool busy = !busy_of(m.at(c.i), nullptr).empty();
        if (busy || m.cfg.close_confirm == CloseConfirm::always) {
            m.ask = Ask{.kind = m.tab_count() == 1 ? Ask::Kind::quit : Ask::Kind::close_tab, .tab = c.i, .focus = 1};
            return {};
        }
    }
    if (m.tab_count() == 1) {   // the last one: like foot, the window goes with it
        if (!m.cfg.demo) (void)config_save(m.cfg, config_path());
        return Cmd::quit();
    }
    if (c.i == m.active) {
        const std::size_t to = c.i + 1 < m.tab_count() ? c.i + 1 : c.i - 1;   // the neighbour on the right, else left
        m.switch_dir = to > c.i ? 1 : -1;
        activate(m, to);
        m.switch_t = m.cfg.motion ? 0.0f : 1.0f;
    }
    // the closing session's pty dies with its Session (SIGHUP to the shell)
    m.tabs.erase(m.tabs.begin() + static_cast<long>(c.i));
    if (m.active > c.i) --m.active;
    if (m.tab_count() == 1 && !m.cfg.strip_always() && !m.cfg.motion) m.strip_t = 0.0f;
    fit(m);
    return SetTitle{m.title};
}

App::Cmd App::update(Model& m, SelectTab st) {
    if (st.i >= m.tab_count() || st.i == m.active) return {};
    m.switch_dir = st.i > m.active ? 1 : -1;
    activate(m, st.i);
    m.switch_t = m.cfg.motion ? 0.0f : 1.0f;
    return SetTitle{m.title};
}

App::Cmd App::update(Model& m, StepTab st) {
    const auto n = static_cast<long>(m.tab_count());
    if (n < 2) return {};
    return update(m, SelectTab{static_cast<std::size_t>(((static_cast<long>(m.active) + st.d) % n + n) % n)});
}

App::Cmd App::update(Model& m, MoveTab mv) {
    const long to = static_cast<long>(m.active) + mv.d;
    if (to < 0 || to >= static_cast<long>(m.tab_count())) return {};
    std::swap(m.tabs[m.active], m.tabs[static_cast<std::size_t>(to)]);   // the placeholder moves with it
    m.active = static_cast<std::size_t>(to);
    return {};
}

}  // namespace hand
