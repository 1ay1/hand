// SPDX-License-Identifier: LGPL-2.0-or-later
//
// TabModel — the BRAIN of hand's "Activity Tabs": pure, windowing-free logic
// that turns each terminal's OSC 133 shell-integration signal into a live,
// self-describing tab. No other terminal labels its tabs from what's actually
// running; this is why hand can.
//
// A tab is never manually named. Its label + status are DERIVED every frame
// from a TabSignal snapshot the host reads off toe::Session:
//
//   - RUNNING a command  -> spinner + "<cwd> ▸ <cmd>  <elapsed>"
//   - last command OK     -> "✓ <cwd> ▸ <cmd>"       (or the window title)
//   - last command FAILED -> "✗ <cmd> (<exit>)"       in the fail colour
//   - idle at a prompt    -> just "<cwd>"
//
// The killer feature is ATTENTION: when a command finishes in a tab you're NOT
// looking at, that tab latches an alert (done_ok / done_fail) that the chrome
// pulses until you visit it — an ambient, in-place "your build is done" with no
// notification daemon. Attention is cleared when the tab becomes active.
//
// This header is deliberately free of GL/windowing/toe-Session so it can be
// unit-tested against synthetic signals (see tests/tab_model_test.cpp).

#ifndef HAND_TAB_MODEL_HPP
#define HAND_TAB_MODEL_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hand {

// A per-frame snapshot of one terminal's shell-integration state, read by the
// host from toe::Session (current_command / last_command / working_dir /
// window_title / generation). Pure data — the model never touches the engine.
struct TabSignal {
    bool alive = true;                       // false once the child has exited
    std::string cwd{};                       // OSC 7 working dir (may be empty)
    std::string title{};                     // OSC 0/2 window title (fallback label)
    // The command currently executing (nullopt when idle at a prompt).
    bool running = false;
    std::string running_cmd{};
    std::int64_t running_ms = 0;             // elapsed wall-clock of the running cmd
    // The most recently finished command (nullopt until one completes).
    bool have_last = false;
    std::string last_cmd{};
    std::optional<int> last_exit{};          // nullopt => unknown
    // Monotone activity counter (Session::generation): any output bumps it.
    std::uint64_t generation = 0;
};

// The status a tab is in, for glyph + colour selection in the chrome.
enum class TabStatus {
    Idle,     // at a prompt, nothing running, no unseen result
    Running,  // a command is executing (show spinner + elapsed)
    Ok,       // last command in view succeeded
    Failed,   // last command in view failed (non-zero exit)
    Dead,     // the child exited
};

// Attention latched on a background tab whose command just finished. Pulses in
// the chrome until the tab is visited. None while the tab is active or nothing
// has completed unseen.
enum class TabAttention { None, DoneOk, DoneFail };

class TabModel {
public:
    // Fold this frame's signal in. `active` is whether this tab is the one the
    // user is currently viewing. Returns nothing; read the accessors after.
    void update(const TabSignal &s, bool active) {
        status_ = derive_status(s);

        // Attention: latch when a command FINISHES while we're NOT the active
        // tab. We detect a finish by the last-command identity changing (its
        // text or exit code) since we last saw it.
        const std::string fingerprint =
            s.have_last ? (s.last_cmd + "\x1f" + (s.last_exit ? std::to_string(*s.last_exit) : "?"))
                        : std::string{};
        if (active) {
            attention_ = TabAttention::None; // visiting clears the alert
            last_fingerprint_ = fingerprint; // and marks the current result seen
        } else if (s.have_last && fingerprint != last_fingerprint_) {
            // A NEW completed command we haven't shown the user yet.
            attention_ = (s.last_exit && *s.last_exit != 0) ? TabAttention::DoneFail
                                                            : TabAttention::DoneOk;
            last_fingerprint_ = fingerprint;
        }

        // Unseen output: any generation bump while inactive lights a dot; going
        // active clears it.
        if (active) {
            seen_generation_ = s.generation;
            unseen_ = false;
        } else if (s.generation != seen_generation_) {
            unseen_ = true;
        }

        label_ = derive_label(s);
    }

    [[nodiscard]] TabStatus status() const noexcept { return status_; }
    [[nodiscard]] TabAttention attention() const noexcept { return attention_; }
    [[nodiscard]] bool unseen() const noexcept { return unseen_; }
    [[nodiscard]] const std::string &label() const noexcept { return label_; }

    // The status glyph for the chrome. Spinner rotates through braille frames by
    // `frame` (host passes a time-derived counter) while running.
    [[nodiscard]] char32_t glyph(std::uint32_t frame) const noexcept {
        switch (status_) {
        case TabStatus::Running: {
            static constexpr char32_t kSpin[] = {U'\u280B', U'\u2819', U'\u2839', U'\u2838',
                                                 U'\u283C', U'\u2834', U'\u2826', U'\u2827',
                                                 U'\u2807', U'\u280F'};
            return kSpin[frame % 10];
        }
        case TabStatus::Ok: return U'\u2713';     // ✓
        case TabStatus::Failed: return U'\u2717';  // ✗
        case TabStatus::Dead: return U'\u2205';    // ∅ (empty set)
        case TabStatus::Idle:
        default: return U'\u25CF';                 // ● prompt dot
        }
    }

private:
    static TabStatus derive_status(const TabSignal &s) {
        if (!s.alive) return TabStatus::Dead;
        if (s.running) return TabStatus::Running;
        if (s.have_last) return (s.last_exit && *s.last_exit != 0) ? TabStatus::Failed
                                                                   : TabStatus::Ok;
        return TabStatus::Idle;
    }

    // Basename of a path (the last non-empty segment), for a compact cwd label.
    static std::string basename(std::string_view path) {
        if (path.empty()) return {};
        // Collapse $HOME to ~ is done by the caller if it wants; here just trim.
        while (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
        const auto slash = path.find_last_of('/');
        std::string_view base = (slash == std::string_view::npos) ? path : path.substr(slash + 1);
        return base.empty() ? std::string{path} : std::string{base};
    }

    // First token of a command line (the program name), for a compact label.
    static std::string program(std::string_view cmd) {
        std::size_t i = 0;
        while (i < cmd.size() && (cmd[i] == ' ' || cmd[i] == '\t')) ++i;
        std::size_t j = i;
        while (j < cmd.size() && cmd[j] != ' ' && cmd[j] != '\t') ++j;
        return std::string{cmd.substr(i, j - i)};
    }

    std::string derive_label(const TabSignal &s) const {
        const std::string dir = basename(s.cwd);
        if (s.running && !s.running_cmd.empty()) {
            std::string l = dir.empty() ? "" : dir + " ";
            l += "\u25B8 " + program(s.running_cmd); // ▸ prog
            if (s.running_ms >= 1000) l += "  " + fmt_elapsed(s.running_ms);
            return l;
        }
        if (s.have_last && !s.last_cmd.empty()) {
            const bool ok = !(s.last_exit && *s.last_exit != 0);
            if (ok) return (dir.empty() ? "" : dir + " \u25B8 ") + program(s.last_cmd);
            std::string l = program(s.last_cmd);
            if (s.last_exit) l += " (" + std::to_string(*s.last_exit) + ")";
            return l;
        }
        // Idle: cwd basename, else the window title, else a generic label.
        if (!dir.empty()) return dir;
        if (!s.title.empty()) return s.title;
        return "shell";
    }

    static std::string fmt_elapsed(std::int64_t ms) {
        const std::int64_t sec = ms / 1000;
        if (sec < 60) return std::to_string(sec) + "s";
        return std::to_string(sec / 60) + "m" + std::to_string(sec % 60) + "s";
    }

    TabStatus status_ = TabStatus::Idle;
    TabAttention attention_ = TabAttention::None;
    bool unseen_ = false;
    std::string label_ = "shell";
    std::string last_fingerprint_{};
    std::uint64_t seen_generation_ = 0;
};

} // namespace hand

#endif // HAND_TAB_MODEL_HPP
