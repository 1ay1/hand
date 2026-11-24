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
#include <hand/ssh/sftp.hpp>

#include "../../app/fx/fx.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
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
    {   // what hand pays to start any child at all (every remote action is one)
        double best = 1e9;
        for (int i = 0; i < 5; ++i) {
            const auto s = std::chrono::steady_clock::now();
            (void)proc::run({"true"}, {}, 5000);
            best = std::min(best, ms_since(s));
        }
        std::printf("       spawn a child: %.1f ms\n", best);
    }
    {   // the kept-open channel: what every remote action rides
        const auto lat = [&](const char* what, auto&& f) {
            std::vector<double> xs;
            for (int i = 0; i < 15; ++i) { const auto s = std::chrono::steady_clock::now(); f(); xs.push_back(ms_since(s)); }
            std::sort(xs.begin(), xs.end());
            std::printf("       %s: p50 %.1f ms  max %.1f ms\n", what, xs[xs.size() / 2], xs.back());
            return xs[xs.size() / 2];
        };
        (void)remote::exec(h, book, "true");
        const double chan = lat("exec over the channel", [&] { (void)remote::exec(h, book, "true"); });
        const double fresh = lat("exec, a new ssh each time", [&] { auto a = remote::ssh_args(h, book); a.push_back("true"); (void)proc::run(a, {}, 10000); });
        const bool channels_on = std::getenv("HAND_SSH_CHANNEL") == nullptr || std::getenv("HAND_SSH_CHANNEL")[0] != '0';
        if (channels_on) ok(chan * 3.0 < fresh, "a request over the kept-open channel costs a fraction of a new ssh");
        // framing: whatever a command prints, the answer is exactly its bytes
        const auto x = remote::exec(h, book, "printf '\\nhand-err-fixed\\nno newline'; printf 'warn' >&2; exit 3");
        ok(x.code == 3 && x.out == "\nhand-err-fixed\nno newline" && x.err == "warn",
           "output that looks like the frame, no final newline, stderr and the exit code all come back exact");
        std::string bin;
        for (int i = 0; i < 300000; ++i) bin += static_cast<char>((i * 7919) & 0xFF);
        const auto echo = remote::exec(h, book, "cat", bin, 20000);
        ok(echo.code == 0 && echo.out == bin, "binary input (300 KB, every byte value) goes through and back untouched");
        const auto before = remote::exec(h, book, "echo ${X-unset}; pwd");
        (void)remote::exec(h, book, "X=leaked; cd /");
        ok(remote::exec(h, book, "echo ${X-unset}; pwd").out == before.out, "one request's variables and cwd never leak into the next");
        // a channel whose far end died (the laptop slept): the next request reconnects
        (void)remote::exec(h, book, "kill -9 $$");
        const auto after = remote::exec(h, book, "echo alive");
        ok(after.code == 0 && after.out == "alive\n", "a dead channel is replaced, and the request still lands");
        {   // the pane, the editor's watcher and a ⌘P list ask at once: each gets its own answer
            std::vector<std::string> got(8);
            {
                std::vector<std::jthread> ts;
                for (int i = 0; i < 8; ++i)
                    ts.emplace_back([&, i] { for (int k = 0; k < 10; ++k) got[static_cast<std::size_t>(i)] += remote::exec(h, book, "echo " + std::to_string(i)).out; });
            }
            bool right = true;
            for (int i = 0; i < 8; ++i) {
                std::string want;
                for (int k = 0; k < 10; ++k) want += std::to_string(i) + "\n";
                right = right && got[static_cast<std::size_t>(i)] == want;
            }
            ok(right, "eight threads asking at once each get only their own answers");
        }
    }

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

    {   // the file pane lists a folder over sftp, through the same connection
        t = std::chrono::steady_clock::now();
        const auto pl = sftp::list(h, book, dir);
        const double pane_ms = ms_since(t);
        bool has = false;
        for (const auto& e : pl.entries) has = has || e.name == "notes.sh";
        ok(pl.error.empty() && has, "the file pane lists a remote folder");
        std::printf("       pane folder: %.0f ms\n", pane_ms);
    }

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
