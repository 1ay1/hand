// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand-agent — a headless, GPU-free terminal an AI agent can drive.
//
// This is the "terminal for AI" surface: it spawns a real PTY (so vim/htop/less
// behave exactly as they do for a human), runs toe's VT engine with NO renderer
// and NO window, and speaks a line-delimited JSON protocol on stdio. An agent
// (or an MCP shim in front of it) sends one JSON request per line and gets one
// JSON reply per line:
//
//   {"op":"snapshot"}                     -> clean text of the visible screen
//   {"op":"blocks","last":N}              -> OSC 133 command blocks as JSON
//   {"op":"send","text":"ls -la\r"}       -> type text
//   {"op":"send","keys":"<C-c>"}          -> vim-notation special keys
//   {"op":"wait","for":"idle","ms":2000}  -> wait until output settles
//   {"op":"wait","for":"pattern","re":"\\$ ","ms":5000}
//   {"op":"resize","cols":120,"rows":40}
//   {"op":"key","name":"Enter"}
//
// Why not the agent's built-in shell tool? Because that runs against a pipe,
// fire-and-forget: no TTY, no interactivity, and it dumps raw ANSI (mostly
// noise). This gives the agent a *settled, structured, token-frugal* view.
//
// The protocol is deliberately tiny and dependency-free (a hand-rolled JSON
// reader/writer) so it drops into any harness. See docs/AI_TERMINAL.md.

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <poll.h>
#include <time.h>
#include <unistd.h>

#include "hand/platform/posix_pty.hpp"
#include "toe/pty/pty.hpp"
#include "toe/term/redact.hpp"
#include "toe/term/update.hpp"

namespace {

using toe::term::Model;

std::int64_t now_ms() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1'000'000;
}

// ─────────────────────────── minimal JSON ───────────────────────────────────
// A tiny reader for the flat request objects this protocol uses (string / int /
// bool values, no nesting) and a small writer. Not a general JSON library — just
// enough to keep the driver dependency-free.

std::string json_escape(std::string_view s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o.push_back(static_cast<char>(c));
        }
    }
    return o;
}

// Pull the string value of "key" out of a flat JSON object line. Handles the
// escapes we emit. Returns nullopt if absent.
std::optional<std::string> json_str(std::string_view j, std::string_view key) {
    std::string needle = "\"" + std::string{key} + "\"";
    auto k = j.find(needle);
    if (k == std::string_view::npos) return std::nullopt;
    auto c = j.find(':', k + needle.size());
    if (c == std::string_view::npos) return std::nullopt;
    auto q = j.find('"', c + 1);
    if (q == std::string_view::npos) return std::nullopt;
    std::string out;
    for (std::size_t i = q + 1; i < j.size(); ++i) {
        char ch = j[i];
        if (ch == '\\' && i + 1 < j.size()) {
            char n = j[++i];
            switch (n) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case 'u': {
                if (i + 4 < j.size()) {
                    int v = (int)std::strtol(std::string{j.substr(i + 1, 4)}.c_str(), nullptr, 16);
                    i += 4;
                    if (v < 0x80) out.push_back(static_cast<char>(v));
                    else if (v < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (v >> 6)));
                        out.push_back(static_cast<char>(0x80 | (v & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (v >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((v >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (v & 0x3F)));
                    }
                }
                break;
            }
            default: out.push_back(n);
            }
        } else if (ch == '"') {
            return out;
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::optional<long> json_int(std::string_view j, std::string_view key) {
    std::string needle = "\"" + std::string{key} + "\"";
    auto k = j.find(needle);
    if (k == std::string_view::npos) return std::nullopt;
    auto c = j.find(':', k + needle.size());
    if (c == std::string_view::npos) return std::nullopt;
    std::size_t i = c + 1;
    while (i < j.size() && (j[i] == ' ' || j[i] == '\t')) ++i;
    bool neg = false;
    if (i < j.size() && j[i] == '-') { neg = true; ++i; }
    if (i >= j.size() || !std::isdigit((unsigned char)j[i])) return std::nullopt;
    long v = 0;
    for (; i < j.size() && std::isdigit((unsigned char)j[i]); ++i) v = v * 10 + (j[i] - '0');
    return neg ? -v : v;
}

// ─────────────────────── vim-notation key encoding ──────────────────────────
// Translate <C-c>, <CR>, <Esc>, <Tab>, <Up>… into the bytes a PTY expects.
// Anything not in <...> is passed through literally.
std::string encode_keys(std::string_view s, bool app_cursor) {
    auto special = [&](std::string_view name) -> std::string {
        // Ctrl chord: <C-x>
        if (name.size() == 3 && (name[0] == 'C' || name[0] == 'c') && name[1] == '-') {
            char c = name[2];
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 1);
            else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 1);
            else if (c == '[') c = 27; else if (c == ']') c = 29;
            return std::string(1, c);
        }
        std::string n{name};
        for (auto &ch : n) ch = static_cast<char>(std::tolower((unsigned char)ch));
        const char *ss = app_cursor ? "\x1bO" : "\x1b[";
        if (n == "cr" || n == "enter" || n == "return") return "\r";
        if (n == "lf") return "\n";
        if (n == "esc") return "\x1b";
        if (n == "tab") return "\t";
        if (n == "bs" || n == "backspace") return "\x7f";
        if (n == "space") return " ";
        if (n == "up") return std::string(ss) + "A";
        if (n == "down") return std::string(ss) + "B";
        if (n == "right") return std::string(ss) + "C";
        if (n == "left") return std::string(ss) + "D";
        if (n == "home") return std::string(ss) + "H";
        if (n == "end") return std::string(ss) + "F";
        if (n == "pageup") return "\x1b[5~";
        if (n == "pagedown") return "\x1b[6~";
        if (n == "delete" || n == "del") return "\x1b[3~";
        if (n == "insert") return "\x1b[2~";
        return {}; // unknown -> drop
    };
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '<') {
            auto close = s.find('>', i);
            if (close != std::string_view::npos) {
                out += special(s.substr(i + 1, close - i - 1));
                i = close;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

// ─────────────────────────── the driver core ────────────────────────────────

class Driver {
public:
    Driver(toe::Pty pty, Model model) : pty_(std::move(pty)), model_(std::move(model)) {}

    void set_redactor(bool on) { redactor_.set_enabled(on); }
    [[nodiscard]] std::string redact(std::string_view s) const { return redactor_.apply(s); }

    // Drain whatever the child has written, folding it into the model. Returns
    // false when the child has hung up. Non-blocking.
    bool drain() {
        for (;;) {
            bool cont = std::visit(
                [&](auto &&r) -> bool {
                    using R = std::decay_t<decltype(r)>;
                    if constexpr (std::is_same_v<R, toe::pty::Data>) {
                        auto cmds = toe::term::feed_output(
                            model_, std::string_view{r.bytes.data(), r.bytes.size()});
                        write_replies(cmds);
                        return true;
                    } else if constexpr (std::is_same_v<R, toe::pty::WouldBlock>) {
                        return false;
                    } else {
                        hung_up_ = true;
                        return false;
                    }
                },
                pty_.read());
            if (!cont) break;
        }
        return !hung_up_;
    }

    // Block until the child has data or `deadline_ms` (wall clock) passes.
    // Returns true if data arrived. deadline<=0 -> poll immediately.
    bool wait_readable(std::int64_t deadline_ms) {
        std::int64_t timeout = deadline_ms - now_ms();
        if (timeout < 0) timeout = 0;
        struct pollfd pfd{pty_.fd(), POLLIN, 0};
        int n = ::poll(&pfd, 1, static_cast<int>(timeout > 1000 ? 1000 : timeout));
        return n > 0 && (pfd.revents & POLLIN);
    }

    // Wait until the child stops producing output for `quiet_ms`, or `cap_ms`
    // total elapses, or it hangs up. This is the "screen settled" primitive.
    void wait_idle(std::int64_t quiet_ms, std::int64_t cap_ms) {
        const std::int64_t hard = now_ms() + cap_ms;
        std::int64_t last_activity = now_ms();
        while (now_ms() < hard && !hung_up_) {
            // Respect DEC 2026: while an app is mid-frame, it hasn't settled.
            const bool settled = model_.screen.sync_active() == false &&
                                 (now_ms() - last_activity) >= quiet_ms;
            if (settled) break;
            if (wait_readable(std::min(hard, now_ms() + quiet_ms))) {
                if (!drain()) break;
                last_activity = now_ms();
            }
        }
    }

    // Wait until `snapshot` matches `substr`, or cap elapses.
    bool wait_pattern(const std::string &substr, std::int64_t cap_ms) {
        const std::int64_t hard = now_ms() + cap_ms;
        while (now_ms() < hard && !hung_up_) {
            if (snapshot().find(substr) != std::string::npos) return true;
            if (wait_readable(std::min(hard, now_ms() + 100))) {
                if (!drain()) break;
            }
        }
        return snapshot().find(substr) != std::string::npos;
    }

    void send(std::string_view bytes) { (void)pty_.write(bytes); drain(); }

    void resize(int cols, int rows) {
        if (cols <= 0 || rows <= 0) return;
        model_.screen.resize(toe::Extent{cols, rows});
        (void)pty_.resize(toe::Extent{cols, rows});
    }

    [[nodiscard]] std::string snapshot() const {
        const auto &scr = model_.screen;
        const toe::Extent g = scr.size();
        const std::int64_t total = scr.total_rows();
        const std::int64_t top = total - g.rows < 0 ? 0 : total - g.rows;
        return scr.text_between_abs(top, total);
    }

    [[nodiscard]] bool app_cursor() const { return model_.screen.app_cursor_keys(); }
    [[nodiscard]] bool hung_up() const { return hung_up_; }
    [[nodiscard]] const toe::term::CommandLog &commands() const { return model_.commands; }
    [[nodiscard]] const Model &model() const { return model_; }

    // Resolve a block's command/output text (mirrors Session::commands()).
    std::pair<std::string, std::string> block_text(const toe::term::CommandBlock &b) const {
        const auto &scr = model_.screen;
        std::string cmd, out;
        if (b.input_row >= 0) {
            std::int64_t e = (b.output_row > b.input_row) ? b.output_row : b.input_row + 1;
            if (b.output_row < 0) e = scr.total_rows();
            if (e <= b.input_row) e = b.input_row + 1;
            cmd = scr.text_between_abs(b.input_row, e, b.input_col);
        }
        if (b.output_row >= 0) {
            std::int64_t s = b.output_row;
            if (b.input_row >= 0 && s <= b.input_row) s = b.input_row + 1;
            std::int64_t e = (b.end_row >= 0) ? b.end_row : scr.total_rows();
            out = scr.text_between_abs(s, e);
        }
        return {std::move(cmd), std::move(out)};
    }

private:
    void write_replies(const toe::Cmds &cmds) {
        std::string batch;
        for (const auto &c : cmds)
            if (const auto *w = std::get_if<toe::WriteChild>(&c)) batch += w->bytes;
        if (!batch.empty()) (void)pty_.write(batch);
    }

    toe::Pty pty_;
    Model model_;
    toe::term::Redactor redactor_{};
    bool hung_up_ = false;
};

void reply_ok(const std::string &body) { std::printf("{\"ok\":true,%s}\n", body.c_str()); std::fflush(stdout); }
void reply_err(std::string_view msg) {
    std::printf("{\"ok\":false,\"error\":\"%s\"}\n", json_escape(msg).c_str());
    std::fflush(stdout);
}

std::string blocks_json(const Driver &d, long last) {
    const auto &log = d.commands();
    const auto &blocks = log.blocks();
    std::size_t start = 0;
    if (last > 0 && static_cast<std::size_t>(last) < blocks.size())
        start = blocks.size() - static_cast<std::size_t>(last);
    std::string j = "\"blocks\":[";
    bool first = true;
    for (std::size_t i = start; i < blocks.size(); ++i) {
        const auto &b = blocks[i];
        auto [cmd, out] = d.block_text(b);
        if (!first) j += ",";
        first = false;
        j += "{\"id\":" + std::to_string(b.id);
        j += ",\"command\":\"" + json_escape(d.redact(cmd)) + "\"";
        j += ",\"output\":\"" + json_escape(d.redact(out)) + "\"";
        j += ",\"cwd\":\"" + json_escape(b.cwd) + "\"";
        j += b.exit_code.has_value() ? ",\"exitCode\":" + std::to_string(*b.exit_code)
                                     : std::string(",\"exitCode\":null");
        j += ",\"finished\":" + std::string(b.finished() ? "true" : "false");
        j += ",\"durationMs\":" + std::to_string(b.duration_ms());
        j += "}";
    }
    j += "]";
    return j;
}

} // namespace

int main(int argc, char **argv) {
    // Args: [--cols N] [--rows N] [-- CMD ARGS...]. Default 80x24 running $SHELL.
    int cols = 80, rows = 24;
    bool redact = false;
    hand::SpawnCommand cmd;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--cols" && i + 1 < argc) cols = std::atoi(argv[++i]);
        else if (a == "--rows" && i + 1 < argc) rows = std::atoi(argv[++i]);
        else if (a == "--redact") redact = true;
        else if (a == "--") { for (int j = i + 1; j < argc; ++j) cmd.argv.emplace_back(argv[j]); break; }
    }

    auto adopted = hand::spawn_pty(cmd);
    if (!adopted) { reply_err(adopted.error().message); return 1; }
    auto pty = toe::Pty::adopt(*adopted);
    if (!pty) { reply_err(pty.error().message); return 1; }
    (void)pty->resize(toe::Extent{cols, rows});

    toe::Config cfg;
    Model model{cfg, toe::Extent{cols, rows}};
    Driver drv{std::move(*pty), std::move(model)};
    drv.set_redactor(redact);

    reply_ok("\"ready\":true,\"cols\":" + std::to_string(cols) + ",\"rows\":" + std::to_string(rows));

    std::string line;
    int ch;
    while ((ch = std::getchar()) != EOF) {
        if (ch != '\n') { line.push_back(static_cast<char>(ch)); continue; }
        std::string_view j = line;
        auto op = json_str(j, "op").value_or("");

        // Always drain pending output before answering a read.
        drv.drain();

        if (op == "snapshot") {
            reply_ok("\"text\":\"" + json_escape(drv.redact(drv.snapshot())) + "\"");
        } else if (op == "blocks") {
            long last = json_int(j, "last").value_or(0);
            reply_ok(blocks_json(drv, last));
        } else if (op == "send") {
            if (auto t = json_str(j, "text")) drv.send(*t);
            else if (auto k = json_str(j, "keys")) drv.send(encode_keys(*k, drv.app_cursor()));
            reply_ok("\"sent\":true");
        } else if (op == "key") {
            auto name = json_str(j, "name").value_or("");
            drv.send(encode_keys("<" + name + ">", drv.app_cursor()));
            reply_ok("\"sent\":true");
        } else if (op == "wait") {
            auto what = json_str(j, "for").value_or("idle");
            long ms = json_int(j, "ms").value_or(2000);
            if (what == "pattern") {
                auto re = json_str(j, "re").value_or("");
                bool hit = drv.wait_pattern(re, ms);
                reply_ok(std::string("\"matched\":") + (hit ? "true" : "false"));
            } else { // idle
                long quiet = json_int(j, "quiet").value_or(120);
                drv.wait_idle(quiet, ms);
                reply_ok("\"idle\":true");
            }
        } else if (op == "resize") {
            drv.resize((int)json_int(j, "cols").value_or(cols),
                       (int)json_int(j, "rows").value_or(rows));
            reply_ok("\"resized\":true");
        } else if (op == "close") {
            reply_ok("\"bye\":true");
            break;
        } else {
            reply_err("unknown op: " + op);
        }

        if (drv.hung_up()) { reply_ok("\"exited\":true"); break; }
        line.clear();
    }
    return 0;
}
