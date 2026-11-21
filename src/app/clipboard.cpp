// hand::App — text in and out: copy, paste (with its guard), search, hints, copy mode.

#include "detail.hpp"

#include <hand/app/copy_mode.hpp>

#include <hand/core/shell.hpp>

#include <mayag/widget/ssh.hpp>
#include <mayag/widget/term_dialog.hpp>
#include <mayag/widget/toast.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace hand {

using namespace mayag;
using namespace mayag::dsl;
using namespace hand::detail;

namespace {
/// Keep the copy-mode cursor on screen: scroll (smoothly) just enough.
void reveal(Model& m, std::int64_t row) {
    const auto info = m.term->scroll_info();
    const std::int64_t first = static_cast<std::int64_t>(info.total - info.visible) - static_cast<std::int64_t>(std::lround(m.scroll_goal));
    if (row < first) App::scroll_by(m, static_cast<float>(first - row));
    else if (row >= first + info.visible) App::scroll_by(m, -static_cast<float>(row - (first + info.visible) + 1));
}

/// The selection copy mode's visual mode implies.
void select(Model& m, const CopyMode& cm) {
    using V = CopyMode::Visual;
    if (cm.visual == V::none) { m.term->select_clear(); return; }
    m.term->select_abs(cm.anchor_row, cm.anchor_col, cm.row, cm.col, cm.visual == V::lines ? 1 : cm.visual == V::block ? 2 : 0);
}
}  // namespace

App::Cmd App::update(Model& m, HintPick h) {
    if (!m.hint || h.hint >= m.hint->hints.size()) { m.hint.reset(); m.grid_key = 0; return {}; }
    const Term::Hint hint = m.hint->hints[h.hint];
    m.hint.reset();
    m.grid_key = 0;
    if (h.use == HintUse::insert) {   // type it at the prompt
        if (m.pty) m.pty->write(m.term->encode_paste(hint.text));
        return {};
    }
    if (hint.kind == Term::Hint::Kind::url) return update(m, OpenUrl{hint.text});
    m.toast = Toast{.title = "copied", .body = hint.text, .kind = Toast::Kind::info};
    m.toast_at = m.time;
    return WriteClipboard{hint.text};
}

App::Cmd App::update(Model& m, CopyKey k) {
    if (!m.copy) return {};
    std::vector<std::int64_t> prompts;
    for (const auto& c : m.term->commands()) prompts.push_back(c.prompt_row);
    const CopyScreen screen{.last_row = std::max<std::int64_t>(m.term->total_rows() - 1, 0),
                            .max_col = std::max(m.term->cols() - 1, 0),
                            .half_page = std::max(m.term->rows() / 2, 1),
                            .cursor_row = m.term->cursor_abs(), .cursor_col = m.term->cursor().col,
                            .prompts = std::move(prompts),
                            .line = [t = m.term](std::int64_t r) { return t->line(r); }};
    const CopyStep step = copy_step(*m.copy, k.key, k.mods, k.text, screen);
    using Do = CopyStep::Do;
    switch (step.act) {
        case Do::nothing: return {};
        case Do::leave:
            m.copy.reset();
            m.term->select_clear();
            break;
        case Do::unmark:
            m.copy = step.mode;
            m.term->select_clear();
            break;
        case Do::search:
            m.copy.reset();
            return update(m, SearchToggle{});
        case Do::yank: {
            // nothing marked: yank the line under the cursor
            if (m.copy->visual == CopyMode::Visual::none) m.term->select_abs(m.copy->row, 0, m.copy->row, screen.max_col, 1);
            Cmd c = update(m, Copy{});
            m.copy.reset();
            return c;
        }
        case Do::moved:
            m.copy = step.mode;
            select(m, *m.copy);
            reveal(m, m.copy->row);
            break;
    }
    m.grid_key = 0;
    return {};
}

App::Cmd App::update(Model& m, CopyOutput) {
    const auto cs = m.term->commands();
    for (auto it = cs.rbegin(); it != cs.rend(); ++it) {
        if (!it->ran || !it->finished) continue;
        const std::string out = m.term->output_text(*it);
        m.toast = Toast{.title = "copied the output of", .body = m.term->command_text(*it), .kind = Toast::Kind::info};
        m.toast_at = m.time;
        return WriteClipboard{out};
    }
    m.toast = Toast{.title = "no command output yet", .body = "needs shell integration (settings \u203A shell)", .kind = Toast::Kind::warning};
    m.toast_at = m.time;
    return {};
}

App::Cmd App::update(Model& m, Copy) {
    m.selecting = false;
    m.sel_edge = 0.0f;
    if (!m.term->has_selection()) return {};
    const auto si = m.term->selection_info();
    m.copy_flash_at = m.time;
    m.toast = Toast{.title = "copied", .body = std::to_string(si.lines) + (si.lines == 1 ? " line, " : " lines, ") +
                                             std::to_string(si.chars) + " chars" + (si.block ? " (block)" : ""),
                    .kind = Toast::Kind::info};
    m.toast_at = m.time;
    return WriteClipboard{m.term->selection_text()};
}

App::Cmd App::update(Model& m, Pasted p) {
    if (p.text.empty() || m.link.dropped) return {};
    // Ask first (paste.confirm): risky lines, multi-line, or only into programs
    // that would run each line as it lands.
    if (m.ask.has_value()) return {};   // one question at a time
    if (p.confirm == Confirm::asking) {
        std::string body = p.text;
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
        const bool multi = body.find('\n') != std::string::npos;
        bool risky = false;
        for (const auto& l : split_lines(body)) risky = risky || risk_of(l).flag;
        bool ask = false;
        switch (m.cfg.paste_confirm) {
            case PasteConfirm::never: break;
            case PasteConfirm::risky: ask = risky; break;
            case PasteConfirm::multiline: ask = multi || risky; break;
            case PasteConfirm::unbracketed: ask = (multi || risky) && !m.term->bracketed(); break;
        }
        if (ask) {
            m.pending_paste = p.text;
            m.ask = Ask{.kind = Ask::Kind::paste, .focus = 2};
            return {};
        }
    }
    if (!m.pty) return {};
    m.term->select_clear();
    m.term->scroll_to_bottom();
    m.pty->write(m.term->encode_paste(p.text));
    settle_scroll(m);
    return {};
}

App::Cmd App::update(Model& m, SearchToggle) {
    m.search_open = !m.search_open;
    if (!m.search_open) { m.term->search_clear(); m.search_query.clear(); }
    else if (m.term->has_selection()) {   // seed with the selection, like every editor
        const std::string sel = m.term->selection_text();
        if (!sel.empty() && sel.find('\n') == std::string::npos) { m.search_query.set(sel); (void)m.term->search(sel); }
    }
    return {};
}

App::Cmd App::update(Model& m, SearchStep s) {
    if (s.step == Step::forward) m.term->search_next(); else m.term->search_prev();
    settle_scroll(m);   // fly to the match
    return {};
}

App::Cmd App::update(Model& m, OpenUrl u) {
    if (u.url.empty()) return {};
    std::string url = u.url;
    if (url.rfind("www.", 0) == 0) url = "https://" + url;
    if (!m.cfg.demo) spawn_detached({m.cfg.url_launcher, url});
    return {};
}

App::Cmd App::update(Model&, PasteReq) {
    return ReadClipboard<Msg>{[](std::string s) -> Msg { return Pasted{std::move(s)}; }};
}

}  // namespace hand
