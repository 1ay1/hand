#pragma once
// hand::model — per-tab modes: what one terminal can be doing besides showing
// its shell. Each is an optional on the Session: engaged exactly while on.

#include <hand/core/term.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace hand {

/// Copy mode (ctrl+shift+m): a keyboard cursor through the scrollback, vim keys.
struct CopyMode {
    enum class Visual : std::uint8_t { none, chars, lines, block };   ///< v, V, ^v
    std::int64_t row = 0;          ///< absolute line of the cursor
    int          col = 0;
    Visual       visual = Visual::none;
    std::int64_t anchor_row = 0;   ///< where visual mode started
    int          anchor_col = 0;
};

/// Hints (ctrl+shift+space): every URL / path / hash on screen, labelled.
struct HintMode {
    std::vector<Term::Hint>  hints;
    std::vector<std::string> labels;
    std::string              typed;
};

/// The machine a tab talks to over ssh.
struct Remote {
    bool   launched = false;        ///< hand ran ssh as the tab's program (so it can reconnect)
    bool   dropped = false;         ///< it ended badly: the tab stays, offering reconnect
    double dropped_at = -10.0;
    double connected_at = -10.0;
};

}  // namespace hand
