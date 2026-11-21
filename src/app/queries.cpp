// hand::app::detail — queries over the model, and small shared mechanics.

#include "detail.hpp"

#include <hand/core/credit.hpp>
#include <hand/core/pty.hpp>

#include <mayag/widget/ssh.hpp>
#include <mayag/widget/terminal/window.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <hand/core/platform.hpp>
#include <thread>

namespace hand::detail {

HostSources host_sources(const Config& c) {
    return HostSources{.store = (std::filesystem::path{config_path()}.parent_path() / "hosts.vibe").string(),
                       .read_known = c.ssh_known_hosts};
}

using namespace mayag;
using namespace mayag::dsl;

/// Where the terminal starts (the strip may sit above it or below it).
float term_top(const Model& m) { return m.cfg.tab_bottom ? 0.0f : m.strip_h(); }
/// Where the terminal starts across (the file pane may dock on the left).
float term_left(const Model& m) { return m.pane.open && m.cfg.files_left ? m.pane.width : 0.0f; }
bool in_pane(const Model& m, float x) {
    if (!m.pane.open) return false;
    return m.cfg.files_left ? x < m.pane.width : x >= m.win.x - m.pane.width;
}
bool in_strip(const Model& m, float y) {
    return m.cfg.tab_bottom ? y >= m.win.y - m.strip_h() : y < m.strip_h();
}

/// A clock for caches that must refresh even while the frame clock is idle.
double mono_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// The foreground process of a session's shell (cached ~0.5 s, on the wall clock).
std::string fg_process(const Session& se) {
    const double now = mono_s();
    if (!se.pty || now - se.proc_at < 0.5) return se.proc;
    se.proc_at = now;
    const pid_t pg = pty_foreground(se.pty->fd());
    if (pg <= 0) return se.proc;
    se.proc = proc_name(pg);
    return se.proc;
}

/// The machine a tab talks to: the host hand launched it on, else the
/// destination of an `ssh ...` you typed at its shell (cached ~1 s).
const std::optional<Host>& remote_of(const Session& se) {
    if (se.link.launched || !se.pty) return se.remote;
    const double now = mono_s();
    if (now - se.remote_at < 1.0) return se.remote;
    se.remote_at = now;
    const std::string p = fg_process(se);
    if (p != "ssh" && p != "mosh" && p != "autossh" && p != "mosh-client") { se.remote.reset(); return se.remote; }
    const auto argv = proc_argv(pty_foreground(se.pty->fd()));
    se.remote = ssh_destination(argv);
    return se.remote;
}

/// A remote's environment: what hosts.vibe says, else what its name suggests.
HostEnv remote_env(const Model& m, const Host& h) {
    if (h.env != HostEnv::none) return h.env;
    if (const Host* b = m.ssh.book.find(h.alias)) return b->env;
    return guess_env(h.alias, h.hostname);
}

std::int64_t unix_now() { return static_cast<std::int64_t>(std::time(nullptr)); }

/// The book as the picker shows it, with live tabs counted per host.
std::vector<SshHost> picker_hosts(const Model& m) {
    std::vector<SshHost> out;
    out.reserve(m.ssh.book.hosts.size());
    for (const auto& h0 : m.ssh.book.hosts) {
        const Host h = effective_host(h0, m.ssh.book);   // what connecting uses (folder defaults filled in)
        SshHost s;
        s.alias = h.alias; s.user = h.user; s.hostname = h.hostname; s.port = h.port;
        s.jump = h.jump; s.identity = h.identity;
        s.folder = h.folder;
        s.inherited = h.inherited;
        s.env = static_cast<SshHost::Env>(h.env);   // same order: none, dev, staging, prod
        s.tags = h.tags; s.pinned = h.pinned; s.uses = h.uses; s.last_used = h.last;
        s.source = h.source == HostSource::known ? "known" : h.source == HostSource::history ? "typed" : "";
        s.forwards = h.forwards;
        s.command = h.command;
        s.editable = h.editable();
        for (std::size_t i = 0; i < m.tab_count(); ++i) {
            const Session& se = m.at(i);
            const auto& r = remote_of(se);
            if (!r || se.link.dropped) continue;
            if (r->alias == h.alias || (r->hostname == h.hostname && r->port == h.port && (r->user.empty() || h.user.empty() || r->user == h.user)))
                ++s.sessions;
        }
        s.connected = s.sessions > 0;
        // each tunnel, live: who listens on its local port right now
        for (const auto& fw : h.forwards) {
            const auto f = mayag::dsl::ssh_forward_parse(fw);
            using T = SshHost::Tunnel;
            T st = T::idle;
            std::string note;
            if (!f) { s.tunnel.push_back(T::idle); s.tunnel_note.emplace_back(); continue; }
            if (f->kind == mayag::dsl::SshForward::Kind::remote) {
                st = s.connected ? T::remote : T::idle;
                note = s.connected ? "listening on the server's port " + std::to_string(f->listen) : "";
            } else if (m.ssh.manager.ports_known) {
                std::string owner;
                bool listening = false;
                for (std::size_t k = 0; k < m.ssh.manager.ports.size(); ++k)
                    if (m.ssh.manager.ports[k] == f->listen) { listening = true; if (owner.empty()) owner = m.ssh.manager.port_owners[k]; }
                const bool ssh_owns = owner.empty() || owner.find("ssh") != std::string::npos;
                if (listening && !ssh_owns) { st = T::conflict; note = "port " + std::to_string(f->listen) + " is in use by " + owner; }
                else if (s.connected) { st = listening ? T::up : T::down; note = listening ? "ssh is listening on " + std::to_string(f->listen)
                                                                              : "connected, but nothing listens on " + std::to_string(f->listen) + " (in use when ssh started?)"; }
                else if (listening) { st = T::up; note = "another ssh holds port " + std::to_string(f->listen); }
            }
            s.tunnel.push_back(st);
            s.tunnel_note.push_back(std::move(note));
        }
        out.push_back(std::move(s));
    }
    return out;
}

/// The picker's rows: indices into m.ssh.book.hosts, best first.
std::vector<int> picker_rank(const Model& m) {
    return rank_hosts(picker_hosts(m), m.ssh.picker.query.text, unix_now());
}

/// The minimap maps the normal screen's history: a full-screen app (the
/// alternate screen: vim, htop, cmatrix) owns the whole window and gets none.
/// The command whose output is under the top of the view, when the sticky header applies.
std::optional<Term::Command> sticky_of(const Model& m) {
    const Session& se = m.tab();
    if (!se.term || !m.cfg.shell_marks || se.term->alt_screen()) return std::nullopt;
    if (!(se.scroll_d > 0.01f || se.term->scroll_offset() > 0)) return std::nullopt;
    return se.term->command_at_top();
}

std::int64_t wall_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

/// "12.3s", "4m05s", "1h02m"
std::string pretty_ms(std::int64_t ms) {
    char b[32];
    const double s = static_cast<double>(ms) / 1000.0;
    if (s < 60.0) std::snprintf(b, sizeof b, "%.1fs", s);
    else if (s < 3600.0) std::snprintf(b, sizeof b, "%dm%02ds", static_cast<int>(s / 60), static_cast<int>(s) % 60);
    else std::snprintf(b, sizeof b, "%dh%02dm", static_cast<int>(s / 3600), static_cast<int>(s / 60) % 60);
    return b;
}

/// A path for a header: $HOME as ~, long ones keep their last two parts.
std::string short_path(std::string p) {
    if (const char* h = std::getenv("HOME"); h && *h && p.rfind(h, 0) == 0) p = "~" + p.substr(std::strlen(h));
    int slashes = 0;
    for (char ch : p) slashes += ch == '/';
    if (p.size() > 28 && slashes > 2) {
        const auto b = p.rfind('/', p.rfind('/') - 1);
        p = "\u2026" + p.substr(b);
    }
    return p;
}


// ── confirmation: what is risky, what is running ─────────────────────────


/// Does this pasted line deserve a second look?
Risk risk_of(const std::string& l) {
    for (unsigned char ch : l)
        if ((ch < 0x20 && ch != '\t') || ch == 0x7F) return {true, "control chars"};
    const auto has = [&](const char* w) { return l.find(w) != std::string::npos; };
    const auto word = [&](const char* w) {   // at the start, or after a separator
        const std::string ws = w;
        for (std::size_t p = l.find(ws); p != std::string::npos; p = l.find(ws, p + 1))
            if ((p == 0 || std::string_view{" ;&|("}.find(l[p - 1]) != std::string_view::npos) &&
                (p + ws.size() == l.size() || l[p + ws.size()] == ' '))
                return true;
        return false;
    };
    if (word("sudo") || word("doas") || word("su")) return {true, "root"};
    if ((word("rm") && (has(" -rf") || has(" -fr") || has(" -r ") || has(" -R"))) || word("shred")) return {true, "deletes"};
    if ((has("curl ") || has("wget ")) && (has("| sh") || has("|sh") || has("| bash") || has("|bash"))) return {true, "runs a download"};
    if (has("mkfs") || has("dd if=") || has("of=/dev/") || has("> /dev/sd") || has("> /dev/nvme")) return {true, "writes a disk"};
    if (has(":(){") || has(":() {")) return {true, "fork bomb"};
    if (has("chmod -R 777") || has("chown -R")) return {true, "permissions"};
    if (word("git") && has(" push") && (has(" --force") || has(" -f"))) return {true, "force push"};
    if (word("shutdown") || word("reboot") || word("poweroff")) return {true, "power"};
    return {};
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : text) {
        if (ch == '\n') { if (!cur.empty() && cur.back() == '\r') cur.pop_back(); out.push_back(cur); cur.clear(); }
        else cur.push_back(ch);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool is_shell(const std::string& p) {
    return p.empty() || p == "fish" || p == "zsh" || p == "bash" || p == "sh" || p == "dash" || p == "nu" || p == "xonsh" || p == "ksh";
}

/// What a tab is busy with ("" = just a prompt), and for how long.
std::string busy_of(const Session& se, std::string* elapsed) {
    if (!se.term || se.link.dropped) return {};
    std::string what;
    for (const auto& c : se.term->commands()) {   // shell integration knows exactly
        if (c.ran && !c.finished) {
            what = se.term->command_text(c);
            if (elapsed && c.start_ms > 0) *elapsed = pretty_ms(std::max<std::int64_t>(wall_ms() - c.start_ms, 0));
        }
    }
    const std::string proc = fg_process(se);
    if (what.empty() && !is_shell(proc)) what = proc;   // without integration: the foreground program
    return what;
}

bool minimap_on(const Model& m) { return m.cfg.minimap && m.tab().term && !m.tab().term->alt_screen(); }

float minimap_w(const Model& m) { return minimap_on(m) ? std::max(m.minimap.width, kMinimapRest) : 0.0f; }

bool corner_buttons(const Model& m) { return !m.cfg.decorations && m.strip.t <= 0.0f; }

CornerControlsProps corner_props(const Model& m) {
    const float pane = m.pane.open && !m.cfg.files_left ? m.pane.width : 0.0f;
    return {.width = m.win.x, .right_inset = minimap_w(m) + pane};
}

bool in_corner(const Model& m, Vec2 p) {
    if (!corner_buttons(m)) return false;
    const auto cp = corner_props(m);
    const Vec2 sz = corner_controls_size(cp);
    const float right = std::round(cp.width - cp.right_inset);
    return p.y >= 0.0f && p.y < sz.y && p.x >= right - sz.x && p.x < right;
}

/// The live tail shows while you read history and the live prompt is off screen.
bool tail_wanted(const Model& m) {
    const Session& se = m.tab();
    if (!se.term || m.cfg.live_tail < 1.0f || se.term->alt_screen()) return false;
    const auto cur = se.term->cursor();
    return se.term->scroll_offset() > 0 && cur.row + se.term->scroll_offset() >= se.term->rows();
}

/// The pty reader: one background stream per shell (see detail.hpp).
void read_pty(Sink<App::Msg> out, std::stop_token st, mayag::CoOwned<Pty> pty, SessionId sid) {
    // This reader co-owns the pty: the fd stays open until it returns, even
    // if the tab is closed under it (the stream is then stopped, and the
    // last owner out closes the fd).
    const int fd = pty->fd();
    // Pipelined: while one chunk is with the UI the reader keeps draining the
    // pty into the next, so the producer never stalls on the (tiny, ~1 KB on
    // macOS) pty buffer; only when that next chunk is full (kCap) does it
    // stop reading -- and then the producer blocks, which is the throttle.
    constexpr std::size_t kCap = 4u << 20;
    // Each Output is at most a frame's worth of parsing (~6 ms at toe's
    // ~80 MB/s): a bigger one would starve the frame it lands in, and while
    // that frame is late the reader piles up an even bigger next one -- a
    // feedback loop that ran a `yes` flood down to 8 fps.
    constexpr std::size_t kSlice = 512u << 10;
    auto credit = mayag::CoOwned<OutputCredit>::make();   // this reader's, shared with the UI through each Output
    std::string buf;
    bool alive = true;
    const auto ship = [&] {
        std::string chunk;
        if (buf.size() <= kSlice) chunk = std::exchange(buf, std::string{});
        else {
            // cut on a UTF-8 boundary, so no slice ends mid-character
            std::size_t n = kSlice;
            while (n > kSlice - 4 && (static_cast<unsigned char>(buf[n]) & 0xC0) == 0x80) --n;
            chunk.assign(buf, 0, n);
            buf.erase(0, n);
        }
        credit->spend();
        if (!out.send(App::Output{std::move(chunk), sid, credit})) credit->refund();
    };
    while (!st.stop_requested()) {
        const bool busy = !credit->free();
        if (!busy && !buf.empty()) { ship(); continue; }
        if (!alive) {
            if (busy) {   // the last output lands before the exit
                (void)credit->wait(-1, 50);
                credit->settle();
                continue;
            }
            int code = -1;
            for (int i = 0; i < 20 && code < 0; ++i) {   // bounded: ≤ ~200 ms
                code = pty->reap();
                if (code < 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            out.send(App::Exited{std::max(code, 0), sid});
            return;
        }
        const bool room = buf.size() < kCap;
        // 50 ms: stop is always prompt. pty = -1 while back-pressured, so a
        // stalled reader waits only on the credit, never on the pty.
        const plat::Ready r = credit->wait(room ? pty_handle(fd) : -1, 50);
        if (r.error) { alive = false; continue; }
        if (r.woke) credit->settle();
        if (room && r.data) alive = pty_read(fd, buf, 0);
        // A quiet pty: ask whether the child is gone. On POSIX a dead child
        // breaks the pty (r.data, then EOF above); a ConPTY pipe stays open
        // until the pseudoconsole is closed, so only pty_read can tell, and
        // without this `exit` left the tab (and the window) open forever.
        else if (room && !r.woke) alive = pty_read(fd, buf, 0);
    }
}

/// The viewport row under window y, through a smooth-scroll glide (the grid is
/// drawn up to a row lower mid-glide). May be -1: the row just above the view.
/// How far below its row the grid is drawn (0..1): a scroll glide between rows
/// plus the glide of a freshly arrived line at the live prompt.
float draw_frac(const Model& m) {
    const Session& se = m.tab();
    const float scroll = m.cfg.smooth_scroll ? se.scroll_d - static_cast<float>(se.scroll_applied) : 0.0f;
    return std::clamp(scroll + se.out_shift, 0.0f, 0.999f);
}

int view_row(const Model& m, float y) {
    const float frac = draw_frac(m);
    const float r = (y - m.metrics.origin_y) / std::max(m.metrics.cell_h, 1.0f) - (frac > 0.001f ? frac : 0.0f);
    const int lo = frac > 0.001f ? -1 : 0;
    return std::clamp(static_cast<int>(std::floor(r)), lo, std::max(m.tab().term ? m.tab().term->rows() - 1 : 0, 0));
}

/// Selecting into the top/bottom margin scrolls; the further out, the faster.
float edge_speed(const Model& m, float y) {
    const float ch = std::max(m.metrics.cell_h, 1.0f);
    const float top = m.metrics.origin_y + ch * 0.5f, bottom = m.size.y - m.metrics.origin_y - ch * 0.5f;
    if (y < top) return 6.0f + (top - y) / ch * 18.0f;
    if (y > bottom) return -(6.0f + (y - bottom) / ch * 18.0f);
    return 0.0f;
}

int cell_of(float v, float origin, float size, int max) {
    return std::clamp(static_cast<int>(std::floor((v - origin) / std::max(size, 1.0f))), 0, std::max(max - 1, 0));
}

FontFamily family_of(const Config& c) {
    return font_names().empty() ? FontFamily::mono
                                : custom_family(static_cast<std::size_t>(std::clamp(c.font_family, 0, static_cast<int>(font_names().size()) - 1)));
}


/// Run ssh to `h` as `se`'s program (its terminal keeps whatever it shows).
void start_ssh(Model& m, Session& se, const Host& h) {
    se.link.connected_at = m.time;
    se.link.dropped = false;
    if (m.cfg.demo) return;
    se.pty = PtyRef::make();
    const auto argv = ssh_argv(h, m.ssh.book, {.multiplex = m.cfg.ssh_multiplex, .keepalive = m.cfg.ssh_keepalive});
    if (!se.pty->spawn(m.metrics.fit_cols, m.metrics.fit_rows, "ssh", {},
                       std::vector<std::string>{argv.begin() + 1, argv.end()}, {})) {
        (void)se.term->feed("\x1b[31mhand: could not start ssh\x1b[0m\r\n");
        se.pty.reset();
    }
}

/// Bring tab `i` to the front: it is read, and its cursor appears where it is.
void activate(Model& m, std::size_t i) {
    if (i == m.active() || i >= m.tab_count()) return;
    m.tabs.select(i);
    m.tab().unread = m.tab().bell = false;
    m.tab().grid_key = 0;
    m.tab().cur_known = false;              // the cursor appears where it is, no flight across tabs
    m.tab().fx.ps.clear();
    m.minimap.hover_y = -1.0f;
    m.cursor_on = true;
    m.tab().cur_a = 1.0f;
}

std::optional<App::Msg> send(std::string bytes) {
    if (bytes.empty()) return std::nullopt;
    return App::Typed{std::move(bytes)};
}


}  // namespace hand::detail
