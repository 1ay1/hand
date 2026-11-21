// hand::view — the tab strip.
//
// What each tab says is derived from its session (view::tab_info); the strip
// itself is mayag's tab_strip in the configured style.

#include "view.hpp"

#include <mayag/widget/tab_strip.hpp>

#include <cstring>

namespace hand::view {

using namespace mayag;
using namespace mayag::dsl;
using namespace hand::detail;

namespace {

/// "~/src/hand" for the strip's second line.
std::string tilde(const std::string& p) {
    if (const char* h = std::getenv("HOME"); h && *h && p.rfind(h, 0) == 0) return "~" + p.substr(std::strlen(h));
    return p;
}

/// The title: your name for it > the running command > the program >
/// the shell's own title > the folder (cfg.tab_title picks one source).
std::string title_of(const Model& m, const Session& se, const std::string& proc, const std::string& cwd,
                     const std::string& running) {
    if (!se.name.empty()) return se.name;
    const bool shell_proc = proc.empty() || is_shell(proc);
    std::string base = cwd.empty() ? std::string{} : cwd.substr(cwd.find_last_of('/') + 1);
    if (base.empty() && !cwd.empty()) base = "/";
    // a shell title that is just the directory ("~/P/hand") says less than the folder
    const bool path_title = se.title.empty() || se.title == "hand" || se.title[0] == '~' || se.title[0] == '/' ||
                            se.title.find(cwd.empty() ? "\x01" : base) != std::string::npos;
    const std::string folder = !base.empty() ? base : "shell";
    switch (m.cfg.tab_title) {
        case TabTitle::folder:  return folder;
        case TabTitle::program: return !shell_proc ? proc : proc.empty() ? folder : proc;
        case TabTitle::shell:   return se.title.empty() ? folder : se.title;
        case TabTitle::smart:   return !running.empty() ? running : !shell_proc ? proc : !path_title ? se.title : folder;
    }
    return folder;
}

/// One tab, as the strip shows it.
TermTabInfo tab_info(const Model& m, std::size_t i) {
    const Session& se = m.at(i);
    TermTabInfo ti;
    const std::string proc = fg_process(se);
    const std::string cwd = se.term ? se.term->cwd() : std::string{};
    // the last command that ran: going now, or how it ended
    std::optional<Term::Command> last;
    if (se.term) {
        const auto cs = se.term->commands();
        for (auto it = cs.rbegin(); it != cs.rend(); ++it) if (it->ran) { last = *it; break; }
    }
    const std::string running = last && !last->finished ? se.term->command_text(*last) : std::string{};
    ti.title = title_of(m, se, proc, cwd, running);
    ti.cwd = tilde(cwd);
    ti.process = proc;
    if (const auto& r = remote_of(se)) {   // a remote tab: named for the machine, its second line the address
        if (se.name.empty() && m.cfg.tab_title != TabTitle::shell) ti.title = r->alias;
        ti.host = se.link.dropped ? "disconnected" : (r->user.empty() ? "" : r->user + "@") + r->hostname;
        ti.host_danger = se.link.dropped || (m.cfg.ssh_prod_guard && remote_env(m, *r) == HostEnv::prod);
        if (se.link.launched) ti.process = "ssh";
    }
    ti.unread = se.unread && i != m.active();
    ti.bell = se.bell && i != m.active();
    ti.activity = se.activity;
    if (last) {
        ti.running = !last->finished;
        if (last->finished) {
            ti.last_exit = last->exit_code;
            ti.finished_ago = last->start_ms > 0 ? static_cast<double>(wall_ms() - last->start_ms - last->duration_ms) / 1000.0 : 99.0;
        } else if (last->start_ms > 0) {
            ti.elapsed = static_cast<double>(wall_ms() - last->start_ms) / 1000.0;
        }
    }
    return ti;
}

}  // namespace

Node tabs(const Ctx& c, const Model& m, float height) {
    std::vector<TermTabInfo> infos;
    infos.reserve(m.tab_count());
    for (std::size_t i = 0; i < m.tab_count(); ++i) infos.push_back(tab_info(m, i));
    const Config& cfg = m.cfg;
    return tab_strip(c, "tabs", infos, static_cast<int>(m.active()), &m.strip.motion, m.time,
                     {.show_numbers = m.strip.show_numbers,
                      .max_tab = cfg.tab_width, .min_tab = std::min(96.0f, cfg.tab_width),
                      .height = height,
                      .style = cfg.tab_style,
                      .radius = cfg.tab_radius >= 0.0f ? cfg.tab_radius : 3.0f * cfg.roundness,
                      .icon = cfg.tab_icon, .subtitle = cfg.tab_subtitle && height >= 36.0f,
                      .activity = cfg.tab_activity, .hue = cfg.tab_colors, .new_button = cfg.tab_new_button,
                      .close = cfg.tab_close, .motion = cfg.tab_anim, .number_prefix = "alt+",
                      .bottom = cfg.tab_bottom, .window_controls = !cfg.decorations});
}

}  // namespace hand::view
