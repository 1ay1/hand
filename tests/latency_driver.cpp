// Input-to-photon driver for the Windows build.
//
// The latency that decides how "instant" a terminal feels is keystroke -> the
// photon showing its echo. It cannot be measured by a CPU-side harness: it
// includes the PTY round trip, the parse, the GPU frame, and the compositor.
//
// So this drives the REAL app: it finds hand's window, synthesizes genuine
// WM_CHAR-producing key events with SendInput (the same path a physical
// keyboard takes), and lets hand's own LatencyMeter record input->present.
// Nothing here estimates anything; it only supplies the keystrokes.
//
// Usage: latency_driver.exe [keystrokes] [delay_ms]
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

static HWND find_hand_window() {
    HWND best = nullptr;
    ::EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            wchar_t cls[256] = {};
            ::GetClassNameW(h, cls, 255);
            if (::wcscmp(cls, L"hand.terminal.window") == 0 && ::IsWindowVisible(h)) {
                *reinterpret_cast<HWND *>(lp) = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&best));
    return best;
}

// Windows refuses SetForegroundWindow from a process that doesn't own the
// foreground (anti-focus-stealing). The documented workaround is to attach our
// input queue to the foreground thread's, which makes us a legitimate caller
// for the duration.
static bool force_foreground(HWND hwnd) {
    if (::GetForegroundWindow() == hwnd) return true;

    const DWORD fg_tid = ::GetWindowThreadProcessId(::GetForegroundWindow(), nullptr);
    const DWORD our_tid = ::GetCurrentThreadId();
    const DWORD tgt_tid = ::GetWindowThreadProcessId(hwnd, nullptr);

    ::AttachThreadInput(our_tid, fg_tid, TRUE);
    ::AttachThreadInput(our_tid, tgt_tid, TRUE);

    ::ShowWindow(hwnd, SW_RESTORE);
    ::BringWindowToTop(hwnd);
    ::SetForegroundWindow(hwnd);
    ::SetActiveWindow(hwnd);
    ::SetFocus(hwnd);

    ::AttachThreadInput(our_tid, tgt_tid, FALSE);
    ::AttachThreadInput(our_tid, fg_tid, FALSE);

    for (int i = 0; i < 20 && ::GetForegroundWindow() != hwnd; ++i) ::Sleep(50);
    return ::GetForegroundWindow() == hwnd;
}

int main(int argc, char **argv) {
    const int count = (argc > 1) ? std::atoi(argv[1]) : 200;
    const int delay = (argc > 2) ? std::atoi(argv[2]) : 40;

    HWND hwnd = find_hand_window();
    if (!hwnd) {
        std::printf("SKIP: hand window not found\n");
        return 77;
    }

    // Focus it: SendInput goes to the FOREGROUND window, not a specific HWND.
    if (!force_foreground(hwnd)) {
        std::printf("SKIP: could not foreground hand (click it, then rerun)\n");
        return 77;
    }
    ::Sleep(300); // let the focus change settle before typing

    std::printf("driving %d keystrokes at %d ms intervals...\n", count, delay);

    // Type printable characters. Each is a real key-down/key-up pair, so it
    // travels the identical path as a physical keypress: WM_KEYDOWN ->
    // TranslateMessage -> WM_CHAR -> pty write -> child echo -> parse -> frame.
    for (int i = 0; i < count; ++i) {
        const WORD ch = static_cast<WORD>('a' + (i % 26));
        INPUT in[2] = {};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wScan = ch;
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1] = in[0];
        in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
        ::SendInput(2, in, sizeof(INPUT));
        ::Sleep(static_cast<DWORD>(delay));
    }

    // Clear the typed line so the shell isn't left with junk.
    INPUT esc[2] = {};
    esc[0].type = INPUT_KEYBOARD;
    esc[0].ki.wVk = VK_ESCAPE;
    esc[1] = esc[0];
    esc[1].ki.dwFlags = KEYEVENTF_KEYUP;
    ::SendInput(2, esc, sizeof(INPUT));

    std::printf("done; see the HUD log for input->photon stats\n");
    return 0;
}
