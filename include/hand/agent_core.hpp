// SPDX-License-Identifier: LGPL-2.0-or-later
//
// agent_core.hpp — the shared "terminal for AI" engine behind hand-agent (the
// line-JSON stdio frontend) and hand-agent-mcp (the MCP server frontend).
//
// It owns a PTY + a headless toe::term::Model (no renderer, no window) and
// exposes the read/drive primitives an agent needs: a settled screen snapshot,
// OSC 133 command blocks, key/text input (vim-notation aware), and deterministic
// waits. Everything here is dependency-free (a hand-rolled JSON in/out + a tiny
// key encoder) so both frontends are single translation units.

#ifndef HAND_AGENT_CORE_HPP
#define HAND_AGENT_CORE_HPP

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <poll.h>
#include <time.h>

#include "hand/platform/posix_pty.hpp"
#include "toe/pty/pty.hpp"
#include "toe/term/redact.hpp"
#include "toe/term/update.hpp"

namespace hand::agent {

using Model = toe::term::Model;

inline std::int64_t now_ms() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1'000'000;
}

// ─────────────────────────── minimal JSON ───────────────────────────────────

inline std::string json_escape(std::string_view s) {
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

// Pull the string value of "key" out of a flat JSON object. Handles our escapes.
inline std::optional<std::string> json_str(std::string_view j, std::string_view key) {
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
            case '/': out.push_back('/'); break;
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

inline std::optional<long> json_int(std::string_view j, std::string_view key) {
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

inline std::string encode_keys(std::string_view s, bool app_cursor) {
    auto special = [&](std::string_view name) -> std::string {
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
        return {};
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

    // Spawn a fresh session: forkpty the command, adopt it into a headless
    // model. Returns nullptr on failure (message in `err`).
    static std::unique_ptr<Driver> spawn(const hand::SpawnCommand &cmd, int cols, int rows,
                                         bool redact, std::string &err) {
        auto adopted = hand::spawn_pty(cmd);
        if (!adopted) { err = adopted.error().message; return nullptr; }
        auto pty = toe::Pty::adopt(*adopted);
        if (!pty) { err = pty.error().message; return nullptr; }
        (void)pty->resize(toe::Extent{cols, rows});
        toe::Config cfg;
        Model model{cfg, toe::Extent{cols, rows}};
        auto d = std::make_unique<Driver>(std::move(*pty), std::move(model));
        d->redactor_.set_enabled(redact);
        d->cols_ = cols;
        d->rows_ = rows;
        return d;
    }

    void set_redactor(bool on) { redactor_.set_enabled(on); }
    [[nodiscard]] std::string redact(std::string_view s) const { return redactor_.apply(s); }
    [[nodiscard]] int cols() const { return cols_; }
    [[nodiscard]] int rows() const { return rows_; }

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

    bool wait_readable(std::int64_t deadline_ms) {
        std::int64_t timeout = deadline_ms - now_ms();
        if (timeout < 0) timeout = 0;
        struct pollfd pfd{pty_.fd(), POLLIN, 0};
        int n = ::poll(&pfd, 1, static_cast<int>(timeout > 1000 ? 1000 : timeout));
        return n > 0 && (pfd.revents & POLLIN);
    }

    void wait_idle(std::int64_t quiet_ms, std::int64_t cap_ms) {
        const std::int64_t hard = now_ms() + cap_ms;
        std::int64_t last_activity = now_ms();
        while (now_ms() < hard && !hung_up_) {
            const bool settled = !model_.screen.sync_active() &&
                                 (now_ms() - last_activity) >= quiet_ms;
            if (settled) break;
            if (wait_readable(std::min(hard, now_ms() + quiet_ms))) {
                if (!drain()) break;
                last_activity = now_ms();
            }
        }
    }

    bool wait_pattern(const std::string &substr, std::int64_t cap_ms) {
        const std::int64_t hard = now_ms() + cap_ms;
        while (now_ms() < hard && !hung_up_) {
            if (snapshot().find(substr) != std::string::npos) return true;
            if (wait_readable(std::min(hard, now_ms() + 100)))
                if (!drain()) break;
        }
        return snapshot().find(substr) != std::string::npos;
    }

    void send(std::string_view bytes) { (void)pty_.write(bytes); drain(); }

    void resize(int cols, int rows) {
        if (cols <= 0 || rows <= 0) return;
        cols_ = cols; rows_ = rows;
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

    // One command block as a JSON object (redaction applied to text fields).
    std::string block_json(const toe::term::CommandBlock &b) const {
        auto [cmd, out] = block_text(b);
        std::string j = "{\"id\":" + std::to_string(b.id);
        j += ",\"command\":\"" + json_escape(redact(cmd)) + "\"";
        j += ",\"output\":\"" + json_escape(redact(out)) + "\"";
        j += ",\"cwd\":\"" + json_escape(b.cwd) + "\"";
        j += b.exit_code.has_value() ? ",\"exitCode\":" + std::to_string(*b.exit_code)
                                     : std::string(",\"exitCode\":null");
        j += ",\"finished\":" + std::string(b.finished() ? "true" : "false");
        j += ",\"durationMs\":" + std::to_string(b.duration_ms());
        j += "}";
        return j;
    }

    // The newest `last` command blocks (0 = all) as a JSON array.
    std::string blocks_json(long last) const {
        const auto &blocks = commands().blocks();
        std::size_t start = 0;
        if (last > 0 && static_cast<std::size_t>(last) < blocks.size())
            start = blocks.size() - static_cast<std::size_t>(last);
        std::string j = "[";
        for (std::size_t i = start; i < blocks.size(); ++i) {
            if (i != start) j += ",";
            j += block_json(blocks[i]);
        }
        j += "]";
        return j;
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
    int cols_ = 80, rows_ = 24;
    bool hung_up_ = false;
};

} // namespace hand::agent

#endif // HAND_AGENT_CORE_HPP
