// SPDX-License-Identifier: LGPL-2.0-or-later
//
// conpty — process creation on Windows, the host's job.
//
// The exact sibling of posix_pty.cpp: it creates the child on a pseudoconsole
// and hands back a toe::AdoptFd the engine adopts, with ZERO change to toe. On
// POSIX that fd is a forkpty master; here it is an index into toe's ConPTY
// registry (toe/pty/win_io.hpp), which owns the real HANDLEs.
//
// ─── the channels must be SYNCHRONOUS ───────────────────────────────
// ConPTY services its pipes with synchronous ReadFile/WriteFile and explicitly
// does not support channels that require an OVERLAPPED structure. Handing it a
// CreateNamedPipe(FILE_FLAG_OVERLAPPED) output pipe — tempting, because it would
// remove the reader thread — makes the pseudoconsole fail to bind the child:
// the pty emits only conhost's initial screen paint, and the child's real output
// goes to whatever console it inherited. So we use plain CreatePipe, and the
// blocking read lives on one reader thread inside toe's ConPTY registry
// (toe/pty/win_io.hpp), which converts it back into an event the frontend's
// single wait can block on.

#include "hand/platform/posix_pty.hpp"
#include "toe/pty/win_io.hpp"

#include <cstdio>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace hand {

namespace {

// UTF-8 -> UTF-16, for the Win32 -W entry points. The whole app is UTF-8
// internally (as toe's VT layer is); the boundary conversion lives here.
[[nodiscard]] std::wstring widen(const std::string &s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                        nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Quote one argv element per the CommandLineToArgvW rules, so a path with
// spaces (the common case: "C:\Program Files\...") survives round-tripping.
void append_arg(std::wstring &out, const std::wstring &arg) {
    if (!out.empty()) out += L' ';
    if (arg.find_first_of(L" \t\"") == std::wstring::npos) {
        out += arg;
        return;
    }
    out += L'"';
    std::size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
        } else {
            out.append(backslashes, L'\\');
            out += c;
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
}

// Resolve the shell when the user gave no explicit argv:
//   cmd.argv -> %COMSPEC% -> PowerShell -> cmd.exe
// PowerShell is preferred over cmd only if COMSPEC is unset, which is rare.
[[nodiscard]] std::string resolve_shell(const SpawnCommand &cmd) {
    if (!cmd.argv.empty()) return cmd.argv.front();
    if (const char *c = std::getenv("COMSPEC"); c && *c) return c;
    return "cmd.exe";
}

// Close a handle and null it, so the error paths below stay readable.
void drop(HANDLE &h) {
    if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
    h = nullptr;
}

} // namespace

toe::Result<toe::AdoptFd> spawn_pty(const SpawnCommand &cmd) {
    // --- 1. the two pipes -----------------------------------------------------
    // Plain, synchronous, anonymous pipes — the shape ConPTY requires (see the
    // header note). Default buffer size: ConPTY writes in screen-sized bursts.
    HANDLE in_read = nullptr, in_write = nullptr;
    HANDLE out_read = nullptr, out_write = nullptr;
    if (!::CreatePipe(&in_read, &in_write, nullptr, 0)) {
        return toe::fail("CreatePipe (conpty in) failed");
    }
    if (!::CreatePipe(&out_read, &out_write, nullptr, 0)) {
        drop(in_read);
        drop(in_write);
        return toe::fail("CreatePipe (conpty out) failed");
    }

    // --- 2. the pseudoconsole ------------------------------------------------
    // The initial size is a placeholder; toe resizes to the real grid as soon as
    // the Terminal is created (exactly as the POSIX path does).
    HPCON hpcon = nullptr;
    // Create the pseudoconsole at the REAL grid size when the host knows it.
    // ConPTY repaints its entire viewport on every resize, so being born at the
    // right size is what stops the child painting twice (see SpawnCommand::cols).
    const COORD size{static_cast<SHORT>(cmd.cols > 0 ? cmd.cols : 80),
                     static_cast<SHORT>(cmd.rows > 0 ? cmd.rows : 24)};
    if (FAILED(::CreatePseudoConsole(size, in_read, out_write, 0, &hpcon))) {
        drop(out_read);
        drop(out_write);
        drop(in_read);
        drop(in_write);
        return toe::fail("CreatePseudoConsole failed (needs Windows 10 1809+)");
    }
    // NOTE: out_write / in_read are deliberately still OPEN here. The docs are
    // explicit that the child-side handles must be released only AFTER
    // CreateProcess ("Upon completion of the CreateProcess call ... the handles
    // given during creation should be freed from this process"). Closing them
    // early tears the channel down before the child attaches, so the pty emits
    // only conhost's initial paint and then goes silent.

    // --- 3. the child --------------------------------------------------------
    std::wstring cmdline;
    std::vector<std::string> args = cmd.argv;
    if (args.empty()) args.emplace_back(resolve_shell(cmd));
    for (const auto &a : args) append_arg(cmdline, widen(a));
    if (const char *dbg = std::getenv("HAND_CONPTY_DEBUG"); dbg && *dbg) {
        std::fprintf(stderr, "conpty cmdline: [%ls]\n", cmdline.c_str());
    }

    // PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE is what makes the spawned process see
    // the pseudoconsole as its real console (so it allocates a proper conhost
    // state, and TUI apps behave).
    //
    // NOTE the call order. InitializeProcThreadAttributeList is invoked TWICE by
    // design: once with a null list purely to LEARN the required byte size, and
    // once on the real allocation to initialise it. Calling it a third time (or
    // re-initialising after UpdateProcThreadAttribute) RESETS the list to empty
    // and silently discards the pseudoconsole attribute — the child then binds
    // to the parent's console instead of the pty, and the terminal renders a
    // blank grid while the child's output appears in whatever console launched
    // hand.
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    // Setting STARTF_USESTDHANDLES while leaving hStdInput/hStdOutput/hStdError
    // NULL is the documented way to ensure the child inherits NO handles from
    // us. Without it the child picks up our std handles / console and writes
    // there instead of into the pseudoconsole — the terminal then shows only
    // conhost's initial screen paint while the child's real output appears in
    // whatever console launched hand. (Same approach Alacritty uses.)
    si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;

    SIZE_T attr_size = 0;
    // Sizing probe: expected to fail with ERROR_INSUFFICIENT_BUFFER.
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buf(attr_size);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());

    if (!::InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size)) {
        ::ClosePseudoConsole(hpcon);
        drop(out_read);
        drop(in_write);
        return toe::fail("InitializeProcThreadAttributeList failed");
    }
    if (!::UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                     hpcon, sizeof(hpcon), nullptr, nullptr)) {
        ::DeleteProcThreadAttributeList(si.lpAttributeList);
        ::ClosePseudoConsole(hpcon);
        drop(out_read);
        drop(in_write);
        return toe::fail("UpdateProcThreadAttribute(PSEUDOCONSOLE) failed");
    }

    // TERM and friends: the child inherits our environment, and we publish the
    // configured TERM so ncurses-style apps agree with what toe emulates.
    if (!cmd.term.empty()) ::SetEnvironmentVariableW(L"TERM", widen(cmd.term).c_str());

    PROCESS_INFORMATION pi{};
    // bInheritHandles is FALSE by design: the pseudoconsole attribute (not handle
    // inheritance) is what binds the child to the ConPTY.
    //
    // EXTENDED_STARTUPINFO_PRESENT is the ONLY flag. Two others are tempting and
    // both break the binding:
    //   • CREATE_UNICODE_ENVIRONMENT declares that lpEnvironment is a UTF-16
    //     block — but we pass nullptr (inherit ours), so the flag describes an
    //     environment that isn't there and the child fails to attach to the pty.
    //   • CREATE_NO_WINDOW / CREATE_NEW_CONSOLE are console-creation flags that
    //     contradict PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE outright.
    // In both cases the symptom is identical and misleading: the pty emits only
    // conhost's initial paint while the child's real output appears in whatever
    // console launched hand.
    const BOOL ok = ::CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr,
                                     /*bInheritHandles=*/FALSE,
                                     EXTENDED_STARTUPINFO_PRESENT,
                                     nullptr, nullptr, &si.StartupInfo, &pi);
    ::DeleteProcThreadAttributeList(si.lpAttributeList);

    if (!ok) {
        ::ClosePseudoConsole(hpcon);
        drop(out_read);
        drop(in_write);
        return toe::fail("CreateProcess failed for: " + args.front());
    }

    ::CloseHandle(pi.hThread); // we only track the process

    // NOTE: out_write / in_read are NOT closed. CreatePseudoConsole takes
    // ownership of the pty-side ends and services them for the device's
    // lifetime; ClosePseudoConsole releases them at teardown. Closing them here
    // races the pseudoconsole and can deadlock ClosePseudoConsole, which blocks
    // until the conout pipe is drained.

    // --- 4. hand it to the engine -------------------------------------------
    // The registry takes ownership of all four handles and arms the first
    // overlapped read; the int it returns IS the AdoptFd master_fd.
    const int fd = toe::win::register_pty(out_read, in_write, hpcon, pi.hProcess,
                                          size.X, size.Y);
    if (fd < 0) {
        ::ClosePseudoConsole(hpcon);
        drop(out_read);
        drop(in_write);
        ::CloseHandle(pi.hProcess);
        return toe::fail("toe::win::register_pty failed");
    }

    // child is carried as an opaque id: exit detection on Windows goes through
    // the process HANDLE in the registry slot, reached via the fd.
    return toe::AdoptFd{.master_fd = fd,
                        .child = static_cast<toe::pid_type>(pi.dwProcessId),
                        .owns_fd = true};
}

} // namespace hand
