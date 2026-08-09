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
#include <fstream>
#include <thread>
#include <chrono>
#include <poll.h>
static bool wait_and_drain(hand::ConfigWatch& w, int ms){
  pollfd p{w.fd(), POLLIN, 0};
  if (poll(&p, 1, ms) <= 0) return false;
  return w.drained();
}
int main(){
    const char* dir="/tmp/hand_cw_test";
    const char* path="/tmp/hand_cw_test/config.vibe";
    system("rm -rf /tmp/hand_cw_test; mkdir -p /tmp/hand_cw_test");
    (void)dir;
  { std::ofstream(path) << "font { size 13 }\n"; }
  hand::ConfigWatch w;
  if(!w.start(path)){ std::printf("FAIL: start\n"); return 1; }
  std::printf("watch fd=%d active=%d\n", w.fd(), w.active());

  // 1) In-place write.
  { std::ofstream(path, std::ios::app) << "# edited\n"; }
  bool got1 = wait_and_drain(w, 1000);
  std::printf("in-place write detected: %d\n", got1);

  // 2) Atomic-rename save (editor style: write temp, rename over).
  { std::ofstream("/tmp/hand_cw_test/.tmp") << "font { size 20 }\n"; }
  system("mv /tmp/hand_cw_test/.tmp /tmp/hand_cw_test/config.vibe");
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
