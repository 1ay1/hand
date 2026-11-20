#pragma once
// hand::proc — the OS services the SSH manager needs, behind one small seam.
//
//   run(argv, in, ms)     a child process, stdout/stderr captured, killed at
//                         the timeout (sftp listings, transfers)
//   tcp_listeners()       every listening TCP port and who owns it (live
//                         tunnel status: is ssh's -L 5432 really up?)
//   putty_sessions(root)  PuTTY's saved sessions (registry on Windows,
//                         ~/.putty/sessions elsewhere), raw key=value
//   pick_files(title)     the OS file picker (Windows); empty if cancelled
//   downloads_dir()       where downloads go
//   reveal(path)          show a file in the OS file manager
//
// POSIX in proc_posix.cpp, Windows in proc_win.cpp; nothing above this
// header includes an OS header.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace hand::proc {

struct Result {
    bool        started = false;   ///< the program could be run at all
    bool        timed_out = false;
    int         code = -1;
    std::string out, err;
};
/// Run argv[0] (looked up on PATH) with argv[1..], feeding `in` to stdin.
[[nodiscard]] Result run(const std::vector<std::string>& argv, std::string_view in = {}, int timeout_ms = 30000);

struct Listener {
    int         port = 0;
    int         pid = 0;
    std::string process;           ///< short name ("ssh"), "" if unknown
};
/// Listening TCP sockets (IPv4 and IPv6). `ok` false: not knowable here.
[[nodiscard]] std::vector<Listener> tcp_listeners(bool* ok = nullptr);

struct RawSession {
    std::string name;              ///< decoded ("My server")
    std::map<std::string, std::string> values;   ///< DWORDs as decimal
};
/// PuTTY's sessions under `root` ("" = PuTTY's own location).
[[nodiscard]] std::vector<RawSession> putty_sessions(const std::string& root = {});

[[nodiscard]] std::vector<std::string> pick_files(const std::string& title);
[[nodiscard]] std::string downloads_dir();
void reveal(const std::string& path);
/// A fresh temp file path (not created).
[[nodiscard]] std::string temp_path(std::string_view stem);

}  // namespace hand::proc
