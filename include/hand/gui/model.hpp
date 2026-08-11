// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand/gui/model.hpp — the GUI's pure Model + update.
//
// GuiModel is the Elm state for the whole window: a Zipper of tab METADATA
// (never the terminals — those live on their actor threads; the model only
// mirrors each tab's derived status via TabModel + a stable TabId). update is
// pure and total:
//
//     gui_update(GuiModel&, GuiMsg) -> GuiCmds
//
// It mutates the model in place (an owned value on the GUI thread) and RETURNS
// the effects to run. No I/O, no threads, no windowing here — so the entire GUI
// control flow is unit-testable by feeding GuiMsgs and asserting (state, cmds).

#ifndef HAND_GUI_MODEL_HPP
#define HAND_GUI_MODEL_HPP

#include <cstdint>
#include <optional>
#include <string>

#include "hand/gui/message.hpp"
#include "hand/tab_model.hpp"
#include "hand/zipper.hpp"

namespace hand {

// One tab's GUI-side mirror. The Terminal itself is NOT here — it's owned by the
// tab's actor thread; this is only what the chrome needs to draw + route.
struct TabEntry {
    TabId id{};
    TabModel model{};                 // derived status/label/attention (from TabSignals)
    std::int64_t running_since_ms = 0;
    std::string title{};
    std::string cwd{};
    bool running_cmd = false;
    std::string cur_cmd{};
    bool have_last = false;
    std::string last_cmd{};
    std::optional<int> last_exit{};
    std::uint64_t generation = 0;

    explicit TabEntry(TabId i) : id(i) {}
    TabEntry(TabEntry &&) noexcept = default;
    TabEntry &operator=(TabEntry &&) noexcept = default;
    TabEntry(const TabEntry &) = delete;
    TabEntry &operator=(const TabEntry &) = delete;
};

class GuiModel {
public:
    // The GUI is born with one tab already spawning (id assigned here; the
    // SpawnTab cmd is emitted by the caller's bootstrap, not update).
    explicit GuiModel(TabId first) : tabs_(TabEntry{first}) { next_raw_ = to_raw(first) + 1; }

    GuiModel(GuiModel &&) noexcept = default;
    GuiModel &operator=(GuiModel &&) noexcept = default;

    [[nodiscard]] Zipper<TabEntry> &tabs() noexcept { return tabs_; }
    [[nodiscard]] const Zipper<TabEntry> &tabs() const noexcept { return tabs_; }
    [[nodiscard]] bool quitting() const noexcept { return quitting_; }
    [[nodiscard]] std::uint64_t frame() const noexcept { return frame_; }
    // Set the animation frame index (time-derived by the loop).
    void set_frame_index(std::uint64_t f) noexcept { frame_ = f; }

    // Allocate a fresh, never-reused TabId (for the runtime to spawn an actor).
    [[nodiscard]] TabId mint_id() noexcept { return TabId{next_raw_++}; }

    // Refresh a tab entry's TabModel from its mirrored signal fields. Called
    // after any field-updating message, and on Tick (so the spinner animates).
    void refresh(TabEntry &e, bool active) {
        TabSignal sig;
        sig.alive = true;
        sig.cwd = e.cwd;
        sig.title = e.title;
        sig.generation = e.generation;
        sig.running = e.running_cmd;
        sig.running_cmd = e.cur_cmd;
        sig.running_ms = 0; // GUI-side elapsed is derived by the runtime clock
        sig.have_last = e.have_last;
        sig.last_cmd = e.last_cmd;
        sig.last_exit = e.last_exit;
        e.model.update(sig, active);
    }

private:
    Zipper<TabEntry> tabs_;
    std::uint64_t next_raw_ = 1;
    std::uint64_t frame_ = 0;
    bool quitting_ = false;

    static std::uint64_t to_raw(TabId i) noexcept { return static_cast<std::uint64_t>(i); }

    friend GuiCmds gui_update(GuiModel &, GuiMsg);
    // update helpers may touch these:
    void set_frame(std::uint64_t f) noexcept { frame_ = f; }
    void set_quitting() noexcept { quitting_ = true; }
};

// The pure reducer. Mutates `m` and returns the effects to perform.
[[nodiscard]] GuiCmds gui_update(GuiModel &m, GuiMsg msg);

} // namespace hand

#endif // HAND_GUI_MODEL_HPP
