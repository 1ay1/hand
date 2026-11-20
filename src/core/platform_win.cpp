// hand::plat — Windows.
//
// Two of these need real thought; the rest are direct equivalents.
//
// Waker is a manual-reset Event rather than a pipe: signalling an event is a
// single syscall with no buffer to overflow, and WaitForMultipleObjects takes
// it directly.
//
// wait_readable is the interesting one. ConPTY hands back ANONYMOUS pipes,
// and anonymous pipes cannot be waited on for readability — they are always
// signalled as objects, and overlapped I/O is not supported on them. So
// readiness is polled with PeekNamedPipe on a short duty cycle, while the
// WAKER is waited on properly. That ordering matters: a stop request or new
// output credit returns immediately (ctrl-C stays crisp), and only the
// "is there data yet" question costs a 1 ms tick. Peak cost is ~1000
// cheap peeks/second on an idle terminal, which does not show up on a
// profile; the alternative (a per-pty pump thread doing blocking ReadFile)
// costs a thread and a copy per session.

#include <hand/core/platform.hpp>

#if defined(_WIN32)

#include <windows.h>

#include <shlwapi.h>  // PathMatchSpecA — Win32's fnmatch

#include <algorithm>
#include <array>

namespace hand::plat {

bool glob_match(const std::string& pat, const std::string& s) {
    // PathMatchSpecA implements `*` and `?`. It does NOT implement `[a-z]`
    // character classes, which ssh_config allows, so fall back to a small
    // matcher when the pattern uses one. Most patterns are `*` globs.
    if (pat.find('[') == std::string::npos)
        return ::PathMatchSpecA(s.c_str(), pat.c_str()) != FALSE;

    // Iterative backtracking glob: `*` `?` and `[...]` with ranges and `!`.
    std::size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < s.size()) {
        if (p < pat.size() && pat[p] == '[') {
            std::size_t q = p + 1;
            const bool neg = q < pat.size() && (pat[q] == '!' || pat[q] == '^');
            if (neg) ++q;
            bool hit = false;
            for (bool first = true; q < pat.size() && (first || pat[q] != ']'); ++q, first = false) {
                if (q + 2 < pat.size() && pat[q + 1] == '-' && pat[q + 2] != ']') {
                    if (s[t] >= pat[q] && s[t] <= pat[q + 2]) hit = true;
                    q += 2;
                } else if (pat[q] == s[t]) {
                    hit = true;
                }
            }
            if (q < pat.size() && hit != neg) { p = q + 1; ++t; continue; }
        } else if (p < pat.size() && (pat[p] == '?' || pat[p] == s[t])) {
            ++p;
            ++t;
            continue;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = t;
            continue;
        }
        if (star == std::string::npos) return false;
        p = star + 1;
        t = ++mark;
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

std::string user_token() {
    // There is no uid. The username is unique per machine and is what the
    // user would recognise in a temp path, so sanitise it to path-safe ASCII.
    std::array<char, 256> name{};
    DWORD n = static_cast<DWORD>(name.size());
    if (::GetUserNameA(name.data(), &n) == FALSE || n == 0) return "user";
    std::string out;
    for (DWORD i = 0; i + 1 < n; ++i) {
        const char c = name[i];
        out.push_back((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : '-');
    }
    return out.empty() ? "user" : out;
}

int self_pid() { return static_cast<int>(::GetCurrentProcessId()); }

std::string self_exe() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return "hand.exe";
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);   // truncated: try again with more room
    }
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, buf.c_str(), static_cast<int>(buf.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, buf.c_str(), static_cast<int>(buf.size()), out.data(), need, nullptr, nullptr);
    return out;
}

std::uint64_t peak_rss() {
    // PROCESS_MEMORY_COUNTERS lives in psapi.h, but K32GetProcessMemoryInfo
    // is in kernel32 on every supported Windows, so this needs no extra lib.
    struct Counters {
        DWORD cb;
        DWORD page_fault_count;
        SIZE_T peak_working_set;
        SIZE_T working_set;
        SIZE_T quota_peak_paged_pool;
        SIZE_T quota_paged_pool;
        SIZE_T quota_peak_nonpaged_pool;
        SIZE_T quota_nonpaged_pool;
        SIZE_T pagefile;
        SIZE_T peak_pagefile;
    } c{};
    c.cb = sizeof c;
    using Fn = BOOL(WINAPI*)(HANDLE, void*, DWORD);
    static const Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(
        ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo")));
    if (fn == nullptr || fn(::GetCurrentProcess(), &c, c.cb) == FALSE) return 0;
    return static_cast<std::uint64_t>(c.peak_working_set);
}

Waker::Waker() {
    // Manual reset: signal() may be called many times before a waiter looks,
    // and drain() is the explicit reset, matching the self-pipe semantics.
    const HANDLE h = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    read_ = reinterpret_cast<std::intptr_t>(h);
    write_ = read_;
}

Waker::~Waker() {
    if (read_ != 0) ::CloseHandle(reinterpret_cast<HANDLE>(read_));
}

void Waker::signal() const noexcept {
    if (read_ != 0) ::SetEvent(reinterpret_cast<HANDLE>(read_));
}

void Waker::drain() const noexcept {
    if (read_ != 0) ::ResetEvent(reinterpret_cast<HANDLE>(read_));
}

Ready wait_readable(const Waker& w, std::intptr_t pty, int timeout_ms) {
    Ready out;
    const HANDLE wake = reinterpret_cast<HANDLE>(w.handle());
    const HANDLE pipe = pty > 0 ? reinterpret_cast<HANDLE>(pty) : INVALID_HANDLE_VALUE;

    // Already have bytes? Say so without sleeping at all.
    const auto peek = [&]() -> bool {
        if (pipe == INVALID_HANDLE_VALUE) return false;
        DWORD avail = 0;
        if (::PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr) == FALSE)
            return true;  // broken pipe: "readable" so the caller reads EOF
        return avail > 0;
    };
    if (peek()) {
        out.data = true;
        return out;
    }

    // Otherwise wait on the waker in short slices, peeking between them.
    const DWORD total = timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms);
    const DWORD slice = 1;
    for (DWORD spent = 0; total == INFINITE || spent < total;) {
        const DWORD step = total == INFINITE ? slice : std::min(slice, total - spent);
        const DWORD r = ::WaitForSingleObject(wake, step);
        if (r == WAIT_OBJECT_0) {
            out.woke = true;
            // Report data alongside the wakeup when both are pending.
            out.data = peek();
            return out;
        }
        if (r == WAIT_FAILED) {
            out.error = true;
            return out;
        }
        if (peek()) {
            out.data = true;
            return out;
        }
        spent += step;
    }
    return out;  // timeout: nothing ready, nothing wrong
}

}  // namespace hand::plat

#endif  // _WIN32
