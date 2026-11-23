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
    if (l.reload) {   // the file changed on disk, or "reload from disk"
        if (!l.error.empty()) return {};
        d->disk_mtime = l.mtime;
        text_editor_ops::disk_text(d->st, l.text);
        m.toast.card = Toast{.title = "reloaded", .body = name_of(d->path) + " changed on disk", .kind = Toast::Kind::info};
        m.toast.at = m.time;
        return {};
    }
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
    d->disk_mtime = l.mtime;
    return {};
}

App::Cmd App::update(Model& m, EditorOp e) {
    Session& se = m.tab();
    if (!se.file || se.file->loading || !se.file->error.empty()) return {};
    e.op(se.file->st);
    if (!m.cfg.motion) (void)text_editor_ops::step(se.file->st, 0.0, false);   // no motion: everything snaps
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
            case TextEditorAction::Kind::reload:
                if (!m.cfg.demo) cmds.push_back(Cmd::task(fx::reread_file, se.sid, se.file->path));
                break;
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
    d->disk_mtime = s.mtime;   // our own write: not an outside change
    d->st.disk_changed = false;
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

// ── the file on disk ───────────────────────────────────────────────────────

App::Cmd App::update(Model& m, FileWatch) {
    const Session& se = m.tab();
    if (!se.file || se.file->loading || !se.file->error.empty() || se.file->saving || m.cfg.demo) return {};
    return Cmd::task(fx::stat_file, se.sid, se.file->path);
}

App::Cmd App::update(Model& m, FileStat s) {
    FileDoc* d = doc_of(m, s.sid);
    if (d == nullptr || d->saving || s.mtime == 0 || s.mtime == d->disk_mtime) return {};
    // changed by someone else. Unedited: just take it. Edited: say so and
    // keep the edits until the user chooses (the banner's "reload").
    if (!d->st.ed.dirty()) return Cmd::task(fx::reread_file, s.sid, d->path);
    d->disk_mtime = s.mtime;   // asked once per change, not every poll
    d->st.disk_changed = true;
    return {};
}

// ── ⌘P: the project's files ────────────────────────────────────────────────

namespace {
/// The files, ranked against the query: (index into files, hits). An empty
/// query keeps git's order.
std::vector<std::pair<int, FuzzyMatch>> quick_ranked(const Model& m) {
    std::vector<std::pair<int, FuzzyMatch>> out;
    const auto& q = m.quick.q.text;
    for (std::size_t i = 0; i < m.quick.files.size(); ++i) {
        auto fm = fuzzy_match(q, m.quick.files[i]);
        if (!fm.ok) continue;
        // the file NAME matching counts more than its folders
        const auto& f = m.quick.files[i];
        const auto slash = f.find_last_of('/');
        const std::size_t name_at = slash == std::string::npos ? 0 : slash + 1;
        int bonus = 0;
        for (auto p : fm.positions) bonus += p >= name_at ? 2 : 0;
        fm.score += bonus - static_cast<int>(f.size() / 16);   // shorter paths win ties
        out.emplace_back(static_cast<int>(i), std::move(fm));
        if (out.size() > 4000) break;
    }
    if (!q.empty()) std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second.score > b.second.score; });
    if (out.size() > 200) out.resize(200);
    return out;
}
/// Where ⌘P starts: the front tab's folder (list_project climbs to its repo's top).
std::string quick_root(const Model& m) {
    const Session& se = m.tab();
    std::string dir = se.file ? se.file->path.substr(0, se.file->path.find_last_of('/')) : se.term ? se.term->cwd() : std::string{};
    if (dir.empty()) dir = m.env.cwd;
    return dir;
}
}  // namespace

App::Cmd App::update(Model& m, QuickOpen) {
    if (m.quick.open) { m.quick.open = false; return {}; }
    m.quick.open = true;
    m.quick.q.set({});
    m.quick.sel = 0;
    const std::string root = quick_root(m);
    if (!m.quick.files.empty() && (root == m.quick.root || root.rfind(m.quick.root + "/", 0) == 0)) return {};   // still inside the listed project
    m.quick.loading = true;
    m.quick.files.clear();
    if (m.cfg.demo) return {};
    return Cmd::task(fx::list_project, root);
}

App::Cmd App::update(Model& m, QuickListed l) {
    m.quick.loading = false;
    m.quick.root = l.root;   // the repo's top when it is one: paths are relative to it
    m.quick.files = std::move(l.files);
    m.quick.truncated = l.truncated;
    return {};
}

App::Cmd App::update(Model& m, QuickKey k) {
    if (!m.quick.open) return {};
    const int n = static_cast<int>(quick_ranked(m).size());
    const auto in = picker_key(m.quick.q, k.key, k.mods, m.quick.sel, n);
    if (in.act == PickerIntent::Act::close) { m.quick.open = false; return {}; }
    if (in.act == PickerIntent::Act::choose) return update(m, QuickPick{in.index});
    return {};
}

App::Cmd App::update(Model& m, QuickText t) {
    if (!m.quick.open) return {};
    m.quick.q.insert(t.text);
    m.quick.sel = 0;
    return {};
}

App::Cmd App::update(Model& m, QuickPick p) {
    const auto r = quick_ranked(m);
    m.quick.open = false;
    if (p.index < 0 || p.index >= static_cast<int>(r.size())) return {};
    const std::string& rel = m.quick.files[static_cast<std::size_t>(r[static_cast<std::size_t>(p.index)].first)];
    return update(m, OpenFile{m.quick.root + "/" + rel});
}

std::vector<std::pair<int, FuzzyMatch>> quick_open_ranked(const Model& m) { return quick_ranked(m); }

}  // namespace hand
