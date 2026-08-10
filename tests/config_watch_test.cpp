// SPDX-License-Identifier: LGPL-2.0-or-later
//
// config_watch_test — the inotify config watcher must detect BOTH save styles
// (in-place append AND the atomic write-temp-then-rename editors use) and must
// suppress the echo of hand's own writes. This is the correctness that makes
// live file-reload reliable rather than flaky; a regression here silently
// breaks hot-reload for half the editors on the planet.
#include "hand/config_watch.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// Windows exposes a waitable EVENT rather than a pollable fd; block on that.
static bool wait_and_drain(hand::ConfigWatch& w, int ms){
  HANDLE h = static_cast<HANDLE>(w.event_handle());
  if (!h) return false;
  if (::WaitForSingleObject(h, static_cast<DWORD>(ms)) != WAIT_OBJECT_0) return false;
  return w.drained();
}
#else
#include <poll.h>
static bool wait_and_drain(hand::ConfigWatch& w, int ms){
  pollfd p{w.fd(), POLLIN, 0};
  if (poll(&p, 1, ms) <= 0) return false;
  return w.drained();
}
#endif

int main(){
  // A real temp dir on both platforms, rather than a hardcoded /tmp path.
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "hand_cw_test";
  const std::filesystem::path cfg = dir / "config.vibe";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  const std::string path = cfg.string();

  { std::ofstream(path) << "font { size 13 }\n"; }
  hand::ConfigWatch w;
  if(!w.start(path)){ std::printf("FAIL: start\n"); return 1; }
  std::printf("watch fd=%d active=%d\n", w.fd(), w.active());

  // 1) In-place write.
  { std::ofstream(path, std::ios::app) << "# edited\n"; }
  bool got1 = wait_and_drain(w, 1000);
  std::printf("in-place write detected: %d\n", got1);

  // 2) Atomic-rename save (editor style: write temp, rename over).
  const std::filesystem::path tmp = dir / ".tmp";
  { std::ofstream(tmp.string()) << "font { size 20 }\n"; }
  std::filesystem::rename(tmp, cfg, ec);
  bool got2 = wait_and_drain(w, 1000);
  std::printf("atomic-rename save detected: %d\n", got2);

  // 3) Self-write is suppressed.
  w.note_self_write();
  { std::ofstream(path, std::ios::app) << "# self\n"; }
  bool got3 = wait_and_drain(w, 1000);
  std::printf("self-write suppressed (want 0): %d\n", got3);

  int fails = (!got1) + (!got2) + (got3);
  std::printf(fails? "%d FAIL\n":"ALL WATCH CASES PASS\n", fails);
  return fails?1:0;
}
