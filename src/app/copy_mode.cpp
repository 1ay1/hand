// hand::copy_mode — the motions.

#include <hand/app/copy_mode.hpp>

#include <algorithm>
#include <utility>

namespace hand {

namespace {
bool word_ch(char32_t c) { return c != U' ' && c != U'\0' && c != U'\t'; }
}  // namespace

CopyStep copy_step(const CopyMode& cm, mayag::Key key, mayag::Mods mods, const std::string& t, const CopyScreen& s) {
    using K = mayag::Key;
    using V = CopyMode::Visual;
    using Do = CopyStep::Do;
    CopyMode m = cm;
    const auto out = [&](Do d) { return CopyStep{d, m}; };
    // leaving
    if (key == K::escape || t == "q") {
        if (m.visual != V::none && key == K::escape) { m.visual = V::none; return out(Do::unmark); }
        return out(Do::leave);
    }
    if (t == "y" || key == K::enter) return out(Do::yank);
    if (t == "/") return out(Do::search);
    // visual modes: the same key again turns it off
    const auto visual = [&](V v) {
        if (m.visual == v) { m.visual = V::none; return out(Do::unmark); }
        if (m.visual == V::none) { m.anchor_row = m.row; m.anchor_col = m.col; }
        m.visual = v;
        return out(Do::moved);
    };
    if (t == "v") return visual(V::chars);
    if (t == "V") return visual(V::lines);
    if (key == K::v && mods.ctrl) return visual(V::block);
    // motions
    if (t == "h" || key == K::left) m.col = std::max(m.col - 1, 0);
    else if (t == "l" || key == K::right) m.col = std::min(m.col + 1, s.max_col);
    else if (t == "k" || key == K::up) m.row = std::max<std::int64_t>(m.row - 1, 0);
    else if (t == "j" || key == K::down) m.row = std::min(m.row + 1, s.last_row);
    else if ((key == K::u && mods.ctrl) || key == K::page_up) m.row = std::max<std::int64_t>(m.row - s.half_page, 0);
    else if ((key == K::d && mods.ctrl) || key == K::page_down) m.row = std::min(m.row + s.half_page, s.last_row);
    else if (t == "0" || key == K::home) m.col = 0;
    else if (t == "^") {
        const auto l = s.line(m.row);
        int c = 0;
        while (c < static_cast<int>(l.size()) && !word_ch(l[static_cast<std::size_t>(c)])) ++c;
        m.col = std::min(c, s.max_col);
    } else if (t == "$" || key == K::end) {
        const auto l = s.line(m.row);
        int c = static_cast<int>(l.size()) - 1;
        while (c > 0 && !word_ch(l[static_cast<std::size_t>(c)])) --c;
        m.col = std::max(c, 0);
    } else if (t == "g") { m.row = 0; m.col = 0; }
    else if (t == "G") { m.row = s.cursor_row; m.col = s.cursor_col; }
    else if (t == "w" || t == "e") {   // next word start / end, across lines
        std::int64_t r = m.row;
        int c = m.col;
        auto l = s.line(r);
        const auto at = [&](int x) { return x < static_cast<int>(l.size()) && word_ch(l[static_cast<std::size_t>(x)]); };
        const auto to_next_word = [&] {
            while (true) {
                while (c < static_cast<int>(l.size()) && !at(c)) ++c;
                if (c < static_cast<int>(l.size()) || r >= s.last_row) break;
                ++r; l = s.line(r); c = 0;
            }
        };
        if (t == "w") { while (at(c)) ++c; to_next_word(); }
        else { ++c; to_next_word(); while (at(c + 1)) ++c; }
        m.row = r;
        m.col = std::clamp(c, 0, s.max_col);
    } else if (t == "b") {   // previous word start, across lines
        std::int64_t r = m.row;
        int c = m.col - 1;
        auto l = s.line(r);
        const auto at = [&](int x) { return x >= 0 && x < static_cast<int>(l.size()) && word_ch(l[static_cast<std::size_t>(x)]); };
        while (true) {
            while (c >= 0 && !at(c)) --c;
            if (c >= 0 || r == 0) break;
            --r; l = s.line(r); c = static_cast<int>(l.size()) - 1;
        }
        while (at(c - 1)) --c;
        m.row = r;
        m.col = std::clamp(c, 0, s.max_col);
    } else if (t == "{" || t == "}") {   // the previous / next prompt
        std::int64_t best = -1;
        for (const std::int64_t pr : s.prompts) {
            if (pr < 0) continue;
            if (t == "{" && pr < m.row && pr > best) best = pr;
            if (t == "}" && pr > m.row && (best < 0 || pr < best)) best = pr;
        }
        if (best >= 0) { m.row = best; m.col = 0; }
    } else if (t == "o" && m.visual != V::none) {   // swap to the other end
        std::swap(m.row, m.anchor_row);
        std::swap(m.col, m.anchor_col);
    } else {
        return out(Do::nothing);
    }
    return out(Do::moved);
}

}  // namespace hand
