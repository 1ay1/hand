// hand::cli — the world a run starts in, read once (App::Flags::env).
//
// The ONLY place hand reads the environment for the app: update and view see
// these values in m.env and never ask the OS (tests/purity.cmake).

#include "cli.hpp"

#include <hand/core/platform.hpp>
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
    e.home = plat::home_dir();   // ~ for hand and for ssh, one definition (core/platform.hpp)
    e.editor = var("VISUAL");
    if (e.editor.empty()) e.editor = var("EDITOR");
    if (e.editor.empty()) e.editor = "vi";
    e.shell = Pty::resolve_shell("");
    e.host_name = proc::host_name();
    std::error_code ec;
    e.cwd = std::filesystem::current_path(ec).generic_string();
    e.config_path = config_path();
    // The inbox ConPTY repaints its viewport on every resize: it hears a
    // drag's size once it holds 150 ms (measured: 60-100 still doubled lines
    // on some drags; 150 never did). The modern ConPTY beside hand.exe does
    // not repaint: at once. HAND_RESIZE_SETTLE_MS overrides (measuring).
    e.resize_settle_ms = plat::pty_repaints_on_resize() ? 150 : 0;
    if (const std::string s = var("HAND_RESIZE_SETTLE_MS"); !s.empty()) e.resize_settle_ms = std::atoi(s.c_str());
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
