// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand/gui/runtime.hpp — the impure conductor of the GUI Elm loop.
//
// Everything pure (gui_update) and everything isolated (TabActor threads,
// Mailbox) has been built and proven; the runtime is the single impure seam
// that wires them to the real window:
//
//   loop:
//     wait on { window fd, GUI mailbox fd }        (one poll, no busy-wait)
//     drain window events        -> translate to GuiMsg -> gui_update -> interpret
//     drain actor messages (GUI mailbox) -> gui_update -> interpret
//     if a Present cmd fired, render the active tab + chrome via TabbedView
//
// interpret(GuiCmd) is where effects happen: SpawnTab starts a TabActor thread,
// SendToTab posts input to an actor, KillTab joins one, Present draws, Quit
// ends the loop. The runtime owns the TabActors (keyed by TabId) so the pure
// model never holds a thread or a Terminal.
//
// It is deliberately templated on nothing terminal-specific except a
// `SpawnFn` (makes a toe::Terminal for a new tab) and a `Present` callback that
// draws one frame — so the same runtime drives any plat backend.

#ifndef HAND_GUI_RUNTIME_HPP
#define HAND_GUI_RUNTIME_HPP

#include <atomic>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>

#include "hand/actor/mailbox.hpp"
#include "hand/actor/tab_actor.hpp"
#include "hand/gui/animator.hpp"
#include "hand/gui/message.hpp"
#include "hand/gui/model.hpp"
#include "toe/terminal.hpp"

namespace hand {

class GuiRuntime {
public:
    // SpawnFn: create a fresh Terminal for a new tab, opened in `cwd` (empty =>
    // default). Returns nullopt on failure (the tab is then dropped).
    using SpawnFn = std::function<std::optional<toe::Terminal>(const std::string &cwd)>;
    // PresentFn: draw one frame for `active` tab's terminal (already locked by
    // the runtime) at the current window size. Supplied by the backend, which
    // owns the GL context + TabbedView.
    using PresentFn = std::function<void(TabActor &active)>;

    GuiRuntime(SpawnFn spawn, PresentFn present)
        : spawn_(std::move(spawn)), present_(std::move(present)),
          model_(mint_first_()) {}

    // The GUI mailbox actors post GuiMsgs to. The backend also posts window
    // events here (translated to GuiMsg) and folds mailbox_.wait_fd() into its
    // poll set.
    [[nodiscard]] Mailbox<GuiMsg> &mailbox() noexcept { return mailbox_; }
    [[nodiscard]] GuiModel &model() noexcept { return model_; }
    [[nodiscard]] bool done() const noexcept { return model_.quitting(); }

    // Bootstrap: spawn the first tab's actor (the model already has its entry).
    void start() {
        const TabId first = model_.tabs().focus().id;
        spawn_actor(first, /*cwd=*/{});
    }

    // Post a GuiMsg from the backend (window event) or anywhere.
    void post(GuiMsg m) { mailbox_.post(std::move(m)); }

    // Process all pending messages: drain -> update -> interpret. Renders at
    // most once even if several Present cmds fired this batch (coalesced).
    void pump() {
        bool need_present = false;
        mailbox_.drain([&](GuiMsg m) {
            GuiCmds cmds = gui_update(model_, std::move(m));
            for (auto &c : cmds) {
                if (interpret(std::move(c))) need_present = true;
            }
        });
        if (need_present && !model_.quitting()) present_active();
    }

private:
    // Returns true if the cmd requested a present (coalesced by the caller).
    bool interpret(GuiCmd c) {
        bool present = false;
        std::visit(
            [&](auto &&e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, SpawnTab>) {
                    // The model already inserted the entry + focused it; its id
                    // is the focus. Spawn its actor in the given cwd.
                    spawn_actor(model_.tabs().focus().id, e.cwd);
                } else if constexpr (std::is_same_v<T, SendToTab>) {
                    if (auto it = actors_.find(e.id); it != actors_.end())
                        it->second->send(TabInput{std::move(e.bytes)});
                } else if constexpr (std::is_same_v<T, SendKeyToTab>) {
                    if (auto it = actors_.find(e.id); it != actors_.end())
                        it->second->send(TabKey{e.key});
                } else if constexpr (std::is_same_v<T, KillTab>) {
                    if (auto it = actors_.find(e.id); it != actors_.end()) {
                        it->second->stop(); // joins the thread
                        actors_.erase(it);
                    }
                } else if constexpr (std::is_same_v<T, Present>) {
                    present = true;
                } else if constexpr (std::is_same_v<T, SetWindowTitle>) {
                    if (set_title_) set_title_(e.title);
                } else if constexpr (std::is_same_v<T, WindowControl>) {
                    if (window_ctl_) window_ctl_(e.action);
                } else if constexpr (std::is_same_v<T, Quit>) {
                    // Model is already quitting; stop every actor.
                    for (auto &[id, a] : actors_) a->stop();
                    actors_.clear();
                }
            },
            std::move(c));
        return present;
    }

    void spawn_actor(TabId id, const std::string &cwd) {
        auto term = spawn_(cwd);
        if (!term) {
            // Spawn failed: report the tab as exited so the model drops it.
            mailbox_.post(TabExited{id, -1});
            return;
        }
        auto actor = std::make_unique<TabActor>(id, std::move(*term), mailbox_);
        TabActor *raw = actor.get();
        actors_.emplace(id, std::move(actor));
        raw->start();
    }

    void present_active() {
        const TabId fid = model_.tabs().focus().id;
        if (auto it = actors_.find(fid); it != actors_.end()) present_(*it->second);
    }

public:
    // Run `fn(toe::Session&)` on the FOCUSED tab's live terminal, holding that
    // tab's render_lock (the actor may be mutating its grid). Returns false if
    // the focused tab has no live session. Used by the overlay input routing so
    // search's screen mutations are race-free.
    template <class F>
    bool with_focus_session(F &&fn) {
        const TabId fid = model_.tabs().focus().id;
        auto it = actors_.find(fid);
        if (it == actors_.end()) return false;
        std::lock_guard lk(it->second->render_lock());
        if (auto *s = it->second->terminal().poll().running) {
            fn(*s);
            return true;
        }
        return false;
    }

    // Run `fn(toe::Session&)` on EVERY live tab, each under its own render_lock.
    // Used to fan a global settings change out to all tabs.
    template <class F>
    void for_each_live_session(F &&fn) {
        for (auto &[id, actor] : actors_) {
            std::lock_guard lk(actor->render_lock());
            if (auto *s = actor->terminal().poll().running) fn(*s);
        }
    }

    // A running-command spinner or a done-attention pulse needs ~60fps repaints.
    // Pure model read (TabModel status/attention) — no lock, no engine call.
    // Refreshes the Animator's Spinner/Pulse sources; call once per pump.
    void refresh_anim_sources() {
        bool running = false; // a command IN FLIGHT -> smooth 60fps spinner
        bool pulse = false;   // a latched done-attention -> slow pulse
        model_.tabs().for_each_ordered([&](const TabEntry &e, bool, std::size_t) {
            if (e.model.status() == TabStatus::Running) running = true;
            if (e.model.attention() != TabAttention::None) pulse = true;
        });
        anim_.set(Anim::Spinner, running);
        anim_.set(Anim::Pulse, pulse);
    }

    // The one animation authority — the loop asks it for the wait deadline, and
    // present()/refresh publish the live sources into it. Lock-free.
    [[nodiscard]] Animator &animator() noexcept { return anim_; }
    [[nodiscard]] int animation_deadline_ms() { refresh_anim_sources(); return anim_.next_deadline_ms(); }
    // Cache the focused tab's cursor-blink period (call when focus changes /
    // settings apply).
    void set_focus_blink_ms(int ms) noexcept { anim_.set_blink_ms(ms); anim_.set(Anim::Blink, ms > 0); }
    // Publish whether the focused caret is animating (set by present() under the
    // render_lock). Feeds the Animator's CaretGlide source, lock-free.
    void set_focus_animating(bool a) noexcept { anim_.set(Anim::CaretGlide, a); }
    // Set the animation frame index (time-derived; drives the chrome spinner /
    // pulse phase). Also refreshes the focused tab's derived state.
    void set_frame(std::uint32_t f) {
        model_.set_frame_index(f);
        model_.refresh(model_.tabs().focus(), /*active=*/true);
    }
    // Present the focused tab now (the loop's direct-present path for animation).
    void present_focused() { present_active(); }

    // --- interaction-driven present (the OTHER repaint reason) --------------
    // Animation is time-driven (Animator). But some interactions change the
    // VISIBLE pixels WITHOUT bumping the terminal's content generation() — a
    // selection drag, a scrollback jump, rail hover. Those would be frame-
    // skipped by the RenderKey (generation unchanged) and lag until the next
    // unrelated repaint. So the input path calls request_present() after
    // routing any event to the session; the loop consumes the flag and folds
    // interaction_revision() into the RenderKey so the gate can't skip it.
    void request_present() noexcept {
        interaction_rev_.fetch_add(1, std::memory_order_relaxed);
        present_pending_.store(true, std::memory_order_relaxed);
    }
    // The loop calls this once per iteration: true (once) if a present was
    // requested since the last take. Clears the pending flag.
    [[nodiscard]] bool take_present_request() noexcept {
        return present_pending_.exchange(false, std::memory_order_relaxed);
    }
    // Monotonic counter of interaction-driven repaint requests; fold into the
    // RenderKey so a selection/scroll/hover change always yields a fresh key.
    [[nodiscard]] std::uint32_t interaction_revision() const noexcept {
        return interaction_rev_.load(std::memory_order_relaxed);
    }

private:
    static TabId mint_first_() { return TabId{1}; }

public:
    // Optional: the backend installs a title setter (reflects focused tab title).
    std::function<void(const std::string &)> set_title_{};
    // Optional: the backend installs a window-control handler (CSD buttons).
    std::function<void(WinCtl)> window_ctl_{};

private:
    SpawnFn spawn_;
    PresentFn present_;
    Mailbox<GuiMsg> mailbox_;
    GuiModel model_;
    std::unordered_map<TabId, std::unique_ptr<TabActor>> actors_;
    Animator anim_; // the single animation-timing authority (lock-free)
    // Interaction repaint bus: request_present() raises present_pending_ and
    // bumps interaction_rev_ (folded into the RenderKey so the gate repaints a
    // selection/scroll change whose content generation() is unchanged).
    std::atomic<bool> present_pending_{false};
    std::atomic<std::uint32_t> interaction_rev_{0};
};

} // namespace hand

#endif // HAND_GUI_RUNTIME_HPP
