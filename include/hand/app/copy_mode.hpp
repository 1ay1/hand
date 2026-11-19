#pragma once
// hand::copy_mode — vim keys through the scrollback, as a pure step function.
//
//   CopyMode next = copy_step(cm, key, text, screen)   -> where the cursor goes, what to do
//
// No model, no clipboard: the update applies the outcome. That keeps every
// motion (w e b { } ^ $ g G ...) testable on a fake screen.

#include <hand/app/model.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hand {

/// What copy mode sees of the terminal.
struct CopyScreen {
    std::int64_t last_row = 0;            ///< the last absolute line
    int          max_col = 0;
    int          half_page = 1;
    std::int64_t cursor_row = 0;          ///< the live cursor (G)
    int          cursor_col = 0;
    std::vector<std::int64_t> prompts;    ///< prompt lines ({ })
    std::function<std::u32string(std::int64_t)> line;   ///< one absolute line
};

/// What a key in copy mode asks for.
struct CopyStep {
    enum class Do : std::uint8_t {
        nothing,   ///< not a copy-mode key
        moved,     ///< the cursor or the visual mode changed: reselect, reveal
        yank,      ///< copy the selection (or the cursor's line) and leave
        unmark,    ///< drop the selection, stay in copy mode
        leave,     ///< leave, selection cleared
        search,    ///< leave into scrollback search
    };
    Do       act = Do::nothing;
    CopyMode mode{};
};

[[nodiscard]] CopyStep copy_step(const CopyMode& cm, mayag::Key key, mayag::Mods mods, const std::string& text,
                                 const CopyScreen& s);

}  // namespace hand
