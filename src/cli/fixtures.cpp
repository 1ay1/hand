// hand::cli — hand as its own test fixture: tiny programs the self-test runs
// on a pty, so a UI test never depends on coreutils or on a shell.
//
//   hand --emit-red   print red text and exit 7 (colour through a ConPTY)
//   hand --cat-v      raw mode + mouse reporting, echo input in caret notation
//   hand --cat        `cat`: copy stdin to stdout (the tty echoes each key) -- the echo_latency program
//   hand --yes        `yes`: "y" lines as fast as the pty takes them -- the flood
//
// --cat and --yes make the perf gates one program on every OS: Windows has no
// coreutils, so they used to be skipped there, and its pty path went unmeasured.

#include <mayag/core/target.hpp>
#include <hand/core/platform.hpp>
#include "cli.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if MAYAG_OS_WINDOWS
#  include <windows.h>   // --cat-v puts the console in raw mode
#else
#  include <termios.h>
#  include <unistd.h>
#endif

namespace hand::cli {

using namespace mayag;

std::optional<int> fixture(int argc, char** argv) {
    // ── --emit-red: the colour-through-the-pty fixture ──────────────────
    // Writing SGR from a program's own stdout is the only reliable way to
    // get colour through a ConPTY: escapes typed on stdin are eaten by the
    // shell's line editor, and conhost re-renders colour from its screen
    // buffer rather than passing the original bytes through. This is the
    // Windows stand-in for `printf '\033[31mred\033[0m'` under /bin/sh.
    if (argc >= 2 && std::string_view{argv[1]} == "--emit-red") {
#if MAYAG_OS_WINDOWS
        const HANDLE hout = ::GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD m = 0;
        ::GetConsoleMode(hout, &m);
        ::SetConsoleMode(hout, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
        std::fputs("\x1b[31mred\x1b[0m\r\n", stdout);
        std::fflush(stdout);
        return 7;   // the exit code the test checks for
    }

    // ── --cat-v: hand as its own test fixture ───────────────────────────
    // The mouse tests need a program that puts the tty in raw mode, enables
    // mouse reporting, and echoes every byte it receives in caret notation.
    // That used to be `sh -c 'stty raw -echo; ... exec cat -v'`, which does
    // not exist on Windows -- and even on Linux it made a UI test depend on
    // coreutils. hand is already on the pty, so it plays the part itself.
    if (argc >= 2 && std::string_view{argv[1]} == "--cat-v") {
#if MAYAG_OS_WINDOWS
        // Raw mode: no line buffering, no echo, and crucially no
        // ENABLE_PROCESSED_INPUT, so ctrl-C arrives as a byte, not a signal.
        const HANDLE hin = ::GetStdHandle(STD_INPUT_HANDLE);
        const HANDLE hout = ::GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD in_mode = 0, out_mode = 0;
        ::GetConsoleMode(hin, &in_mode);
        ::GetConsoleMode(hout, &out_mode);
        ::SetConsoleMode(hin, (in_mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT)) |
                                  ENABLE_VIRTUAL_TERMINAL_INPUT);
        ::SetConsoleMode(hout, out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
        termios t{};
        if (::tcgetattr(STDIN_FILENO, &t) == 0) {
            termios raw = t;
            ::cfmakeraw(&raw);
            ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }
#endif
        // Announce readiness and ask for any-motion + SGR mouse reporting,
        // exactly as the old printf did.
        std::fputs("ready\x1b[?1003h\x1b[?1006h\r\n", stdout);
        std::fflush(stdout);

        // Echo in caret notation: ESC as "^[", other controls as "^X".
        for (int c = std::getchar(); c != EOF; c = std::getchar()) {
            const auto u = static_cast<unsigned char>(c);
            if (u == 0x1b) std::fputs("^[", stdout);
            else if (u < 0x20) { std::putchar('^'); std::putchar(static_cast<char>(u + '@')); }
            else if (u == 0x7f) std::fputs("^?", stdout);
            else std::putchar(static_cast<char>(u));
            std::fflush(stdout);   // the test reads this back through a pty
        }
        return 0;
    }

    // ── --cat / --yes: coreutils' two, the same bytes on every OS ───────
    if (argc >= 2 && std::string_view{argv[1]} == "--cat") {
        // The terminal's own echo (the tty line discipline; conhost's cooked
        // read) shows each key; the line comes back whole after enter. One
        // OS read per chunk, never stdio's fill-the-buffer fread.
        std::array<char, 4096> b{};
        for (;;) {
            const long n = plat::read_stdin(b.data(), b.size());
            if (n <= 0) break;
            plat::write_stdout(b.data(), static_cast<std::size_t>(n));
        }
        return 0;
    }
    if (argc >= 2 && std::string_view{argv[1]} == "--yes") {
        std::string chunk;   // one big write per call, as GNU yes does
        for (int i = 0; i < 8192; ++i) chunk += "y\n";
        while (plat::write_stdout(chunk.data(), chunk.size())) {}
        return 0;
    }
    return std::nullopt;
}

}  // namespace hand::cli
