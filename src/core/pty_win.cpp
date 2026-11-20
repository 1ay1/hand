// hand::Pty — ConPTY.
//
// The Windows half of the pty. Shape-compatible with the POSIX half: the app
// still sees an opaque `int fd` and an `int pid`, and still calls pty_read /
// pty_reap / pty_foreground. The mapping is:
//
//   fd   → the read end of the ConPTY output pipe (a HANDLE, via a table)
//   pid  → the child's real PID (looked up for name/argv via NtQuery)
//
// A ConPTY session is four objects: an input pipe we write to, an output pipe
// we read from, the HPCON itself, and the child process. They must be torn
// down in that order or conhost leaks; `Session` owns that order.
//
// Handles do not fit in an int, so sessions live in a small table and `fd` is
// an index into it. That keeps hand/core/pty.hpp free of windows.h, which is
// what lets the rest of the app stay POSIX-shaped.

#include <hand/core/pty.hpp>

#if defined(_WIN32)

#include <hand/core/platform.hpp>

#include <windows.h>

#include <shlwapi.h>    // PathMatchSpecA — Win32's fnmatch
#include <tlhelp32.h>   // process enumeration, for the foreground child
#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

namespace hand {
namespace {

struct Session {
    HANDLE in = INVALID_HANDLE_VALUE;    ///< we write here → child's stdin
    HANDLE out = INVALID_HANDLE_VALUE;   ///< child's stdout → we read here
    HPCON pc = nullptr;
    HANDLE proc = INVALID_HANDLE_VALUE;
    DWORD pid = 0;
    bool used = false;
    /// Held only while the spawn worker runs, or for good if the spawn
    /// failed: then the error text sits in the pipe and the reader never
    /// sees EOF, so the tab stays open showing it (as a sync failure did).
    HANDLE out_w = INVALID_HANDLE_VALUE;
    COORD size{80, 24};                  ///< the latest size asked for (applied once pc exists)
};

// 64 concurrent ptys is far more than tabs anyone opens, and a fixed table
// means an `fd` stays valid for the process lifetime (no ABA on reuse within
// a frame). The mutex is taken only on spawn/close, never per read.
constexpr int kMaxSessions = 64;
std::array<Session, kMaxSessions> g_sessions{};
std::mutex g_mu;
/// The spawn worker per slot (conhost + CreateProcessW, ~50 ms): joined
/// before the slot is torn down.
std::array<std::thread, kMaxSessions> g_spawners{};

/// A pid stand-in while the shell is still being started: negative, so it
/// is never a real pid, and it names the slot so pty_reap can find it.
constexpr int pending_pid(int slot) { return -(slot + 2); }
constexpr int slot_of_pending(int pid) { return -pid - 2; }

Session* session_of(int fd) {
    if (fd < 0 || fd >= kMaxSessions) return nullptr;
    Session& s = g_sessions[static_cast<std::size_t>(fd)];
    return s.used ? &s : nullptr;
}

/// Quote one argv element the way the CRT's parser will unquote it.
/// Windows has no execv: the child gets a single string and splits it, so
/// hand must do the quoting that the POSIX side gets for free.
void append_arg(std::string& cmd, const std::string& arg) {
    if (!cmd.empty()) cmd.push_back(' ');
    const bool needs = arg.empty() || arg.find_first_of(" \t\n\v\"") != std::string::npos;
    if (!needs) {
        cmd += arg;
        return;
    }
    cmd.push_back('"');
    for (std::size_t i = 0;; ++i) {
        std::size_t slashes = 0;
        while (i < arg.size() && arg[i] == '\\') {
            ++i;
            ++slashes;
        }
        if (i == arg.size()) {
            cmd.append(slashes * 2, '\\');   // all escape the closing quote
            break;
        }
        if (arg[i] == '"') {
            cmd.append(slashes * 2 + 1, '\\');   // and escape the quote too
            cmd.push_back('"');
        } else {
            cmd.append(slashes, '\\');
            cmd.push_back(arg[i]);
        }
    }
    cmd.push_back('"');
}

/// The environment block for the child: ours, plus hand's overrides.
std::string make_env(const std::vector<std::pair<std::string, std::string>>& env) {
    std::vector<std::pair<std::string, std::string>> all = {
        {"TERM", "xterm-256color"}, {"COLORTERM", "truecolor"}, {"TERM_PROGRAM", "hand"}};
    for (const auto& kv : env) all.push_back(kv);

    std::string block;
    // Inherit everything we were given except the keys we are replacing and
    // the stale geometry COLUMNS/LINES, which confuse shells on resize.
    if (const LPWCH base = ::GetEnvironmentStringsW(); base != nullptr) {
        for (LPWCH p = base; *p != L'\0';) {
            const std::size_t wlen = ::wcslen(p);
            const int n = ::WideCharToMultiByte(CP_UTF8, 0, p, static_cast<int>(wlen), nullptr, 0, nullptr, nullptr);
            std::string entry(static_cast<std::size_t>(n), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, p, static_cast<int>(wlen), entry.data(), n, nullptr, nullptr);
            p += wlen + 1;

            const std::size_t eq = entry.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            std::string key = entry.substr(0, eq);
            std::string upper = key;
            for (char& c : upper) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
            if (upper == "COLUMNS" || upper == "LINES") continue;
            bool overridden = false;
            for (const auto& [k, v] : all)
                if (_stricmp(k.c_str(), key.c_str()) == 0) overridden = true;
            if (overridden) continue;
            block += entry;
            block.push_back('\0');
        }
        ::FreeEnvironmentStringsW(base);
    }
    for (const auto& [k, v] : all) {
        block += k;
        block.push_back('=');
        block += v;
        block.push_back('\0');
    }
    block.push_back('\0');   // the block's own terminator
    return block;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

Pty::~Pty() {
    if (fd_ < 0 || fd_ >= kMaxSessions) return;
    // The spawn worker writes the slot; let it finish first (outside g_mu,
    // which it takes).
    if (auto& t = g_spawners[static_cast<std::size_t>(fd_)]; t.joinable()) t.join();
    std::lock_guard lk{g_mu};
    Session* s = session_of(fd_);
    if (s == nullptr) return;
    if (s->out_w != INVALID_HANDLE_VALUE) ::CloseHandle(s->out_w);
    // Order matters: close our input end first so the shell sees EOF and
    // exits on its own, then the HPCON (which waits for conhost to drain),
    // then the handles. Closing the HPCON first can hang on a busy child.
    if (s->in != INVALID_HANDLE_VALUE) ::CloseHandle(s->in);
    if (s->pc != nullptr) ::ClosePseudoConsole(s->pc);
    if (s->out != INVALID_HANDLE_VALUE) ::CloseHandle(s->out);
    if (s->proc != INVALID_HANDLE_VALUE) {
        // The shell gets the chance to notice the closed pipe; if it is wedged
        // it is killed, because a leaked conhost outlives the window.
        if (::WaitForSingleObject(s->proc, 250) == WAIT_TIMEOUT) ::TerminateProcess(s->proc, 0);
        ::CloseHandle(s->proc);
    }
    *s = Session{};
}

std::string Pty::resolve_shell(std::string shell) {
    if (!shell.empty()) return shell;
    // No $SHELL on Windows. Prefer PowerShell 7 (pwsh) if the user has it,
    // then Windows PowerShell, then cmd — the same "best available" ladder a
    // Windows user expects, and all three drive ConPTY correctly.
    if (const char* s = std::getenv("HAND_SHELL"); s != nullptr && *s != '\0') return s;
    if (const char* c = std::getenv("COMSPEC"); c != nullptr && *c != '\0') {
        // COMSPEC is cmd.exe; only use it as the last resort below.
    }
    std::array<char, MAX_PATH> buf{};
    for (const wchar_t* cand : {L"pwsh.exe", L"powershell.exe"}) {
        wchar_t found[MAX_PATH];
        if (::SearchPathW(nullptr, cand, nullptr, MAX_PATH, found, nullptr) != 0) return narrow(found);
    }
    if (const char* c = std::getenv("COMSPEC"); c != nullptr && *c != '\0') return c;
    const UINT n = ::GetSystemDirectoryA(buf.data(), static_cast<UINT>(buf.size()));
    return n > 0 ? std::string(buf.data(), n) + "\\cmd.exe" : "cmd.exe";
}

namespace {
bool start_child(HPCON pc, const std::string& shell, const std::vector<std::pair<std::string, std::string>>& env,
                 const std::vector<std::string>& args, const std::string& cwd, HANDLE& proc_out, DWORD& pid_out);
}  // namespace

bool Pty::spawn(int cols, int rows, std::string shell, const std::vector<std::pair<std::string, std::string>>& env,
                const std::vector<std::string>& args, const std::string& cwd) {
    int slot = -1;
    {
        std::lock_guard lk{g_mu};
        for (int i = 0; i < kMaxSessions; ++i)
            if (!g_sessions[static_cast<std::size_t>(i)].used) {
                slot = i;
                g_sessions[static_cast<std::size_t>(i)].used = true;
                break;
            }
    }
    if (slot < 0) return false;
    Session& s = g_sessions[static_cast<std::size_t>(slot)];

    // Two pipes, crossed: conhost reads our input pipe and writes our output
    // pipe. We keep the far ends; the HPCON owns the near ones. Made HERE,
    // synchronously, so the pty is usable the moment spawn returns: the
    // reader can wait on `out`, keys typed early queue in `in`.
    HANDLE in_read = INVALID_HANDLE_VALUE, out_write = INVALID_HANDLE_VALUE;
    if (::CreatePipe(&in_read, &s.in, nullptr, 0) == FALSE || ::CreatePipe(&s.out, &out_write, nullptr, 0) == FALSE) {
        std::lock_guard lk{g_mu};
        if (in_read != INVALID_HANDLE_VALUE) ::CloseHandle(in_read);
        if (s.in != INVALID_HANDLE_VALUE) ::CloseHandle(s.in);
        s = Session{};
        return false;
    }
    s.out_w = out_write;
    s.size = COORD{static_cast<SHORT>(cols > 0 ? cols : 80), static_cast<SHORT>(rows > 0 ? rows : 24)};
    fd_ = slot;
    pid_ = static_cast<pid_t>(pending_pid(slot));

    // conhost (~20 ms) and the shell's process (~30 ms) start on a worker:
    // the window draws its first frame meanwhile instead of after them.
    g_spawners[static_cast<std::size_t>(slot)] =
        std::thread{[slot, in_read, shell = std::move(shell), env, args, cwd]() mutable {
            Session& ss = g_sessions[static_cast<std::size_t>(slot)];
            const auto fail = [&](const char* what) {
                // The message goes down the output pipe, so it shows in the
                // tab exactly where the shell's first prompt would have.
                const std::string msg = std::string{"\x1b[31mhand: could not start a shell ("} + what + ")\x1b[0m\r\n";
                DWORD w = 0;
                (void)::WriteFile(ss.out_w, msg.data(), static_cast<DWORD>(msg.size()), &w, nullptr);
                ::CloseHandle(in_read);
            };
            shell = resolve_shell(std::move(shell));
            HPCON pc = nullptr;
            COORD size;
            {
                std::lock_guard lk{g_mu};
                size = ss.size;
            }
            const HRESULT hr = ::CreatePseudoConsole(size, in_read, ss.out_w, 0, &pc);
            if (FAILED(hr) || pc == nullptr) return fail("CreatePseudoConsole");
            // conhost dup'd both ends; ours are dead weight now and MUST be
            // closed, or the child never sees EOF when the pty goes away.
            ::CloseHandle(in_read);
            HANDLE proc = INVALID_HANDLE_VALUE;
            DWORD pid = 0;
            const bool ok = start_child(pc, shell, env, args, cwd, proc, pid);
            std::lock_guard lk{g_mu};
            ss.pc = pc;
            if (ss.size.X != size.X || ss.size.Y != size.Y) (void)::ResizePseudoConsole(pc, ss.size);   // resized meanwhile
            if (!ok) {
                const std::string msg = "\x1b[31mhand: could not start a shell (" + shell + ")\x1b[0m\r\n";
                DWORD w = 0;
                (void)::WriteFile(ss.out_w, msg.data(), static_cast<DWORD>(msg.size()), &w, nullptr);
                return;   // out_w stays open: the message stays on screen
            }
            ss.proc = proc;
            ss.pid = pid;
            ::CloseHandle(ss.out_w);   // conhost holds its own copy; EOF now follows the shell
            ss.out_w = INVALID_HANDLE_VALUE;
        }};
    return true;
}

namespace {
/// Start `shell` attached to `pc`. Any thread.
bool start_child(HPCON pc, const std::string& shell, const std::vector<std::pair<std::string, std::string>>& env,
                 const std::vector<std::string>& args, const std::string& cwd, HANDLE& proc_out, DWORD& pid_out) {
    // The child is attached to the pty through a thread attribute, which is
    // how ConPTY replaces forkpty's "child inherits the slave side".
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    // Critical: with bInheritHandles = TRUE the child would otherwise inherit
    // OUR stdin/stdout/stderr and prefer them over the pseudoconsole -- the
    // shell's output would go to hand's own stdout (or wherever it was
    // redirected) and the pty would stay silent while every API reported
    // success. Handing it explicit NULL handles leaves the pseudoconsole as
    // the only console it has.
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nullptr;
    si.StartupInfo.hStdOutput = nullptr;
    si.StartupInfo.hStdError = nullptr;
    SIZE_T attr_size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attrs(attr_size);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
    if (::InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size) == FALSE) return false;
    if (::UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pc, sizeof pc,
                                    nullptr, nullptr) == FALSE) {
        ::DeleteProcThreadAttributeList(si.lpAttributeList);
        return false;
    }

    std::string cmd;
    append_arg(cmd, shell);
    for (const auto& a : args) append_arg(cmd, a);

    std::wstring wcmd = widen(cmd);
    wcmd.push_back(L'\0');   // CreateProcessW writes into this buffer
    const std::wstring wcwd = widen(cwd);

    // The environment block is a run of NUL-separated entries ending in a
    // double NUL, so it CANNOT go through widen(): that stops at the first
    // NUL and would hand the child a one-variable environment. Convert it
    // entry by entry, and keep it in a named local -- a temporary's buffer
    // would be freed before CreateProcessW ever read it.
    std::wstring wenv;
    {
        const std::string block = make_env(env);
        std::size_t i = 0;
        while (i < block.size() && block[i] != '\0') {
            const std::size_t len = block.find('\0', i) - i;
            wenv += widen(block.substr(i, len));
            wenv.push_back(L'\0');
            i += len + 1;
        }
        wenv.push_back(L'\0');   // the block's own terminator
    }

    PROCESS_INFORMATION pi{};
    // bInheritHandles MUST be TRUE: the pseudoconsole attribute attaches the
    // child to conhost's end of our pipes by handle inheritance. With FALSE
    // every call still reports success, but the child quietly inherits the
    // PARENT's console instead -- output goes to hand's own stdout and the
    // pty stays silent forever.
    const BOOL ok = ::CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE,
                                     EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, wenv.data(),
                                     wcwd.empty() ? nullptr : wcwd.c_str(), &si.StartupInfo, &pi);
    ::DeleteProcThreadAttributeList(si.lpAttributeList);
    if (ok == FALSE) return false;

    ::CloseHandle(pi.hThread);
    proc_out = pi.hProcess;
    pid_out = pi.dwProcessId;
    return true;
}
}  // namespace

void Pty::write(std::string_view bytes) const {
    Session* s = session_of(fd_);
    if (s == nullptr || s->in == INVALID_HANDLE_VALUE) return;
    while (!bytes.empty()) {
        DWORD wrote = 0;
        if (::WriteFile(s->in, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr) == FALSE) return;
        if (wrote == 0) return;   // pipe gone: drop, never hang the UI
        bytes.remove_prefix(wrote);
    }
}

void Pty::resize(int cols, int rows, int /*px_w*/, int /*px_h*/) const {
    std::lock_guard lk{g_mu};   // the spawn worker may be creating the HPCON
    Session* s = session_of(fd_);
    if (s == nullptr) return;
    // ConPTY has no pixel geometry; cells are all it models.
    const COORD size{static_cast<SHORT>(cols > 0 ? cols : 80), static_cast<SHORT>(rows > 0 ? rows : 24)};
    s->size = size;   // still starting: the worker applies it
    if (s->pc != nullptr) (void)::ResizePseudoConsole(s->pc, size);
}

bool pty_read(int fd, std::string& out, int timeout_ms) {
    Session* s = session_of(fd);
    if (s == nullptr || s->out == INVALID_HANDLE_VALUE) return false;

    // Only read what is actually buffered: ReadFile on a pipe blocks until it
    // gets at least one byte, and the reader thread must not block here (it
    // owns the stop check). The caller polls via plat::wait_readable.
    //
    // EOF: unlike a POSIX pty, ConPTY's output pipe does NOT break when the
    // shell exits -- conhost holds it open until ClosePseudoConsole -- so
    // `exit` left a dead tab (and a window that never closed). An empty pipe
    // whose shell has exited is EOF.
    const auto exited = [&] {
        const HANDLE p = s->proc;
        return p != INVALID_HANDLE_VALUE && ::WaitForSingleObject(p, 0) == WAIT_OBJECT_0;
    };
    const auto avail = [&]() -> DWORD {
        DWORD n = 0;
        if (::PeekNamedPipe(s->out, nullptr, 0, nullptr, &n, nullptr) == FALSE) return 0xFFFFFFFFu;
        if (n == 0 && exited()) {
            // The shell's last bytes may still be in flight through conhost:
            // look once more after it has had a moment to flush them.
            ::Sleep(15);
            if (::PeekNamedPipe(s->out, nullptr, 0, nullptr, &n, nullptr) == FALSE || n == 0) return 0xFFFFFFFFu;
        }
        return n;
    };

    DWORD n = avail();
    if (n == 0xFFFFFFFFu) return false;   // broken pipe: the child is gone
    if (n == 0) {
        if (timeout_ms <= 0) return true;   // nothing yet, still alive
        ::Sleep(static_cast<DWORD>(timeout_ms));
        n = avail();
        if (n == 0xFFFFFFFFu) return false;
        if (n == 0) return true;
    }

    char buf[65536];
    while (n > 0) {
        DWORD got = 0;
        const DWORD want = std::min<DWORD>(n, static_cast<DWORD>(sizeof buf));
        if (::ReadFile(s->out, buf, want, &got, nullptr) == FALSE || got == 0) return false;
        out.append(buf, got);
        if (out.size() >= (1u << 20)) return true;
        n = avail();
        if (n == 0xFFFFFFFFu) return true;   // drained what we had; report EOF next call
    }
    return true;
}

std::intptr_t pty_handle(int fd) {
    Session* s = session_of(fd);
    return s != nullptr && s->out != INVALID_HANDLE_VALUE ? reinterpret_cast<std::intptr_t>(s->out) : -1;
}

int pty_reap(pid_t pid) {
    // Find the session by pid so the process handle can be waited on; a bare
    // pid cannot be waited for without reopening it.
    std::lock_guard lk{g_mu};
    if (pid < -1) {
        // A pid captured while the shell was still starting (pending_pid):
        // the slot knows the real one by now. Not started yet, or the start
        // failed (its error stays on screen): still running.
        const int slot = slot_of_pending(static_cast<int>(pid));
        if (slot < 0 || slot >= kMaxSessions) return -1;
        const Session& ps = g_sessions[static_cast<std::size_t>(slot)];
        if (!ps.used || ps.proc == INVALID_HANDLE_VALUE) return -1;
        pid = static_cast<pid_t>(ps.pid);
    }
    for (Session& s : g_sessions) {
        if (!s.used || s.pid != static_cast<DWORD>(pid)) continue;
        DWORD code = 0;
        if (::GetExitCodeProcess(s.proc, &code) == FALSE) return -1;
        return code == STILL_ACTIVE ? -1 : static_cast<int>(code);
    }
    // Not ours (or already torn down): ask the OS directly.
    const HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h == nullptr) return 0;   // gone: treat as a clean exit
    DWORD code = 0;
    const BOOL ok = ::GetExitCodeProcess(h, &code);
    ::CloseHandle(h);
    if (ok == FALSE) return -1;
    return code == STILL_ACTIVE ? -1 : static_cast<int>(code);
}

pid_t pty_foreground(int fd) {
    // ConPTY has no notion of a foreground process group, so hand reports the
    // most recently started descendant of the shell instead: for "is a
    // program running right now" (the title, the ctrl-C hint) that is the
    // same answer in practice.
    Session* s = session_of(fd);
    if (s == nullptr || s->pid == 0) return -1;

    const HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof e;
    DWORD best = 0;
    ULONGLONG newest = 0;
    if (::Process32FirstW(snap, &e) != FALSE) {
        do {
            if (e.th32ParentProcessID != s->pid) continue;
            const HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
            if (h == nullptr) continue;
            FILETIME c{}, x{}, k{}, u{};
            if (::GetProcessTimes(h, &c, &x, &k, &u) != FALSE) {
                const ULONGLONG t = (static_cast<ULONGLONG>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
                if (t >= newest) {
                    newest = t;
                    best = e.th32ProcessID;
                }
            }
            ::CloseHandle(h);
        } while (::Process32NextW(snap, &e) != FALSE);
    }
    ::CloseHandle(snap);
    return best != 0 ? static_cast<pid_t>(best) : static_cast<pid_t>(s->pid);
}

std::string proc_name(pid_t pid) {
    if (pid <= 0) return {};
    const HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h == nullptr) return {};
    wchar_t path[MAX_PATH] = {};
    DWORD n = MAX_PATH;
    const BOOL ok = ::QueryFullProcessImageNameW(h, 0, path, &n);
    ::CloseHandle(h);
    if (ok == FALSE || n == 0) return {};

    std::string full = narrow(std::wstring(path, n));
    const std::size_t slash = full.find_last_of("\\/");
    std::string name = slash == std::string::npos ? full : full.substr(slash + 1);
    // hand's callers compare against "ssh", "nvim": drop the .exe so the
    // POSIX-shaped comparisons in the app keep working unchanged.
    if (name.size() > 4) {
        const std::string tail = name.substr(name.size() - 4);
        if (_stricmp(tail.c_str(), ".exe") == 0) name.resize(name.size() - 4);
    }
    return name;
}

std::vector<std::string> proc_argv(pid_t pid) {
    // A full argv needs the remote PEB (NtQueryInformationProcess +
    // ReadProcessMemory), which needs rights hand does not want to require.
    // The image name is what every caller actually uses, so report argv[0].
    std::vector<std::string> out;
    std::string n = proc_name(pid);
    if (!n.empty()) out.push_back(std::move(n));
    return out;
}

void spawn_detached(const std::vector<std::string>& argv) {
    if (argv.empty() || argv[0].empty()) return;
    std::string cmd;
    for (const auto& a : argv) append_arg(cmd, a);
    std::wstring wcmd = widen(cmd);
    wcmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;   // no console flash when opening a link
    PROCESS_INFORMATION pi{};
    // DETACHED_PROCESS + BREAKAWAY: the child outlives hand and never shares
    // our console or job, which is the double-fork/setsid equivalent.
    if (::CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE,
                         DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr,
                         &si, &pi) == FALSE) {
        // A job object may forbid breakaway; retry without it.
        wcmd = widen(cmd);
        wcmd.push_back(L'\0');
        if (::CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE,
                             DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi) == FALSE)
            return;
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);   // never waited on: no zombie to reap
}

}  // namespace hand

#endif  // _WIN32
