#pragma once
// hand::cli — hand's run modes besides the window, one file each. main.cpp
// parses the arguments and hands over; nothing here is linked into the app
// layer, and the app layer never knows these exist.
//
//   fixtures.cpp   --emit-red, --cat-v           hand as a test fixture on a pty
//   repro.cpp      --repro-narrow[-fish], --repro-resize   resize repros, real shells
//   selftest.cpp   --headless                    the self-test, `ok` / `FAIL` per check
//   shots.cpp      --png --shot NAME, --bench, --flood     a frame to PNG, benchmarks
//   env.cpp        the world a run starts in (App::Flags::env), read once

#include <hand/app.hpp>

#include <mayag/mayag.hpp>

#include <optional>
#include <string>

namespace hand::cli {

/// $HOME, $EDITOR, $SHELL, the host name, the start directory, hand.vibe's
/// path, the wall clock and a seed. `scripted`: a fixed seed (shots repeat).
[[nodiscard]] Env read_env(bool scripted);

/// The headless runtime every scripted mode drives.
using Runtime = mayag::headless_app<App>;

/// A fixture program, when argv asks for one: its exit code.
[[nodiscard]] std::optional<int> fixture(int argc, char** argv);
/// A resize repro, when argv asks for one: 0 = intact, 1 = corrupted.
[[nodiscard]] std::optional<int> repro(int argc, char** argv);
/// The self-test, on a runtime showing the demo screen. Returns the failures.
[[nodiscard]] int selftest(Runtime& rt, mayag::Vec2 size);
/// Stage `shot`, render one frame, write it to `out` (bench / flood print stats).
[[nodiscard]] int shoot(Runtime& rt, mayag::Vec2 size, const std::string& shot, const std::string& out);

/// A screenshot request: the scene and the file it goes to.
struct Shot { std::string name; std::string out; };
/// The scripted modes on a headless runtime showing the demo screen: the
/// self-test (no shot) or one shot. Returns the exit code.
[[nodiscard]] int run_scripted(mayag::AppConfig ac, App::Flags flags, mayag::Vec2 size, std::optional<Shot> shot);

/// Has the cursor's motion (and its particles) come to rest?
[[nodiscard]] inline bool motion_settled(const App::Model& m) { return motion::tail_settled(m.tab().fx, m.tab().cur_x, m.tab().cur_y); }

}  // namespace hand::cli
