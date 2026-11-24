// hand::cli::checks — a file from the pane opens in a tab, in the editor.
//
// Through the real input layer: the pane's open action makes an editor tab
// beside the shell; keys type into the file (and never reach a pty); ⌘ /
// ctrl chords are the editor's (find, save) while the window keeps its own
// (switching tabs); closing with unsaved edits asks, and "save" closes it
// once the write lands. (The self-test runs as the demo: the read is fed as
// the FileLoaded a real read sends, and saves land without touching disk.)

#include "suites.hpp"

#include <hand/app.hpp>

#include "../../app/detail.hpp"   // quick_open_ranked

#include <algorithm>
#include <cstdio>
#include <string>

namespace hand::cli::checks {

using namespace mayag;
using namespace mayag::dsl;

namespace {
// The editor's chords, in the keymap this pass presses them in: ⌘ on the
// Mac's, ctrl on the PC's. The suite runs both on every machine -- one
// implementation, checked for each convention, wherever it builds.
Keymap g_km = native_keymap();
Mods primary(bool shift = false) { return Mods::cmd(shift, g_km); }
Mods with(Mods m) { m.keymap = g_km; return m; }

void editor_in(Runtime& rt, Check& ok);
}  // namespace

void editor(Runtime& rt, Vec2, Check& ok) {
    for (const Keymap km : {Keymap::mac, Keymap::pc}) {
        g_km = km;
        std::printf("    (the %s's keys)\n", km == Keymap::mac ? "Mac" : "PC");
        editor_in(rt, ok);
    }
    g_km = native_keymap();
}

namespace {
void editor_in(Runtime& rt, Check& ok) {
    const std::size_t tabs0 = rt.model().tab_count();
    const std::size_t shell_tab = rt.model().active();
    const std::string path = "/tmp/hand-selftest/notes.cpp";

    // the pane's open action: a new tab beside the shell, in front
    rt.send(App::Msg{App::PaneOp{FilePaneOp{[path](FilePaneState& s) { s.out.push_back({FilePaneAction::Kind::open, path}); }}}});
    ok(rt.model().tab_count() == tabs0 + 1 && rt.model().tab().file && rt.model().tab().file->path == path,
       "opening a file from the pane makes an editor tab in front");
    ok(rt.model().active() == shell_tab + 1, "...right beside the tab it came from");
    ok(!rt.model().tab().pty && !rt.model().tab().launch, "an editor tab starts no shell");
    const SessionId sid = rt.model().tab().sid;
    rt.send(App::Msg{App::FileLoaded{sid, path, "int main() {\n    return 0;\n}\n", "", false}});
    for (int i = 0; i < 3; ++i) rt.tick();
    const auto& d = [&]() -> const FileDoc& { return *rt.model().tab().file; };
    ok(!d().loading && d().st.ed.line_count() == 4 && d().st.ed.language->name == "C++", "the text arrives, in C++");

    // the same file again: its tab, not a second one
    rt.send(App::Msg{App::OpenFile{path}});
    ok(rt.model().tab_count() == tabs0 + 1, "opening it again brings its tab, not another");

    // typing goes into the file
    rt.press_key(Key::down);
    rt.press_key(Key::end);
    rt.window().type(" // done");
    rt.tick();
    ok(d().st.ed.line(1) == "    return 0; // done", "typed text lands in the file at the caret");
    ok(d().st.ed.dirty(), "...and the file is unsaved");
    ok(rt.model().tab().input.empty(), "nothing is sent to any pty");

    // the editor's chords: find
    rt.press_key(Key::f, primary());
    ok(d().st.find_open, "the find key opens find in the file");
    rt.window().type("return");
    rt.tick();
    ok(d().st.matches.size() == 1, "...and finds what is typed");
    rt.press_key(Key::escape);
    ok(!d().st.find_open, "escape closes find");

    // completion through the real input: a prefix of a word the file uses
    rt.press_key(Key::end);
    rt.press_key(Key::enter);
    rt.window().type("ret");
    rt.tick();
    ok(d().st.comp_open && !d().st.comp.empty() && d().st.comp[0].word == "return", "typing offers the file's words, best first");
    rt.press_key(Key::tab);
    ok(!d().st.comp_open && d().st.ed.line(d().st.ed.cursor().line).find("return") != std::string::npos, "tab takes the completion");
    rt.press_key(Key::z, primary());   // undo it, and the newline
    rt.press_key(Key::z, primary());

    // someone else changed the file: edited here, so the editor asks
    rt.send(App::Msg{App::FileStat{sid, 777}});
    ok(d().st.disk_changed, "an outside change to an edited file is announced, the edits kept");

    // the window keeps its chords: ctrl+tab goes back to the shell and returns
    rt.press_key(Key::tab, with(Mods{.shift = true, .ctrl = true}));
    ok(!rt.model().tab().file, "ctrl+shift+tab still switches tabs from an editor");
    rt.press_key(Key::tab, with(Mods{.ctrl = true}));
    ok(rt.model().tab().file.has_value(), "...and back");

    // close with unsaved edits: asked, even though close.confirm may be never
    rt.press_key(Key::w, with(Mods{.shift = true, .ctrl = true}));
    ok(rt.model().dialog.ask && rt.model().dialog.ask->kind == Ask::Kind::close_file, "closing unsaved edits asks first");
    rt.press_key(Key::escape);
    ok(rt.model().tab_count() == tabs0 + 1 && d().st.ed.dirty(), "cancel keeps the tab and the edits");

    // save, then close: no question
    rt.press_key(Key::s, primary());
    for (int i = 0; i < 3; ++i) rt.tick();
    ok(!d().st.ed.dirty(), "the save key saves");
    rt.press_key(Key::w, with(Mods{.shift = true, .ctrl = true}));
    for (int i = 0; i < 3; ++i) rt.tick();
    if (rt.model().dialog.ask) rt.press_key(Key::escape);   // a close.confirm = always run
    for (int i = 0; i < 3; ++i) rt.tick();
    ok(rt.model().tab_count() == tabs0 || rt.model().dialog.ask.has_value(), "a saved file's tab closes");

    // "save" from the dialog: the tab closes once the write lands
    if (rt.model().tab_count() == tabs0) {
        rt.send(App::Msg{App::OpenFile{path}});
        rt.send(App::Msg{App::FileLoaded{rt.model().tab().sid, path, "x\n", "", false}});
        rt.window().type("y");
        rt.tick();
        rt.press_key(Key::w, with(Mods{.shift = true, .ctrl = true}));
        rt.press_key(Key::enter);   // the focused action: save
        for (int i = 0; i < 4; ++i) rt.tick();
        ok(rt.model().tab_count() == tabs0, "\"save\" in the dialog saves, then closes the tab");
    }

    {   // a remote file read once reopens AT ONCE (its text as it was), and one
        // stat confirms it -- a round trip, not a whole read before you see it
        Host box;
        box.alias = "box";
        box.hostname = "box.example";
        const std::string rp = "/srv/app/main.py";
        const auto close_front = [&] {
            rt.send(App::Msg{App::CloseTab{TabIndex{rt.model().active()}, Confirm::given}});
            for (int i = 0; i < 3; ++i) rt.tick();
        };
        rt.send(App::Msg{App::OpenFile{.path = rp, .host = box}});
        ok(rt.model().tab().file && rt.model().tab().file->loading, "a remote file opens loading (nothing read yet)");
        rt.send(App::Msg{App::FileLoaded{.sid = rt.model().tab().sid, .path = rp, .text = "print('hi')\n", .mtime = 42}});
        close_front();
        rt.send(App::Msg{App::OpenFile{.path = rp, .host = box}});
        const FileDoc* d = rt.model().tab().file ? &*rt.model().tab().file : nullptr;
        ok(d && !d->loading && d->st.ed.text() == "print('hi')\n" && d->disk_mtime == 42,
           "reopened, it shows at once with its change token (no read to wait for)");
        rt.window().type("x");
        rt.tick();
        close_front();
        rt.send(App::Msg{App::OpenFile{.path = rp, .host = box}});
        ok(rt.model().tab().file && rt.model().tab().file->loading, "edited and discarded: forgotten, read afresh (never a version the host lacks)");
        close_front();
    }
    {   // the file pane on a remote host: a folder visited before shows at once on return
        using A = FilePaneAction;
        const bool was_open = rt.model().pane.open;
        rt.send(App::Msg{mayag::Edit<App::Model>{[](App::Model& mm) {
            Host box;
            box.alias = "box";
            box.hostname = "box.example";
            mm.pane.open = true;
            mm.pane.where = "box";
            mm.pane.host = box;
            mm.pane.follow = false;
        }}});
        const auto go = [&](const std::string& p) {
            rt.send(App::Msg{App::PaneOp{FilePaneOp{[p](FilePaneState& s) { s.out.push_back({A::Kind::list, p}); }}}});
        };
        const auto listed = [&](const std::string& p, std::vector<FileEntry> es, App::Notice n = App::Notice::loud) {
            rt.send(App::Msg{App::PaneListed{rt.model().pane.token, p, std::move(es), "", n}});
        };
        const std::string top = "/srv/" + std::to_string(rt.model().pane.token);   // fresh per run of this suite
        go(top);
        ok(rt.model().pane.loading, "a remote folder never seen: listed (loading)");
        listed(top, {{.name = "app", .dir = true}, {.name = "notes.txt"}});
        go(top + "/app");
        listed(top + "/app", {{.name = "main.py"}});
        go(top);
        ok(!rt.model().pane.loading && rt.model().pane.entries.size() == 2 && rt.model().pane.path == top,
           "...seen before: shown at once, the re-list quiet behind it");
        listed(top, {{.name = "app", .dir = true}, {.name = "notes.txt"}, {.name = "new.log"}}, App::Notice::quiet);
        ok(rt.model().pane.entries.size() == 3 && rt.model().pane.fresh_at.count("new.log") == 1,
           "...and what changed since arrives a round trip later, glowing");
        rt.send(App::Msg{mayag::Edit<App::Model>{[was_open](App::Model& mm) {
            mm.pane.where.clear(); mm.pane.host.reset(); mm.pane.open = was_open; mm.pane.entries.clear(); mm.pane.path.clear();
        }}});
    }

    // ⌘P: the project's files, fuzzy-found. On the Mac from any tab; on the
    // PC ctrl+P in a shell is the shell's (history), so from an editor tab.
    const bool mac = g_km == Keymap::mac;
    if (!mac) {
        rt.press_key(Key::p, primary());
        ok(!rt.model().quick.open, "on the PC, ctrl+P in a shell stays the shell's");
        rt.send(App::Msg{App::OpenFile{"/tmp/hand-selftest/other.cpp"}});
        rt.send(App::Msg{App::FileLoaded{rt.model().tab().sid, "/tmp/hand-selftest/other.cpp", "x\n", "", false}});
    }
    rt.press_key(Key::p, primary());
    ok(rt.model().quick.open, "the quick-open key opens the file finder");
    rt.send(App::Msg{App::QuickListed{"/tmp/hand-selftest", {"src/app/editor.cpp", "src/app/pane.cpp", "README.md"}, false}});
    rt.window().type("pane");
    rt.tick();
    ok(!quick_open_ranked(rt.model()).empty() &&
           (*rt.model().quick.files)[static_cast<std::size_t>(quick_open_ranked(rt.model())[0].first)] == "src/app/pane.cpp",
       "...and finds the file by name");
    rt.press_key(Key::escape);
    ok(!rt.model().quick.open, "escape closes it");

    // ⌘⇧F: text across the project, landing on the line
    rt.press_key(Key::f, primary(true));
    ok(rt.model().search_all.open, "the project-search key opens the search");
    rt.window().type("needle");
    rt.tick();
    const std::uint64_t tok = rt.model().search_all.token;
    rt.send(App::Msg{App::SearchAllDone{tok - 1, "/tmp/hand-selftest", {{"stale.cpp", 0, 0, 6, "needle"}}, false, ""}});
    ok(std::none_of(rt.model().search_all.hits.begin(), rt.model().search_all.hits.end(),
                    [](const auto& h) { return h.file == "stale.cpp"; }),
       "an older query's answer is dropped");
    rt.send(App::Msg{App::SearchAllDone{tok, "/tmp/hand-selftest", {{"notes.cpp", 2, 4, 6, "    needle();"}}, false, ""}});
    ok(rt.model().search_all.hits.size() == 1, "the current query's answer is shown");
    rt.press_key(Key::enter);
    ok(!rt.model().search_all.open && rt.model().tab().file && rt.model().tab().file->path == "/tmp/hand-selftest/notes.cpp",
       "enter opens the hit's file");
    rt.send(App::Msg{App::FileLoaded{rt.model().tab().sid, "/tmp/hand-selftest/notes.cpp", "a\nb\n    needle();\n", "", false}});
    {
        const auto& ed = rt.model().tab().file->st.ed;
        ok(ed.primary_sel().start() == mayag::editor::Pos{2, 4} && ed.primary_sel().end() == mayag::editor::Pos{2, 10},
           "...with the match selected on its line");
    }
    rt.send(App::Msg{App::CloseTab{TabIndex{rt.model().active()}, Confirm::given}});
    if (!mac && rt.model().tab_count() > tabs0)   // other.cpp, opened for the PC's keys
        for (std::size_t i = 0; i < rt.model().tab_count(); ++i)
            if (rt.model().at(i).file) { rt.send(App::Msg{App::CloseTab{TabIndex{i}, Confirm::given}}); break; }

    // a binary is not a tab: it goes to its app and the waiting tab closes
    rt.send(App::Msg{App::OpenFile{"/tmp/hand-selftest/logo.png"}});
    rt.send(App::Msg{App::FileLoaded{rt.model().tab().sid, "/tmp/hand-selftest/logo.png", "", "", true}});
    for (int i = 0; i < 3; ++i) rt.tick();
    ok(rt.model().tab_count() == tabs0 && !rt.model().tab().file, "a binary goes to its own app, no tab left behind");
    if (rt.model().active() != shell_tab) rt.send(App::Msg{App::SelectTab{TabIndex{shell_tab}}});
}
}  // namespace

}  // namespace hand::cli::checks
