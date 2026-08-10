// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Concurrency test for Mailbox<Msg>: many sender threads post owned messages
// concurrently; the single owner drains them. Asserts every message arrives
// exactly once (no loss, no dup, no race). Move-only payload proves nothing is
// copied across the boundary. Meant to be run under TSan too.

#include "hand/actor/mailbox.hpp"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

using hand::Mailbox;

// A move-only message carrying a unique id — proves the queue never copies.
struct Msg {
    int sender;
    int seq;
    Msg(int s, int q) : sender(s), seq(q) {}
    Msg(const Msg &) = delete;
    Msg &operator=(const Msg &) = delete;
    Msg(Msg &&) noexcept = default;
    Msg &operator=(Msg &&) noexcept = default;
};

int main() {
    int fails = 0;
    auto ck = [&](bool ok, const char *n) {
        if (!ok) { std::printf("FAIL %s\n", n); ++fails; }
    };

    constexpr int kSenders = 8;
    constexpr int kPerSender = 5000;
    Mailbox<Msg> mbox;

    // Received tally: [sender][seq] seen exactly once.
    std::vector<std::vector<std::atomic<int>>> seen(kSenders);
    for (auto &v : seen) v = std::vector<std::atomic<int>>(kPerSender);

    std::atomic<bool> senders_done{false};
    std::atomic<int> total_received{0};

    // Owner/receiver thread: drain until all messages are in.
    std::thread receiver([&] {
        const int expected = kSenders * kPerSender;
        while (total_received.load() < expected) {
            mbox.drain([&](Msg m) {
                seen[static_cast<std::size_t>(m.sender)][static_cast<std::size_t>(m.seq)]
                    .fetch_add(1);
                total_received.fetch_add(1);
            });
            // Brief yield so we don't spin the CPU flat while senders produce.
            std::this_thread::yield();
        }
    });

    // Sender threads: each posts kPerSender uniquely-tagged messages.
    std::vector<std::thread> senders;
    for (int s = 0; s < kSenders; ++s) {
        senders.emplace_back([&, s] {
            for (int q = 0; q < kPerSender; ++q) mbox.post(Msg{s, q});
        });
    }
    for (auto &t : senders) t.join();
    senders_done.store(true);
    receiver.join();

    // Every (sender, seq) must have been received EXACTLY once.
    int missing = 0, dup = 0;
    for (int s = 0; s < kSenders; ++s)
        for (int q = 0; q < kPerSender; ++q) {
            const int c = seen[static_cast<std::size_t>(s)][static_cast<std::size_t>(q)].load();
            if (c == 0) ++missing;
            else if (c > 1) ++dup;
        }
    ck(total_received.load() == kSenders * kPerSender, "received count == posted count");
    ck(missing == 0, "no message lost");
    ck(dup == 0, "no message delivered twice");

    // The wakeup fd exists on POSIX so the owner can fold it into poll().
#if !defined(_WIN32)
    ck(mbox.wait_fd() >= 0, "mailbox exposes a pollable wakeup fd");
#endif

    std::printf(fails ? "%d MAILBOX TEST(S) FAILED\n" : "ALL MAILBOX TESTS PASS\n", fails);
    return fails ? 1 : 0;
}
