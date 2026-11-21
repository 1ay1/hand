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
// mayag::Sync. core knows nothing of the runtime, so that CLAIM is made where
// the credit is shared: include/hand/app/msg.hpp. This file is one of the
// few places in hand allowed a raw atomic (tests/allow_raw.txt), because it
// IS the safe type the rest of hand uses.

#include <hand/core/platform.hpp>

#include <atomic>
#include <cstdint>

namespace hand {

class OutputCredit {
  public:
    OutputCredit() = default;
    OutputCredit(const OutputCredit&) = delete;
    OutputCredit& operator=(const OutputCredit&) = delete;

    /// Reader: may a chunk go out now (none is with the UI)?
    [[nodiscard]] bool free() const noexcept { return !out_.load(std::memory_order_acquire); }
    /// Reader: a chunk is on its way.
    void spend() noexcept { out_.store(true, std::memory_order_release); }
    /// Reader: the loop is gone, so the chunk never left.
    void refund() noexcept { out_.store(false, std::memory_order_release); }
    /// UI: the chunk is fed; the reader may send the next one.
    void give_back() noexcept {
        out_.store(false, std::memory_order_release);
        wake_.signal();
    }
    /// Reader: sleep until the pty has bytes, the credit comes back, or the
    /// timeout passes. pty = -1 watches only the credit (back-pressured).
    [[nodiscard]] plat::Ready wait(std::intptr_t pty, int timeout_ms) const {
        return plat::wait_readable(wake_, pty, timeout_ms);
    }
    /// Reader: consume the wakeups that were collected.
    void settle() const noexcept { wake_.drain(); }

  private:
    plat::Waker       wake_;
    std::atomic<bool> out_{false};
};

}  // namespace hand
