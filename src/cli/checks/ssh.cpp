// hand::cli::checks — ssh: config, hosts.vibe, the picker, the manager, reconnect.

#include "suites.hpp"

#include "../../app/detail.hpp"
#include <hand/ssh/sftp.hpp>
#include <hand/core/platform.hpp>

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

namespace hand::cli::checks {

using namespace mayag;

void ssh([[maybe_unused]] Runtime& rt, [[maybe_unused]] Vec2 size, Check& ok) {
    // ── ssh: ~/.ssh/config the way ssh reads it, hosts.vibe, the picker ──
    using K = mayag::Key;
    const std::string dir = (std::filesystem::temp_directory_path() / ("hand-ssh-" + std::to_string(hand::self_pid()))).string();
    std::filesystem::create_directories(dir + "/conf.d");
    const auto put = [](const std::string& p, const std::string& s) { std::FILE* f = std::fopen(p.c_str(), "w"); std::fputs(s.c_str(), f); std::fclose(f); };
    put(dir + "/config",
        "# test\nHost *.internal\n  User ops\nHost prod-db\n  HostName 10.0.3.4\n  User deploy\n  ProxyJump bastion\n"
        "Host bastion gpu-box !nope\n  HostName %h.example.com\nHost gpu-box\n  Port 2222\n  User root\n"
        "Include " + std::string{dir} + "/conf.d/*\nHost *\n  User fallback\n  IdentityFile ~/.ssh/id_ed25519\n");
    put(dir + "/conf.d/extra", "Host api.staging.internal\n");
    put(dir + "/known", "seen.example.org ssh-ed25519 AAAA\n[odd.example.org]:2200 ssh-rsa AAAA\n|1|hashed= ssh-rsa AAAA\n10.0.3.4 ssh-ed25519 AAAA\n");
    const hand::HostSources src{.ssh_config = dir + "/config", .known_hosts = dir + "/known", .store = dir + "/hosts.vibe"};
    auto book = hand::hosts_load(src);
    const auto* db = book.find("prod-db");
    const auto* gpu = book.find("gpu-box");
    const auto* api = book.find("api.staging.internal");
    ok(db && db->hostname == "10.0.3.4" && db->user == "deploy" && db->jump == "bastion" && db->env == hand::HostEnv::prod,
       "ssh_config: HostName / User / ProxyJump, and prod-db reads as prod");
    // first value wins: gpu-box's Port comes from its own block, its User from the first block that sets one
    ok(gpu && gpu->hostname == "gpu-box.example.com" && gpu->port == 2222 && gpu->user == "root" && !book.find("nope") && !book.find("*.internal"),
       "ssh_config: %h, first-wins across blocks, globs and !negations are not hosts");
    ok(api && api->user == "ops" && api->env == hand::HostEnv::staging, "ssh_config: Include, a glob block applies, staging from the name");
    ok(book.find("seen.example.org") && book.find("odd.example.org:2200") && book.find("odd.example.org:2200")->port == 2200 &&
       !book.find("10.0.3.4"), "known_hosts: new machines listed, [host]:port kept, hashed and known ones skipped");
    const auto d = hand::parse_destination("deploy@10.0.0.9:2201");
    const auto s = hand::ssh_destination({"ssh", "-t", "-p", "2022", "-l", "me", "-o", "Foo=bar", "box.lan", "uptime"});
    ok(d && d->user == "deploy" && d->hostname == "10.0.0.9" && d->port == 2201 && !hand::parse_destination("a b") &&
       s && s->hostname == "box.lan" && s->user == "me" && s->port == 2022, "destinations: typed user@host:port, and a running ssh's argv");
    const auto argv = hand::ssh_argv(*gpu, book, {});
    const bool muxed = std::find(argv.begin(), argv.end(), "ControlMaster=auto") != argv.end();
#if defined(_WIN32)
    ok(argv.back() == "gpu-box" && !muxed,
       "a config host runs as `ssh <alias>`; never multiplexed on Windows (its OpenSSH has no ControlMaster)");
#else
    ok(argv.back() == "gpu-box" && muxed, "a config host runs as `ssh <alias>`, multiplexed");
#endif
    hand::hosts_record_use(src, *gpu, 1000);
    hand::hosts_record_use(src, *gpu, 2000);
    hand::hosts_set_pinned(src, *db, true);
    hand::hosts_record_use(src, *d, 1500);
    book = hand::hosts_load(src);
    ok(book.find("gpu-box")->uses == 2 && book.find("gpu-box")->last == 2000 && book.find("prod-db")->pinned &&
       book.find("deploy@10.0.0.9:2201") && book.find("deploy@10.0.0.9:2201")->port == 2201,
       "hosts.vibe: uses, last, pinned, and a typed host remembered");
    std::vector<dsl::SshHost> sh;
    for (const auto& h : book.hosts) { dsl::SshHost x; x.alias = h.alias; x.hostname = h.hostname; x.user = h.user; x.pinned = h.pinned; x.uses = h.uses; x.last_used = h.last; sh.push_back(x); }
    const auto r0 = dsl::rank_hosts(sh, "", 2100), r1 = dsl::rank_hosts(sh, "gpu", 2100);
    ok(!r0.empty() && sh[static_cast<std::size_t>(r0[0])].alias == "prod-db" && sh[static_cast<std::size_t>(r0[1])].alias == "gpu-box" &&
       !r1.empty() && sh[static_cast<std::size_t>(r1[0])].alias == "gpu-box",
       "ranking: pinned, then frecent, before typing; a query finds its host");
    std::filesystem::remove_all(dir);

    // the picker, end to end (the demo has no pty: a connect makes a tab)
    rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[s = src](hand::App::Model& mm) {
        mm.ssh.book = hand::HostBook{};
        for (const char* a : {"prod-db", "gpu-box", "build-01"}) { hand::Host h; h.alias = a; h.hostname = a; mm.ssh.book.hosts.push_back(h); }
        mm.ssh.book.hosts[0].env = hand::HostEnv::prod;
    }}});
    rt.press_key(K::s, Mods{.shift = true, .ctrl = true});
    rt.tick();
    // OpenHosts reloads the book in a real run; in the demo it keeps the fixture
    ok(rt.model().ssh.picker.open && rt.node_rect(dsl::part_id("hosts", 0)).has_value() && rt.node_rect(dsl::part_id("hosts", 2)).has_value(),
       "ctrl+shift+s opens the host picker, every host listed");
    rt.type("gpu");
    rt.tick();
    ok(rt.node_rect(dsl::part_id("hosts", 0)).has_value() && !rt.node_rect(dsl::part_id("hosts", 1)).has_value(), "typing narrows it to one host");
    const std::size_t tabs0 = rt.model().tab_count();
    rt.press_key(K::enter);
    for (int i = 0; i < 10; ++i) rt.tick();
    ok(!rt.model().ssh.picker.open && rt.model().tab_count() == tabs0 + 1 && rt.model().tab().remote && rt.model().tab().remote->alias == "gpu-box",
       "enter connects in a new tab, beside this one");
    // ssh fails (255): the tab stays, with its scrollback, and offers to reconnect
    rt.send(hand::App::Msg{hand::App::Exited{255, rt.model().tab().sid}});
    rt.tick();
    ok(rt.model().tab_count() == tabs0 + 1 && rt.model().tab().link.dropped && rt.node_rect(dsl::node_id("ssh.reconnect")).has_value(),
       "a dropped connection keeps the tab and offers to reconnect");
    rt.press_key(K::enter);
    rt.tick();
    ok(!rt.model().tab().link.dropped, "enter reconnects");
    {   // ...and by itself: a scheduled try that fails backs off, one that holds resets
        const auto drop = [&](double up_for) {
            rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[up_for](hand::App::Model& m) { m.tab().link.connected_at = m.time - up_for; }}});
            rt.send(hand::App::Msg{hand::App::Exited{255, rt.model().tab().sid}});
        };
        drop(60.0);   // was up a minute, then dropped
        const double first = rt.model().tab().link.retry_at - rt.model().time;
        ok(rt.model().tab().link.dropped && first > 0.5 && first < 1.5, "a drop schedules a reconnect in about a second");
        rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) { m.time = m.tab().link.retry_at + 0.01; }}});
        rt.send(hand::App::Msg{hand::App::RetryTick{}});
        ok(!rt.model().tab().link.dropped && rt.model().tab().launch.has_value() == false,
           "when it is due, it reconnects by itself (a live terminal: a password prompt works)");
        drop(1.0);    // that try died at once
        const double second = rt.model().tab().link.retry_at - rt.model().time;
        ok(rt.model().tab().link.attempts == 1 && second > 1.5 && second < 2.5, "a try that fails backs off (2 s, then 4, 8 ...)");
        rt.press_key(K::enter);
        rt.tick();
        ok(!rt.model().tab().link.dropped && rt.model().tab().link.attempts == 0, "enter tries now and starts the schedule over");
    }
    rt.send(hand::App::Msg{hand::App::Exited{0, rt.model().tab().sid}});
    for (int i = 0; i < 10; ++i) rt.tick();
    ok(rt.model().tab_count() == tabs0, "a clean logout closes the tab");
    // a typed destination nobody listed
    rt.press_key(K::s, Mods{.shift = true, .ctrl = true});
    rt.type("me@10.1.2.3:2200");
    rt.tick();
    ok(rt.node_rect(dsl::part_id("hosts", dsl::part_inc)).has_value(), "an unknown user@host:port offers to connect as typed");
    rt.press_key(K::escape);
    rt.tick();
    {
        // ── the host manager ─────────────────────────────────────────────
        rt.press_key(K::h, Mods{.shift = true, .ctrl = true});
        rt.tick();
        ok(rt.model().ssh.manager.open && rt.node_rect(dsl::node_id("mgr.connect")).has_value(),
           "ctrl+shift+h opens the host manager on a host, ready to connect");
        const std::size_t n0 = rt.model().ssh.book.hosts.size();
        // a new host: n, fields by typing and tab, a bad tunnel refused
        rt.press_key(K::n);
        rt.tick();
        ok(rt.model().ssh.manager.ui.editing && rt.model().ssh.manager.ui.edit_host < 0, "n starts a new host");
        rt.type("metrics"); rt.press_key(K::tab);
        rt.type("10.0.9.7"); rt.press_key(K::tab);
        rt.type("deploy"); rt.press_key(K::tab);
        rt.type("2222");
        for (int i = 0; i < 6; ++i) rt.press_key(K::tab);   // jump, key, folder, env, tags -> tunnels
        rt.type("L99999");
        rt.press_key(K::enter);
        rt.tick();
        ok(rt.model().ssh.manager.ui.editing && !rt.model().ssh.manager.ui.error.empty() && rt.model().ssh.book.hosts.size() == n0,
           "a bad tunnel (port 99999) is refused, with why");
        for (int i = 0; i < 6; ++i) rt.press_key(K::backspace);
        rt.type("L3000:grafana:3000");
        rt.press_key(K::enter);
        rt.tick();
        const auto& hb = rt.model().ssh.book.hosts;
        const auto* made = hb.size() == n0 + 1 ? &hb.back() : nullptr;
        ok(!rt.model().ssh.manager.ui.editing && made && made->alias == "metrics" && made->hostname == "10.0.9.7" && made->user == "deploy" &&
           made->port == 2222 && made->forwards.size() == 1 && made->forwards[0] == "L3000:grafana:3000" &&
           made->source == hand::HostSource::saved,
           "enter saves the new host, every field and its tunnel");
        if (made) {
            const auto args = hand::ssh_argv(*made, rt.model().ssh.book, {.multiplex = false, .keepalive = false});
            const std::string joined = [&] { std::string j; for (const auto& a : args) j += a + " "; return j; }();
            ok(joined.find("-L 3000:grafana:3000") != std::string::npos && joined.find("-p 2222") != std::string::npos &&
               joined.find("deploy@10.0.9.7") != std::string::npos,
               "connecting runs ssh -L 3000:grafana:3000 -p 2222 deploy@10.0.9.7");
        }
        // pin it, then delete it (asks once)
        rt.press_key(K::p);
        rt.tick();
        ok(!rt.model().ssh.book.hosts.empty() && rt.model().ssh.book.hosts.back().pinned, "p pins the selected host");
        rt.press_key(K::del);
        rt.tick();
        ok(rt.model().ssh.manager.ui.confirm_delete && rt.model().ssh.book.hosts.size() == n0 + 1, "delete asks first");
        rt.press_key(K::del);
        rt.tick();
        ok(rt.model().ssh.book.hosts.size() == n0, "and deletes on the second press");
        // filter, then enter on a host connects in a new tab
        rt.type("/gpu");
        rt.press_key(K::enter);
        const std::size_t t0 = rt.model().tab_count();
        rt.press_key(K::enter);
        for (int i = 0; i < 10; ++i) rt.tick();
        ok(!rt.model().ssh.manager.open && rt.model().tab_count() == t0 + 1 && rt.model().tab().remote && rt.model().tab().remote->alias == "gpu-box",
           "filter + enter connects to the host in a new tab");
        rt.send(hand::App::Msg{hand::App::Exited{0, rt.model().tab().sid}});
        for (int i = 0; i < 10; ++i) rt.tick();
        // ── folders, import, files, tunnels ────────────────────────────────────
        rt.press_key(K::h, Mods{.shift = true, .ctrl = true});
        rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& mm) {
            hand::Host h; h.alias = "ci-1"; h.hostname = "ci-1.internal"; h.source = hand::HostSource::saved; h.folder = "Work/CI";
            h.forwards = {"L5432:localhost:5432", "L8080:localhost:80"};
            mm.ssh.book.hosts.push_back(h);
            hand::ImportSource src{"PuTTY", "test", {}};
            hand::Host a; a.alias = "router"; a.hostname = "192.168.1.1"; a.source = hand::HostSource::saved; a.folder = "PuTTY";
            src.hosts = {a, h};   // ci-1 exists already: skipped
            mm.ssh.manager.imports = {src};
            mm.ssh.manager.ui.sel = static_cast<int>(mm.ssh.book.hosts.size()) - 1;
        }}});
        rt.tick();
        // a folder's defaults, through the folder editor
        rt.send(hand::App::Msg{hand::App::Manager{mayag::dsl::SshManagerOp{[](mayag::dsl::SshManagerState& s) {
            mayag::dsl::ssh_manager_ops::begin_folder_edit(s, {}, "Work");
        }}}});
        rt.type("x");   // name field focus is on user for an existing folder
        rt.press_key(K::backspace);
        rt.type("ops"); rt.press_key(K::tab); rt.type("2222"); rt.press_key(K::tab); rt.type("bastion");
        rt.press_key(K::enter);
        rt.tick();
        const hand::Folder* wf = rt.model().ssh.book.folder("Work");
        ok(wf && wf->user == "ops" && wf->port == 2222 && wf->jump == "bastion" && !rt.model().ssh.manager.ui.editing,
           "the folder editor saves what its hosts share");
        const auto& ci = rt.model().ssh.book.hosts.back();
        std::string av; for (const auto& x : hand::ssh_argv(ci, rt.model().ssh.book, {.multiplex = false, .keepalive = false})) av += x + " ";
        ok(av.find("-p 2222") != std::string::npos && av.find("-J bastion") != std::string::npos && av.find("ops@ci-1.internal") != std::string::npos,
           "a host in Work/CI connects with Work's user, port and jump");
        // tunnel status from a port scan
        rt.send(hand::App::Msg{hand::App::TunnelScan{{5432, 8080}, {"ssh", "postgres"}, true}});
        rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& mm) { mm.tab().remote = mm.ssh.book.hosts.back(); }}});
        rt.tick();
        {
            const auto hs = hand::detail::picker_hosts(rt.model());
            const auto& last = hs.back();
            using T = mayag::dsl::SshHost::Tunnel;
            ok(last.tunnel.size() == 2 && last.tunnel[0] == T::up && last.tunnel[1] == T::conflict &&
                   last.tunnel_note[1].find("postgres") != std::string::npos,
               "tunnels: ssh's port is up, a port taken by another program says by whom");
        }
        rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& mm) { mm.tab().remote.reset(); }}});
        // import: new sessions only, into their folder
        const std::size_t nb = rt.model().ssh.book.hosts.size();
        rt.press_key(K::i);
        rt.tick();
        ok(rt.model().ssh.manager.ui.import_open && rt.node_rect(dsl::part_id("mgr.imp", 0)).has_value(), "i opens the import drawer, one row per client");
        rt.click(dsl::part_id("mgr.imp", 0));
        rt.tick();
        ok(rt.model().ssh.book.hosts.size() == nb + 1 && rt.model().ssh.book.hosts.back().alias == "router" && rt.model().ssh.book.hosts.back().folder == "PuTTY",
           "import adds only the sessions not here yet, in a folder named for the client");
        // the Files tab: f lists, a listing arrives, enter goes into a directory
        rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& mm) { mm.ssh.manager.ui.sel = static_cast<int>(mm.ssh.book.hosts.size()) - 2; }}});
        rt.press_key(K::f);
        rt.tick();
        ok(rt.model().ssh.manager.ui.tab == 1 && rt.model().ssh.manager.files.alias == "ci-1" && rt.model().ssh.manager.files.loading, "f opens the host's files and starts listing");
        rt.send(hand::App::Msg{hand::App::FilesListed{"ci-1", "/home/ci", {{"logs", "Nov 20 09:12", "", true, false, 0}, {"a.txt", "Nov 1 10:00", "", false, false, 12}}, ""}});
        rt.tick();
        ok(!rt.model().ssh.manager.files.loading && rt.model().ssh.manager.files.entries.size() == 2 && rt.node_rect(dsl::part_id("mgr.file", 1)).has_value(),
           "the listing shows, directories first");
        rt.press_key(K::enter);
        rt.tick();
        ok(rt.model().ssh.manager.files.loading, "enter on a directory lists it");
        rt.press_key(K::escape);
        rt.press_key(K::escape);
        rt.tick();
        ok(!rt.model().ssh.manager.open, "esc leaves the files, then the manager");
        {
            const auto l = hand::sftp::parse_listing(
                "Remote working directory: /srv\n"
                "-rw-r--r--    1 a a     220145 Nov 18 22:01 my report.pdf\n"
                "drwxr-xr-x    2 a a       4096 Nov 17 09:12 logs\n"
                "lrwxrwxrwx    1 a a         11 Nov 10 10:00 current -> releases/42\n");
            ok(l.path == "/srv" && l.entries.size() == 3 && l.entries[0].name == "logs" && l.entries[1].target == "releases/42" &&
               l.entries[2].name == "my report.pdf" && l.entries[2].size == 220145,
               "sftp listings: dirs first, names with spaces, links with targets");
            ok(hand::sftp::explain("x: Permission denied (publickey,password).", 255).find("key login") != std::string::npos,
               "sftp: a password-only host is explained, not a raw error");
            const auto mh = hand::parse_mobaxterm_ini(
                "[Bookmarks_1]\nSubRep=Work\\EU\nImgNum=41\ndb=#109#0%10.1.2.3%2200%root%%-1%-1#MobaFont#0# #-1\n"
                "rdp=#91#4%r.example%3389%admin%0#MobaFont#0# #-1\n");
            ok(mh.size() == 1 && mh[0].alias == "db" && mh[0].port == 2200 && mh[0].user == "root" && mh[0].folder == "MobaXterm/Work/EU",
               "MobaXterm.ini: SSH bookmarks with their folder; RDP skipped");
        }
        {   // the file pane's list: hidden, sort, dirs first, fuzzy filter, crumbs, ages
            namespace fp = mayag::dsl;
            std::vector<fp::FileEntry> es = {{.name = "src", .dir = true, .mtime = 100},
                                             {.name = ".git", .dir = true},
                                             {.name = "CMakeLists.txt", .size = 3000, .mtime = 300},
                                             {.name = "README.md", .size = 9000, .mtime = 200},
                                             {.name = "build.sh", .size = 10, .mtime = 400}};
            fp::FilePaneState st;
            auto v = fp::file_pane_view(es, st);
            ok(v.size() == 4 && es[v[0].i].name == "src" && es[v[1].i].name == "build.sh",
               "file pane: dotfiles hidden, folders first, names case-insensitive");
            st.sort = fp::FileSort::size;
            v = fp::file_pane_view(es, st);
            const bool by_size = es[v[1].i].name == "README.md";
            st.sort = fp::FileSort::modified;
            v = fp::file_pane_view(es, st);
            ok(by_size && es[v[1].i].name == "build.sh" && es[v[3].i].name == "README.md", "file pane: sorts by size and by age");
            st.filter = "cmk";
            v = fp::file_pane_view(es, st);
            ok(v.size() == 1 && es[v[0].i].name == "CMakeLists.txt" && v[0].hits.size() == 3, "file pane: fuzzy filter lights its matches");
            st.filter = ".gi";
            ok(fp::file_pane_view(es, st).size() == 1, "file pane: a filter starting with . finds dotfiles");
            const auto cr = fp::file_pane_detail::crumbs("C:/Users/me/src/hand", "C:/Users/me", '/');
            ok(cr.size() == 3 && cr[0].first == "~" && cr[2].second == "C:/Users/me/src/hand", "file pane: crumbs fold home into ~");
            ok(fp::file_pane_detail::age(10000, 10000 - 7200) == "2h" && fp::file_pane_detail::age(10000, 0).empty(),
               "file pane: ages read 2h, unknown is blank");
            fp::FilePaneState fs2;
            auto k1 = fp::file_pane_key(fs2, fp::FilePaneProps{.entries = es}, mayag::Key::slash, {});
            if (k1) (*k1)(fs2);
            if (auto t1 = fp::file_pane_text(fs2, "/")) (*t1)(fs2);   // the / that began it
            if (auto t2 = fp::file_pane_text(fs2, "rea")) (*t2)(fs2);
            ok(fs2.filtering && fs2.filter == "rea", "file pane: / starts a filter, typing narrows it");
            struct Estimate {};   // no font: 0.6 em a byte
            const std::string mid = mayag::dsl::detail::fit_middle(Estimate{}, "compile_commands.json", 10.0f * 0.6f * 14.0f, 10.0f);
            ok(mid.size() < std::string{"compile_commands.json"}.size() && mid.find("\u2026") != std::string::npos &&
                   mid.rfind(".json") == mid.size() - 5 && mid.rfind("comp", 0) == 0,
               ("file pane: a long name is cut in the middle, its start and extension kept (" + mid + ")").c_str());
            ok(mayag::dsl::detail::fit_middle(Estimate{}, "a.txt", 200.0f, 10.0f) == "a.txt", "file pane: a name that fits is left alone");
        }
    }
    ok(!rt.model().ssh.picker.open, "esc closes the picker");
}

}  // namespace hand::cli::checks
