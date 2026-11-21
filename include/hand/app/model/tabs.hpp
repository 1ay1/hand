#pragma once
// hand::Tabs — the window's sessions, in strip order, and which is in front.
//
// Two invariants, kept by construction rather than checked at call sites:
//
//   1. never empty        a window always shows a terminal
//   2. index() < size()   the active tab always exists
//
// Every mutation preserves both, and keeps index() on the SAME session when
// others are inserted, erased or reordered around it. Callers say what they
// mean (insert beside, close, move left) instead of patching indices.

#include <hand/app/model/session.hpp>
#include <hand/core/types.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace hand {

class Tabs {
  public:
    Tabs() : list_(1) {}   // (1): an empty session until init spawns the shell

    // ── reading ────────────────────────────────────────────────────────────
    [[nodiscard]] std::size_t size() const noexcept { return list_.size(); }
    [[nodiscard]] std::size_t index() const noexcept { return index_; }
    [[nodiscard]] Session& active() noexcept { return list_[index_]; }
    [[nodiscard]] const Session& active() const noexcept { return list_[index_]; }
    [[nodiscard]] Session& operator[](std::size_t i) noexcept { assert(i < size()); return list_[i]; }
    [[nodiscard]] const Session& operator[](std::size_t i) const noexcept { assert(i < size()); return list_[i]; }
    [[nodiscard]] auto begin() noexcept { return list_.begin(); }
    [[nodiscard]] auto end() noexcept { return list_.end(); }
    [[nodiscard]] auto begin() const noexcept { return list_.begin(); }
    [[nodiscard]] auto end() const noexcept { return list_.end(); }

    /// Where the session with this id is (its pty's output goes there).
    [[nodiscard]] std::optional<std::size_t> find(SessionId sid) const noexcept {
        const auto it = std::ranges::find(list_, sid, &Session::sid);
        if (it == list_.end()) return std::nullopt;
        return static_cast<std::size_t>(it - list_.begin());
    }

    // ── changing ───────────────────────────────────────────────────────────
    /// Bring tab i to the front.
    void select(std::size_t i) noexcept {
        assert(i < size());
        index_ = i;
    }
    /// Put s at position `at` (clamped to the end); the active tab stays active.
    /// Returns where it went.
    std::size_t insert(std::size_t at, Session s) {
        at = std::min(at, size());
        list_.insert(list_.begin() + static_cast<std::ptrdiff_t>(at), std::move(s));
        if (at <= index_ && size() > 1) ++index_;
        return at;
    }
    /// Append s; returns its position.
    std::size_t push_back(Session s) { return insert(size(), std::move(s)); }
    /// Close tab i (never the last one). The active tab stays active; when it
    /// is i itself, its right neighbour takes over, else its left.
    void erase(std::size_t i) {
        assert(i < size() && size() > 1);
        list_.erase(list_.begin() + static_cast<std::ptrdiff_t>(i));
        if (index_ > i || index_ == size()) --index_;
    }
    /// Move the active tab one step; it stays active. False at either end.
    bool move_active(Step s) noexcept {
        const auto to = static_cast<long long>(index_) + delta(s);
        if (to < 0 || to >= static_cast<long long>(size())) return false;
        std::swap(list_[index_], list_[static_cast<std::size_t>(to)]);
        index_ = static_cast<std::size_t>(to);
        return true;
    }

  private:
    std::vector<Session> list_;
    std::size_t          index_ = 0;
};

}  // namespace hand
