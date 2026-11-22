// hand::App — files open in tabs: the editor.
//
//   OpenFile ─▶ a tab with a FileDoc (or the tab already showing that file)
//              ─▶ fx::read_file ─▶ FileLoaded ─▶ text_editor_ops::open
//   EditorOp ─▶ the editor's state (mayag text_editor), then what it asked:
//              save ─▶ fx::write_file ─▶ FileSaved   copy ─▶ the clipboard
//              paste ─▶ ReadClipboard ─▶ EditorPasted
//
// A file tab has no pty and never gets one (drain skips it: no launch). The
// keyboard goes to the editor whole -- only the window's own chords (tabs,
// settings, the file pane) are taken first, in input_keys.cpp.

#include "detail.hpp"
#include "fx/fx.hpp"

#include <mayag/widget/text_editor.hpp>
#include <mayag/widget/toast.hpp>

#include <algorithm>
#include <utility>

namespace hand {

using namespace mayag;
using namespace mayag::dsl;
using namespace hand::detail;

namespace {

/// The file tab with this id, if it still exists.
FileDoc* doc_of(Model& m, SessionId sid) {
    const auto i = m.tabs.find(sid);
    if (!i || !m.at(*i).file) return nullptr;
    return &*m.at(*i).file;
}

std::string name_of(const std::string& path) {
    const auto s = path.find_last_of("/\\");
    return s == std::string::npos ? path : path.substr(s + 1);
}

}  // namespace

App::Cmd App::update(Model& m, OpenFile o) {
    if (o.path.empty()) return {};
    // already open: that tab
    for (std::size_t i = 0; i < m.tab_count(); ++i)
        if (m.at(i).file && m.at(i).file->path == o.path) {
            activate(m, i);
            m.pane.ui.focused = false;
            return SetTitle{name_of(o.path)};
        }
    Session se;
    se.sid = m.session_ids.next();
    se.opened_at = m.time;
    // a file tab's Term is a 2x1 blank: nothing draws it, but every query on
    // "the active tab's screen" still has a screen to ask
    se.term = std::make_shared<Term>(2, 1, m.palette, 0, m.env.entropy ^ se.sid.get());
    se.title = name_of(o.path);
    se.file = FileDoc{.path = o.path};
    const SessionId sid = se.sid;
    const bool first_split = m.tab_count() == 1 && !m.cfg.strip_always();
    const std::size_t at = m.tabs.insert(m.active() + 1, std::move(se));   // beside the tab it came from
    m.strip.switch_dir = 1;
    activate(m, at);
    m.strip.switch_t = m.cfg.motion ? 0.0f : 1.0f;
    if (first_split && !m.cfg.motion) m.strip.t = 1.0f;
    m.pane.ui.focused = false;   // the editor has the keyboard
    fit(m);
    if (m.cfg.demo) return SetTitle{name_of(o.path)};
    return Cmd::batch(Cmd::task(fx::read_file, sid, o.path), Cmd{SetTitle{name_of(o.path)}});
}

App::Cmd App::update(Model& m, FileLoaded l) {
    FileDoc* d = doc_of(m, l.sid);
    if (d == nullptr) return {};   // closed while it was read
    d->loading = false;
    if (l.binary) {
        // not text: its own app opens it (Preview, an image viewer), and the
        // tab that was waiting for it goes away
        const auto i = m.tabs.find(l.sid);
        std::vector<Cmd> cmds;
        if (!m.cfg.demo) cmds.push_back(Cmd::run(fx::open_path, l.path));
        if (i) cmds.push_back(update(m, CloseTab{TabIndex{*i}, Confirm::given}));
        return Cmd::batch(std::move(cmds));
    }
    if (!l.error.empty()) {
        d->error = l.error;
        return {};
    }
    d->st = text_editor_ops::open(l.text, l.path);
    return {};
}

App::Cmd App::update(Model& m, EditorOp e) {
    Session& se = m.tab();
    if (!se.file || se.file->loading || !se.file->error.empty()) return {};
    e.op(se.file->st);
    m.cursor_on = true;   // a keystroke shows the caret, as in the terminal
    return editor_actions(m);
}

App::Cmd App::editor_actions(Model& m) {
    Session& se = m.tab();
    if (!se.file) return {};
    std::vector<Cmd> cmds;
    for (auto& a : std::exchange(se.file->st.out, {})) {
        switch (a.kind) {
            case TextEditorAction::Kind::save:
                cmds.push_back(save_file(m, m.active()));
                break;
            case TextEditorAction::Kind::copy:
                cmds.push_back(WriteClipboard{std::move(a.text)});
                break;
            case TextEditorAction::Kind::paste: {
                const SessionId sid = se.sid;
                cmds.push_back(ReadClipboard<Msg>{[sid](std::string s) -> Msg { return EditorPasted{sid, std::move(s)}; }});
                break;
            }
        }
    }
    return Cmd::batch(std::move(cmds));
}

App::Cmd App::save_file(Model& m, std::size_t i) {
    if (i >= m.tab_count() || !m.at(i).file) return {};
    FileDoc& d = *m.at(i).file;
    if (d.loading || !d.error.empty()) return {};   // nothing was opened: nothing to write over the file
    const std::uint64_t point = d.st.ed.save_point();
    d.saving = point;
    d.save_error.clear();
    if (m.cfg.demo) {   // shots and the self-test never write: the save simply lands
        return update(m, FileSaved{m.at(i).sid, point, {}});
    }
    return Cmd::task(fx::write_file, m.at(i).sid, d.path, text_editor_ops::file_text(d.st), point);
}

App::Cmd App::update(Model& m, FileSaved s) {
    FileDoc* d = doc_of(m, s.sid);
    if (d == nullptr) return {};
    if (d->saving == s.point) d->saving.reset();
    if (!s.error.empty()) {
        d->save_error = s.error;   // the text is still here: a toast and the status line, never a dead tab
        d->close_after_save = false;
        m.toast.card = Toast{.title = "not saved", .body = name_of(d->path) + ": " + s.error, .kind = Toast::Kind::error};
        m.toast.at = m.time;
        return {};
    }
    text_editor_ops::saved(d->st, s.point);
    if (d->close_after_save && !d->st.ed.dirty()) {
        d->close_after_save = false;
        if (const auto i = m.tabs.find(s.sid)) return update(m, CloseTab{TabIndex{*i}, Confirm::given});
    }
    return {};
}

App::Cmd App::update(Model& m, EditorPasted p) {
    FileDoc* d = doc_of(m, p.sid);
    if (d == nullptr || p.text.empty()) return {};
    text_editor_ops::paste(d->st, p.text);
    return {};
}

}  // namespace hand
