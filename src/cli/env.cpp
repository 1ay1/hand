// hand::cli — the world a run starts in, read once (App::Flags::env).
//
// The ONLY place hand reads the environment for the app: update and view see
// these values in m.env and never ask the OS (tests/purity.cmake).

#include "cli.hpp"

#include <hand/core/proc.hpp>
#include <hand/core/pty.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <random>

namespace hand::cli {

Env read_env(bool scripted) {
    Env e;
    const auto var = [](const char* k) -> std::string {
        const char* v = std::getenv(k);
        return v && *v ? v : "";
    };
#if defined(_WIN32)
    e.home = var("USERPROFILE");   // ssh's ~ on Windows, whatever $HOME says (MSYS2 points it at AppData)
    for (char& ch : e.home) if (ch == '\\') ch = '/';
#else
    e.home = var("HOME");
#endif
    while (e.home.size() > 1 && e.home.back() == '/') e.home.pop_back();
    e.editor = var("VISUAL");
    if (e.editor.empty()) e.editor = var("EDITOR");
    if (e.editor.empty()) e.editor = "vi";
    e.shell = Pty::resolve_shell("");
    e.host_name = proc::host_name();
    std::error_code ec;
    e.cwd = std::filesystem::current_path(ec).generic_string();
    e.config_path = config_path();
    e.wall_epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count();
    // scripted runs are reproducible: a fixed seed and a fixed epoch
    if (scripted) {
        e.entropy = 0x68616E64;   // "hand"
    } else {
        std::random_device rd;
        e.entropy = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    }
    return e;
}

}  // namespace hand::cli
