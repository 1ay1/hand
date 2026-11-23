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

#include <string>

namespace hand::cli::checks {

using namespace mayag;
using namespace mayag::dsl;

namespace {
Mods primary() {
#if defined(__APPLE__)
    return Mods{.super = true};
#else
    return Mods{.ctrl = true};
#endif
}
}  // namespace

void editor(Runtime& rt, Vec2, Check& ok) {
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
    rt.press_key(Key::tab, Mods{.shift = true, .ctrl = true});
    ok(!rt.model().tab().file, "ctrl+shift+tab still switches tabs from an editor");
    rt.press_key(Key::tab, Mods{.ctrl = true});
    ok(rt.model().tab().file.has_value(), "...and back");

    // close with unsaved edits: asked, even though close.confirm may be never
    rt.press_key(Key::w, Mods{.shift = true, .ctrl = true});
    ok(rt.model().dialog.ask && rt.model().dialog.ask->kind == Ask::Kind::close_file, "closing unsaved edits asks first");
    rt.press_key(Key::escape);
    ok(rt.model().tab_count() == tabs0 + 1 && d().st.ed.dirty(), "cancel keeps the tab and the edits");

    // save, then close: no question
    rt.press_key(Key::s, primary());
    for (int i = 0; i < 3; ++i) rt.tick();
    ok(!d().st.ed.dirty(), "the save key saves");
    rt.press_key(Key::w, Mods{.shift = true, .ctrl = true});
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
        rt.press_key(Key::w, Mods{.shift = true, .ctrl = true});
        rt.press_key(Key::enter);   // the focused action: save
        for (int i = 0; i < 4; ++i) rt.tick();
        ok(rt.model().tab_count() == tabs0, "\"save\" in the dialog saves, then closes the tab");
    }

    // ⌘P: the project's files, fuzzy-found, from any tab
    rt.press_key(Key::p, primary());
    ok(rt.model().quick.open, "the quick-open key opens the file finder");
    rt.send(App::Msg{App::QuickListed{"/tmp/hand-selftest", {"src/app/editor.cpp", "src/app/pane.cpp", "README.md"}, false}});
    rt.window().type("pane");
    rt.tick();
    ok(!quick_open_ranked(rt.model()).empty() &&
           rt.model().quick.files[static_cast<std::size_t>(quick_open_ranked(rt.model())[0].first)] == "src/app/pane.cpp",
       "...and finds the file by name");
    rt.press_key(Key::escape);
    ok(!rt.model().quick.open, "escape closes it");

    // ⌘⇧F: text across the project, landing on the line
#if defined(__APPLE__)
    rt.press_key(Key::f, Mods{.shift = true, .super = true});
#else
    rt.press_key(Key::f, Mods{.shift = true, .ctrl = true});
#endif
    ok(rt.model().search_all.open, "the project-search key opens the search");
    rt.window().type("needle");
    rt.tick();
    const std::uint64_t tok = rt.model().search_all.token;
    rt.send(App::Msg{App::SearchAllDone{tok - 1, "/tmp/hand-selftest", {{"stale.cpp", 0, 0, 6, "needle"}}, false, ""}});
    ok(rt.model().search_all.hits.empty(), "an older query's answer is dropped");
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

    // a binary is not a tab: it goes to its app and the waiting tab closes
    rt.send(App::Msg{App::OpenFile{"/tmp/hand-selftest/logo.png"}});
    rt.send(App::Msg{App::FileLoaded{rt.model().tab().sid, "/tmp/hand-selftest/logo.png", "", "", true}});
    for (int i = 0; i < 3; ++i) rt.tick();
    ok(rt.model().tab_count() == tabs0 && !rt.model().tab().file, "a binary goes to its own app, no tab left behind");
    if (rt.model().active() != shell_tab) rt.send(App::Msg{App::SelectTab{TabIndex{shell_tab}}});
}

}  // namespace hand::cli::checks
