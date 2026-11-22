#pragma once
// hand::OutputCredit — one shell's flow control, shared by its reader thread
// and the UI.
//
//   reader thread                         UI (the jaal loop)
//   ─────────────                         ──────────────────
//   free()? ── spend() ── send Output{chunk, credit} ──▶ feed it to toe
//   wait(pty) ◀──────────── wakes ◀──────────────────── give_back()
//
// At most ONE chunk is with the UI at a time, so a firehose (`yes`) is
// throttled by the kernel's pty buffer instead of flooding the mailbox. The
// credit travels INSIDE the Output it pays for, so the UI never looks
// anything up and a closed tab cannot strand it: whatever the UI does with
// the chunk, the message comes back with the credit attached.
//
// Threads share one OutputCredit through mayag::CoOwned, which requires
// mayag::Sync -- PROVEN below by jaal, from the two fields: a Waker (const
// ends) and an atomic flag. This file is one of the few places in hand
// allowed a raw atomic (tests/allow_raw_include_hand.txt), because it IS the
// safe type the rest of hand uses.

#include <hand/core/platform.hpp>

#include <mayag/core/sync.hpp>

#include <atomic>
#include <cstdint>

namespace hand {

class OutputCredit {
  public:
    OutputCredit() = default;
    OutputCredit(const OutputCredit&) = delete;
    OutputCredit& operator=(const OutputCredit&) = delete;

    /// Reader: may a chunk go out now (none is with the UI)?
    [[nodiscard]] bool free() const noexcept { return !out.load(std::memory_order_acquire); }
    /// Reader: a chunk is on its way.
    void spend() noexcept { out.store(true, std::memory_order_release); }
    /// Reader: the loop is gone, so the chunk never left.
    void refund() noexcept { out.store(false, std::memory_order_release); }
    /// UI: the chunk is fed; the reader may send the next one.
    void give_back() noexcept {
        out.store(false, std::memory_order_release);
        wake.signal();
    }
    /// Reader: sleep until the pty has bytes, the credit comes back, or the
    /// timeout passes. pty = -1 watches only the credit (back-pressured).
    [[nodiscard]] plat::Ready wait(std::intptr_t pty, int timeout_ms) const {
        return plat::wait_readable(wake, pty, timeout_ms);
    }
    /// Reader: consume the wakeups that were collected.
    void settle() const noexcept { wake.drain(); }

    // Public on purpose: the state jaal checks.
    const plat::Waker wake;
    std::atomic<bool> out{false};   ///< a chunk is with the UI
};
static_assert(mayag::Sync<OutputCredit>, "OutputCredit must be safe to share between a reader and the UI");

}  // namespace hand
