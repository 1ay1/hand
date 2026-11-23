// hand::cli::checks — remote files over a REAL ssh connection.
//
// Opt-in: `HAND_TEST_SSH=localhost ./build/hand --headless` (any host that
// takes key login). Everything else in the self-test is the demo, which never
// touches a network; this suite drives the editor's remote effects for real:
// read, save (atomic, permissions kept), stat, an outside change, a project
// listing and a search -- all over one shared connection, and timed.

#include "suites.hpp"

#include <hand/app.hpp>
#include <hand/core/proc.hpp>
#include <hand/ssh/remote.hpp>

#include "../../app/fx/fx.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace hand::cli::checks {

namespace {

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

void remote_files(Runtime&, mayag::Vec2, Check& ok) {
    const char* target = std::getenv("HAND_TEST_SSH");
    if (target == nullptr || *target == '\0') return;   // opt-in
    std::printf("  -- remote files over ssh to %s\n", target);
    Host h;
    h.alias = target;
    h.hostname = target;
    h.source = HostSource::history;   // not from ~/.ssh/config: hand builds the destination
    const HostBook book{};
    const std::string dir = "/tmp/hand-remote-test-" + std::to_string(self_pid());
    const std::string file = dir + "/notes.sh";
    std::filesystem::create_directories(dir);   // localhost: the same filesystem, so the test can look
    proc::run({"sh", "-c", "printf 'echo one\\necho two\\n' > " + file + " && chmod 755 " + file + " && cd " + dir +
                               " && git init -q && git add notes.sh"}, {}, 5000);

    const auto t0 = std::chrono::steady_clock::now();
    ok(remote::warm(h, book).empty(), "the host's shared connection comes up (key login)");
    std::printf("       connect: %.0f ms\n", ms_since(t0));

    namespace ro = fx::remote_ops;
    auto t = std::chrono::steady_clock::now();
    const auto ld = ro::read_file(SessionId{1}, h, book, file, false);
    const double read_ms = ms_since(t);
    const auto* loaded = &ld;
    ok(loaded->error.empty() && loaded->text == "echo one\necho two\n" && loaded->mtime != 0,
       "a remote file is read whole, with its change token");
    std::printf("       read: %.0f ms (over the shared connection)\n", read_ms);

    t = std::chrono::steady_clock::now();
    const auto sv = ro::write_file(SessionId{1}, h, book, file, "echo one\necho TWO\n", 7);
    const double save_ms = ms_since(t);
    const auto* saved = &sv;
    ok(saved->error.empty() && saved->point == 7 && saved->mtime != 0, "a save lands and reports its new token");
    std::printf("       save: %.0f ms\n", save_ms);
    {
        std::error_code ec;
        const auto perms = std::filesystem::status(file, ec).permissions();
        ok((perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none, "...keeping the file executable");
        ok(!std::filesystem::exists(file + ".hand-save~"), "...leaving no temp file behind");
        const auto r = proc::run({"cat", file}, {}, 2000);
        ok(r.out == "echo one\necho TWO\n", "...with exactly the bytes written");
    }

    const auto same = ro::stat_file(SessionId{1}, h, book, file);
    ok(same && same->mtime == saved->mtime, "stat after our own save: unchanged (no false alarm)");

    // someone else edits it (a size change makes the token differ within the same second)
    proc::run({"sh", "-c", "printf 'echo changed elsewhere\\n' >> " + file}, {}, 2000);
    const auto changed = ro::stat_file(SessionId{1}, h, book, file);
    ok(changed && changed->mtime != saved->mtime, "an outside edit is noticed");

    t = std::chrono::steady_clock::now();
    const auto ls = ro::list_project(h, book, dir);
    const double list_ms = ms_since(t);
    const auto* listed = &ls;
    ok(listed->files.size() == 1 && listed->files[0] == "notes.sh", "\u2318P lists the remote repo's files");
    std::printf("       project list: %.0f ms\n", list_ms);

    t = std::chrono::steady_clock::now();
    const auto fd = ro::search_project(3, h, book, dir, "two");
    const double search_ms = ms_since(t);
    const auto* found = &fd;
    ok(found->token == 3 && found->hits.size() == 1 && found->hits[0].line == 1 && found->hits[0].col == 5,
       "\u2318\u21e7F finds text on the remote host, case-folded, at its column");
    std::printf("       search: %.0f ms\n", search_ms);

    // a name the remote shell must not interpret
    const std::string odd = dir + "/it's $HOME; rm -rf x.txt";
    (void)ro::write_file(SessionId{1}, h, book, odd, "safe\n", 1);
    ok(std::filesystem::exists(odd), "a file name with quotes, $ and ; is written as a name, never run");

    const auto missing = ro::read_file(SessionId{1}, h, book, dir + "/missing.txt", false);
    ok(!missing.error.empty(), "a missing file says so");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

}  // namespace hand::cli::checks
