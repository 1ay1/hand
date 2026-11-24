// hand::proc — what every OS does the same way (one implementation).

#include <hand/core/platform.hpp>
#include <hand/core/proc.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace hand::proc {

std::string temp_path(std::string_view stem) {
    // No shared counter: sftp batches run on worker threads at the same time
    // (a pane listing and a preview), and a plain `static unsigned n; ++n` is
    // a data race that handed two of them the same file -- one batch then
    // overwrote the other's. A fresh draw from the OS's entropy per call,
    // with the pid and the clock, shares nothing between threads.
    std::random_device rd;
    const auto t = static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count());
    char tail[64];
    std::snprintf(tail, sizeof tail, "-%d-%llx-%08x%08x.txt", plat::self_pid(), t, rd(), rd());
    return (std::filesystem::temp_directory_path() / (std::string{stem} + tail)).string();
}

namespace {
// The pool's state. A leaf lock: held only to move unique_ptrs in and out of
// a map, never across I/O, never while taking another lock (allowlisted in
// tests/allow_raw_src.txt). Closing a channel kills and reaps its child, so
// that happens after the lock is let go.
struct PoolState {
    std::mutex mu;
    std::unordered_map<std::string, std::vector<std::unique_ptr<Channel>>> idle;
};
PoolState& pool_state() {
    static PoolState s;   // lives until exit: its channels' children are ended then
    return s;
}
/// Pull every channel idle longer than `max_idle` out of `idle` into `out`.
void sweep(std::unordered_map<std::string, std::vector<std::unique_ptr<Channel>>>& idle, std::chrono::seconds max_idle,
           std::vector<std::unique_ptr<Channel>>& out) {
    const auto now = std::chrono::steady_clock::now();
    for (auto& [k, v] : idle)
        for (auto it = v.begin(); it != v.end();)
            if (now - (*it)->used > max_idle) { out.push_back(std::move(*it)); it = v.erase(it); }
            else ++it;
}
}  // namespace

ChannelPool& channels() {
    static ChannelPool p;
    return p;
}

std::unique_ptr<Channel> ChannelPool::take(const std::string& key, std::chrono::seconds max_idle) {
    std::vector<std::unique_ptr<Channel>> stale;
    std::unique_ptr<Channel> got;
    {
        auto& s = pool_state();
        std::lock_guard lk(s.mu);
        sweep(s.idle, max_idle, stale);
        if (auto it = s.idle.find(key); it != s.idle.end() && !it->second.empty()) {
            got = std::move(it->second.back());   // the most recently used: the warmest
            it->second.pop_back();
        }
    }
    return got;   // `stale` closes here, outside the lock
}

void ChannelPool::give(const std::string& key, std::unique_ptr<Channel> ch, std::size_t keep) {
    if (!ch || ch->broken()) return;
    ch->used = std::chrono::steady_clock::now();
    std::unique_ptr<Channel> extra;
    {
        auto& s = pool_state();
        std::lock_guard lk(s.mu);
        auto& v = s.idle[key];
        if (v.size() < keep) v.push_back(std::move(ch));
        else extra = std::move(ch);
    }
}

void ChannelPool::drop(const std::string& prefix) {
    std::vector<std::unique_ptr<Channel>> gone;
    {
        auto& s = pool_state();
        std::lock_guard lk(s.mu);
        for (auto& [k, v] : s.idle)
            if (k.rfind(prefix, 0) == 0) {
                for (auto& c : v) gone.push_back(std::move(c));
                v.clear();
            }
    }
}

std::size_t ChannelPool::idle(const std::string& key) {
    auto& s = pool_state();
    std::lock_guard lk(s.mu);
    const auto it = s.idle.find(key);
    return it == s.idle.end() ? 0 : it->second.size();
}

}  // namespace hand::proc
