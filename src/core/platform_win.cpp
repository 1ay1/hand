// hand::plat — Windows.
//
// Two of these need real thought; the rest are direct equivalents.
//
// Waker is a manual-reset Event rather than a pipe: signalling an event is a
// single syscall with no buffer to overflow, and WaitForMultipleObjects takes
// it directly.
//
// wait_readable is the interesting one. ConPTY's output comes through a
// named pipe hand opens for OVERLAPPED reads (pty_win.cpp), with one read
// always in flight; its event is what the reader waits on, together with
// the waker, in a single WaitForMultipleObjects. No polling: output, a
// returned credit and a stop request all wake it the moment they happen,
// and an idle terminal costs no wakeups at all.

#include <mayag/core/target.hpp>
#include <hand/core/platform.hpp>

#if MAYAG_OS_WINDOWS

#include <windows.h>

#include <shlwapi.h>  // PathMatchSpecA — Win32's fnmatch

#include <algorithm>
#include <array>
#include <cstdlib>
#include <ctime>

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

std::tm local_time(std::time_t t) {
    std::tm tm{};
    ::localtime_s(&tm, &t);
    return tm;
}

std::string home_dir() {
    // %USERPROFILE%, never $HOME: Microsoft's OpenSSH reads %USERPROFILE%\.ssh
    const char* u = std::getenv("USERPROFILE");
    std::string s = u && *u ? u : "";
    for (char& ch : s) if (ch == '\\') ch = '/';
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
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

std::vector<std::string> command_runner() {
    // %ComSpec% is cmd.exe's own answer for where it lives. /d: no AutoRun
    // (a user's cmd macros must not run first), /s /c: run the rest as typed.
    std::string cmd = "cmd.exe";
    if (const char* cs = std::getenv("ComSpec"); cs != nullptr && *cs != '\0') cmd = cs;
    return {cmd, "/d", "/s", "/c"};
}

long read_stdin(char* buf, std::size_t n) {
    DWORD got = 0;
    if (::ReadFile(::GetStdHandle(STD_INPUT_HANDLE), buf, static_cast<DWORD>(std::min<std::size_t>(n, 1u << 30)), &got, nullptr) == FALSE)
        return ::GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
    return static_cast<long>(got);
}

bool write_stdout(const char* buf, std::size_t n) {
    const HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
    while (n > 0) {
        DWORD w = 0;
        if (::WriteFile(h, buf, static_cast<DWORD>(std::min<std::size_t>(n, 1u << 30)), &w, nullptr) == FALSE || w == 0) return false;
        buf += w;
        n -= w;
    }
    return true;
}

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

Waker::Ends Waker::open() {
    // a manual-reset event: signal() sets it, drain() resets it
    const HANDLE h = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (h == nullptr) return {0, 0};
    const auto v = reinterpret_cast<std::intptr_t>(h);
    return {v, v};
}

Waker::~Waker() {
    if (read_end != 0) ::CloseHandle(reinterpret_cast<HANDLE>(read_end));
}

void Waker::signal() const noexcept {
    if (read_end != 0) ::SetEvent(reinterpret_cast<HANDLE>(read_end));
}

void Waker::drain() const noexcept {
    if (read_end != 0) ::ResetEvent(reinterpret_cast<HANDLE>(read_end));
}

Ready wait_readable(const Waker& w, std::intptr_t pty, int timeout_ms) {
    // `pty` is the ConPTY output's in-flight read EVENT (pty_handle arms an
    // overlapped read on a named pipe and hands back its event), so both
    // halves are real kernel waits: one WaitForMultipleObjects, no polling,
    // no duty cycle. A byte from the shell, a keystroke's credit or a stop
    // request each wake the reader the moment they happen, and an idle tab
    // costs nothing at all.
    Ready out;
    const HANDLE wake = reinterpret_cast<HANDLE>(w.handle());
    const HANDLE data = pty > 0 ? reinterpret_cast<HANDLE>(pty) : nullptr;
    const DWORD total = timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms);
    const HANDLE hs[2] = {wake, data};
    const DWORD n = data != nullptr ? 2u : 1u;
    const DWORD r = ::WaitForMultipleObjects(n, hs, FALSE, total);
    if (r == WAIT_FAILED) {
        out.error = true;
        return out;
    }
    if (r == WAIT_TIMEOUT) return out;   // nothing ready, nothing wrong
    // Report both when both are pending: the reader drains the credit and reads.
    out.woke = ::WaitForSingleObject(wake, 0) == WAIT_OBJECT_0;
    out.data = data != nullptr && ::WaitForSingleObject(data, 0) == WAIT_OBJECT_0;
    return out;
}

}  // namespace hand::plat

#endif  // _WIN32
