// hand::fx — the ptys: start a program, write to it, resize it, and ask what
// runs in it. Every pty syscall hand makes from the app is here.

#include "fx.hpp"

#include <hand/core/shell.hpp>

#include <algorithm>

namespace hand::fx {

std::string resolved_shell(std::string shell) { return Pty::resolve_shell(std::move(shell)); }

App::Msg spawn(SessionId sid, Launch l) {
    PtySpec spec = std::move(l.spec);
    bool reflow_aware = false;
    if (l.ssh) {
        // ssh_argv may create ssh's control-socket directory: built here, not in update
        const auto argv = ssh_argv(l.ssh->host, l.ssh->book, l.ssh->flags);
        spec.shell = "ssh";
        spec.args.assign(argv.begin() + 1, argv.end());
    } else if (l.integrate) {
        spec.shell = Pty::resolve_shell(spec.shell);
        ShellLaunch si = shell_integration(spec.shell);   // writes the scripts beside $XDG_RUNTIME_DIR
        spec.env.insert(spec.env.end(), si.env.begin(), si.env.end());
        spec.args.insert(spec.args.end(), si.args.begin(), si.args.end());
        reflow_aware = si.reflow_aware;
    }
    PtyRef p = PtyRef::make(spec);
    App::Spawned s{.sid = sid, .reflow_aware = reflow_aware};
    if (p) s.pty = p.share();
    return App::Msg{std::move(s)};
}

void pty_write(mayag::CoOwned<Pty> pty, std::string bytes) { pty->write(bytes); }

void pty_resize(mayag::CoOwned<Pty> pty, int cols, int rows, int px_w, int px_h) {
    pty->resize(cols, rows, px_w, px_h);
}

std::optional<App::Msg> probe(std::vector<ProbeTab> tabs) {
    App::Probed out;
    bool changed = false;
    for (auto& t : tabs) {
        msg::TabProc tp{.sid = t.sid};
        if (t.pty) {
            const pid_t pg = pty_foreground((*t.pty)->fd());
            tp.proc = pg > 0 ? proc_name(pg) : t.was;
            // an `ssh host` typed at the shell: who it talks to
            if (t.watch_ssh && (tp.proc == "ssh" || tp.proc == "mosh" || tp.proc == "autossh" || tp.proc == "mosh-client"))
                tp.remote = ssh_destination(proc_argv(pg));
        }
        changed = changed || tp.proc != t.was || tp.remote.has_value() != t.had_remote || tp.remote.has_value();
        out.tabs.push_back(std::move(tp));
    }
    if (!changed) return std::nullopt;   // the common case: nothing to fold
    return App::Msg{std::move(out)};
}

}  // namespace hand::fx
