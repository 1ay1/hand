// hand::App — text in and out: copy, paste (with its guard), search, hints, copy mode.

#include "detail.hpp"
#include "fx/fx.hpp"

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
    const auto info = m.tab().term->scroll_info();
    const std::int64_t first = static_cast<std::int64_t>(info.total - info.visible) - static_cast<std::int64_t>(std::lround(m.tab().scroll_goal));
    if (row < first) App::scroll_by(m, static_cast<float>(first - row));
    else if (row >= first + info.visible) App::scroll_by(m, -static_cast<float>(row - (first + info.visible) + 1));
}

/// The selection copy mode's visual mode implies.
void select(Model& m, const CopyMode& cm) {
    using V = CopyMode::Visual;
    if (cm.visual == V::none) { m.tab().term->select_clear(); return; }
    m.tab().term->select_abs(cm.anchor_row, cm.anchor_col, cm.row, cm.col, cm.visual == V::lines ? 1 : cm.visual == V::block ? 2 : 0);
}
}  // namespace

App::Cmd App::update(Model& m, HintPick h) {
    if (!m.tab().hint || h.hint >= m.tab().hint->hints.size()) { m.tab().hint.reset(); m.tab().grid_key = 0; return {}; }
    const Term::Hint hint = m.tab().hint->hints[h.hint];
    m.tab().hint.reset();
    m.tab().grid_key = 0;
    if (h.use == HintUse::insert) {   // type it at the prompt
        if (m.tab().pty) m.tab().input += m.tab().term->encode_paste(hint.text);
        return {};
    }
    if (hint.kind == Term::Hint::Kind::url) return update(m, OpenUrl{hint.text});
    m.toast.card = Toast{.title = "copied", .body = hint.text, .kind = Toast::Kind::info};
    ++m.toast.serial;
    return WriteClipboard{hint.text};
}

App::Cmd App::update(Model& m, CopyKey k) {
    if (!m.tab().copy) return {};
    std::vector<std::int64_t> prompts;
    for (const auto& c : m.tab().term->commands()) prompts.push_back(c.prompt_row);
    const CopyScreen screen{.last_row = std::max<std::int64_t>(m.tab().term->total_rows() - 1, 0),
                            .max_col = std::max(m.tab().term->cols() - 1, 0),
                            .half_page = std::max(m.tab().term->rows() / 2, 1),
                            .cursor_row = m.tab().term->cursor_abs(), .cursor_col = m.tab().term->cursor().col,
                            .prompts = std::move(prompts),
                            .line = [t = m.tab().term](std::int64_t r) { return t->line(r); }};
    const CopyStep step = copy_step(*m.tab().copy, k.key, k.mods, k.text, screen);
    using Do = CopyStep::Do;
    switch (step.act) {
        case Do::nothing: return {};
        case Do::leave:
            m.tab().copy.reset();
            m.tab().term->select_clear();
            break;
        case Do::unmark:
            m.tab().copy = step.mode;
            m.tab().term->select_clear();
            break;
        case Do::search:
            m.tab().copy.reset();
            return update(m, SearchToggle{});
        case Do::yank: {
            // nothing marked: yank the line under the cursor
            if (m.tab().copy->visual == CopyMode::Visual::none) m.tab().term->select_abs(m.tab().copy->row, 0, m.tab().copy->row, screen.max_col, 1);
            Cmd c = update(m, Copy{});
            m.tab().copy.reset();
            return c;
        }
        case Do::moved:
            m.tab().copy = step.mode;
            select(m, *m.tab().copy);
            reveal(m, m.tab().copy->row);
            break;
    }
    m.tab().grid_key = 0;
    return {};
}

App::Cmd App::update(Model& m, CopyOutput) {
    Session& se = m.tab();
    const auto cs = se.term->commands();
    for (auto it = cs.rbegin(); it != cs.rend(); ++it) {
        if (!it->ran || !it->finished) continue;
        const std::string out = se.term->output_text(*it);
        m.toast.card = Toast{.title = "copied the output of", .body = se.term->command_text(*it), .kind = Toast::Kind::info};
        ++m.toast.serial;
        return WriteClipboard{out};
    }
    m.toast.card = Toast{.title = "no command output yet", .body = "needs shell integration (settings \u203A shell)", .kind = Toast::Kind::warning};
    ++m.toast.serial;
    return {};
}

App::Cmd App::update(Model& m, Copy) {
    Session& se = m.tab();
    se.selecting = false;
    se.sel_edge = 0.0f;
    if (!se.term->has_selection()) return {};
    const auto si = se.term->selection_info();
    se.copy_flash_at = m.time;
    m.toast.card = Toast{.title = "copied", .body = std::to_string(si.lines) + (si.lines == 1 ? " line, " : " lines, ") +
                                             std::to_string(si.chars) + " chars" + (si.block ? " (block)" : ""),
                    .kind = Toast::Kind::info};
    ++m.toast.serial;
    return WriteClipboard{se.term->selection_text()};
}

App::Cmd App::update(Model& m, Pasted p) {
    Session& se = m.tab();
    if (p.text.empty() || se.link.dropped) return {};
    // Ask first (paste.confirm): risky lines, multi-line, or only into programs
    // that would run each line as it lands.
    if (m.dialog.ask.has_value()) return {};   // one question at a time
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
            case PasteConfirm::unbracketed: ask = (multi || risky) && !se.term->bracketed(); break;
        }
        if (ask) {
            m.dialog.ask = Ask{.kind = Ask::Kind::paste, .focus = 2, .paste = p.text};
            return {};
        }
    }
    if (!se.pty) return {};
    se.term->select_clear();
    se.term->scroll_to_bottom();
    se.input += se.term->encode_paste(p.text);
    settle_scroll(m);
    return {};
}

App::Cmd App::update(Model& m, SearchToggle) {
    Session& se = m.tab();
    se.search_open = !se.search_open;
    if (!se.search_open) { se.term->search_clear(); se.search_query.clear(); }
    else if (se.term->has_selection()) {   // seed with the selection, like every editor
        const std::string sel = se.term->selection_text();
        if (!sel.empty() && sel.find('\n') == std::string::npos) { se.search_query.set(sel); (void)se.term->search(sel); }
    }
    return {};
}

App::Cmd App::update(Model& m, SearchKey k) {
    Session& se = m.tab();
    if (se.search_query.handle_key(k.key, k.mods)) (void)se.term->search(se.search_query.text);
    return {};
}

App::Cmd App::update(Model& m, SearchText t) {
    Session& se = m.tab();
    se.search_query.insert(t.text);
    (void)se.term->search(se.search_query.text);
    return {};
}

App::Cmd App::update(Model& m, SearchStep s) {
    if (s.step == Step::forward) m.tab().term->search_next(); else m.tab().term->search_prev();
    settle_scroll(m);   // fly to the match
    return {};
}

App::Cmd App::update(Model& m, OpenUrl u) {
    if (u.url.empty()) return {};
    std::string url = u.url;
    if (url.rfind("www.", 0) == 0) url = "https://" + url;
    if (m.cfg.demo) return {};
    return Cmd::run(fx::open_url, m.cfg.url_launcher, url);
}

App::Cmd App::update(Model&, PasteReq) {
    return ReadClipboard<Msg>{[](std::string s) -> Msg { return Pasted{std::move(s)}; }};
}

}  // namespace hand
