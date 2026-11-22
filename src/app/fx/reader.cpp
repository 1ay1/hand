// hand::fx — the pty reader: one background stream per shell (see fx.hpp).

#include "fx.hpp"

#include <hand/core/credit.hpp>
#include <hand/core/platform.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

namespace hand::fx {

void read_pty(Sink out, std::stop_token st, mayag::CoOwned<Pty> pty, SessionId sid) {
    // This reader co-owns the pty: the fd stays open until it returns, even
    // if the tab is closed under it (the stream is then stopped, and the
    // last owner out closes the fd).
    const int fd = pty->fd();
    // Pipelined: while one chunk is with the UI the reader keeps draining the
    // pty into the next, so the producer never stalls on the (tiny, ~1 KB on
    // macOS) pty buffer; only when that next chunk is full (kCap) does it
    // stop reading -- and then the producer blocks, which is the throttle.
    constexpr std::size_t kCap = 4u << 20;
    // Each Output is at most a frame's worth of parsing (~6 ms at toe's
    // ~80 MB/s): a bigger one would starve the frame it lands in, and while
    // that frame is late the reader piles up an even bigger next one -- a
    // feedback loop that ran a `yes` flood down to 8 fps.
    constexpr std::size_t kSlice = 512u << 10;
    auto credit = mayag::CoOwned<OutputCredit>::make();   // this reader's, shared with the UI through each Output
    std::string buf;
    bool alive = true;
    const auto ship = [&] {
        std::string chunk;
        if (buf.size() <= kSlice) chunk = std::exchange(buf, std::string{});
        else {
            // cut on a UTF-8 boundary, so no slice ends mid-character
            std::size_t n = kSlice;
            while (n > kSlice - 4 && (static_cast<unsigned char>(buf[n]) & 0xC0) == 0x80) --n;
            chunk.assign(buf, 0, n);
            buf.erase(0, n);
        }
        credit->spend();
        if (!out.send(App::Output{std::move(chunk), sid, credit})) credit->refund();
    };
    while (!st.stop_requested()) {
        const bool busy = !credit->free();
        if (!busy && !buf.empty()) { ship(); continue; }
        if (!alive) {
            if (busy) {   // the last output lands before the exit
                (void)credit->wait(-1, 50);
                credit->settle();
                continue;
            }
            int code = -1;
            for (int i = 0; i < 20 && code < 0; ++i) {   // bounded: ≤ ~200 ms
                code = pty->reap();
                if (code < 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            out.send(App::Exited{std::max(code, 0), sid});
            return;
        }
        const bool room = buf.size() < kCap;
        // 50 ms: stop is always prompt. pty = -1 while back-pressured, so a
        // stalled reader waits only on the credit, never on the pty.
        const plat::Ready r = credit->wait(room ? pty_handle(fd) : -1, 50);
        if (r.error) { alive = false; continue; }
        if (r.woke) credit->settle();
        if (room && r.data) alive = pty_read(fd, buf, 0);
        // A quiet pty: ask whether the child is gone. On POSIX a dead child
        // breaks the pty (r.data, then EOF above); a ConPTY pipe stays open
        // until the pseudoconsole is closed, so only pty_read can tell, and
        // without this `exit` left the tab (and the window) open forever.
        else if (room && !r.woke) alive = pty_read(fd, buf, 0);
    }
}

}  // namespace hand::fx
