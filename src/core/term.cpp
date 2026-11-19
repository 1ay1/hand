// hand::Term — toe ⇄ mayag. The only translation unit that includes toe.

#include <hand/core/term.hpp>

#include <toe/input/keymap.hpp>
#include <toe/term/update.hpp>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <regex>
#include <variant>

namespace hand {

using toe::term::Screen;
namespace dsl = mayag::dsl;

namespace {

toe::Rgb rgb_of(std::uint32_t v) {
    return toe::rgb(static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v));
}
std::uint32_t u32(toe::Rgb c) { return (std::uint32_t{c.r} << 16) | (std::uint32_t{c.g} << 8) | c.b; }

void put_utf8(std::string& s, char32_t cp) {
    if (cp < 0x80) s.push_back(static_cast<char>(cp));
    else if (cp < 0x800) { s.push_back(static_cast<char>(0xC0 | (cp >> 6))); s.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) {
        s.push_back(static_cast<char>(0xE0 | (cp >> 12))); s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        s.push_back(static_cast<char>(0xF0 | (cp >> 18))); s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

toe::Modifiers mods_of(mayag::Mods m) { return {.ctrl = m.ctrl, .alt = m.alt, .shift = m.shift}; }

/// One toe cell as a mayag cell: text (with combining marks), colours, attributes.
void convert_cell(const Screen& s, const toe::term::Palette& pal, const toe::term::Cell& cell, dsl::TermCell& d) {
    using toe::term::Attr;
    using toe::term::has;
    if (cell.spacer()) {
        d.text.clear();
        d.width = 0;
    } else {
        d.text.clear();
        put_utf8(d.text, cell.cp == 0 ? U' ' : cell.cp);
        if (cell.marks != 0)
            for (char32_t m : s.marks_of(cell)) put_utf8(d.text, m);
        d.width = cell.width;
    }
    const auto& pen = cell.pen;
    if (!std::holds_alternative<toe::term::DefaultColor>(pen.fg)) {
        d.fg = u32(pal.resolve(pen.fg, true));
        d.default_fg = false;
    }
    if (!std::holds_alternative<toe::term::DefaultColor>(pen.bg)) {
        d.bg = u32(pal.resolve(pen.bg, false));
        d.default_bg = false;
    }
    std::uint16_t f = 0;
    if (has(pen.attr, Attr::Bold)) f |= dsl::term_attr::bold;
    if (has(pen.attr, Attr::Faint)) f |= dsl::term_attr::dim;
    if (has(pen.attr, Attr::Italic)) f |= dsl::term_attr::italic;
    if (has(pen.attr, Attr::Reverse)) f |= dsl::term_attr::inverse;
    if (has(pen.attr, Attr::Hidden)) f |= dsl::term_attr::hidden;
    if (has(pen.attr, Attr::Strike)) f |= dsl::term_attr::strike;
    if (has(pen.attr, Attr::Overline)) f |= dsl::term_attr::overline;
    if (cell.link != 0) {
        f |= dsl::term_attr::link;
        d.link = cell.link;
    }
    d.flags = f;
    if (has(pen.attr, Attr::Underline)) {
        d.ul_style = static_cast<dsl::TermUnderline>(std::clamp(static_cast<int>(pen.underline), 1, 5));
        if (!std::holds_alternative<toe::term::DefaultColor>(pen.underline_color))
            d.ul_color = u32(pal.resolve(pen.underline_color, true));
    }
}

}  // namespace

/// A command block whose rows still make sense (prompt <= output <= end). A
/// `clear` wipes the screen under a block's feet and leaves it pointing at
/// rows that now hold something else: those are not drawn, not jumped to.
bool block_valid(const toe::term::CommandBlock& b) {
    if (b.prompt_row < 0) return false;
    if (b.output_row >= 0 && b.output_row < b.prompt_row) return false;
    if (b.finished() && b.end_row >= 0 && b.end_row < std::max(b.prompt_row, b.output_row)) return false;
    return true;
}

struct Term::Impl {
    toe::term::Model m;
    Impl(int cols, int rows, const Palette& pal, int scrollback)
        : m(toe::term::Config{.default_fg = rgb_of(pal.fg), .default_bg = rgb_of(pal.bg), .word_separators = {},
                              .scrollback_lines = scrollback},
            toe::Extent{cols, rows}) {
        // the scheme's 16 colours are the palette's first 16 slots
        auto& s = m.screen;
        for (std::uint8_t i = 0; i < 16; ++i)
            s.edit_color({Screen::ColorEdit::Target::index, i, false, rgb_of(pal.ansi[i])});
        s.set_default_palette_colors(rgb_of(pal.fg), rgb_of(pal.bg));
    }
    [[nodiscard]] std::int64_t abs_row(int vrow) const {
        // viewport row -> absolute row (history row 0 = oldest)
        const auto& s = m.screen;
        return static_cast<std::int64_t>(s.history_rows()) - s.scroll_offset() + vrow;
    }
};

Term::Term(int cols, int rows, const Palette& pal, int scrollback)
    : p_(std::make_unique<Impl>(std::max(cols, 2), std::max(rows, 1), pal, scrollback)) {}
Term::~Term() = default;
Term::Term(Term&&) noexcept = default;
Term& Term::operator=(Term&&) noexcept = default;

int Term::cols() const { return p_->m.screen.size().cols; }
int Term::rows() const { return p_->m.screen.size().rows; }

Effects Term::feed(std::string_view bytes) {
    Effects fx;
    if (bytes.empty()) return fx;
    ++epoch_; ++content_epoch_;
    for (auto& c : toe::term::feed_output(p_->m, bytes)) {
        std::visit([&](auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, toe::WriteChild>) fx.reply += x.bytes;
            else if constexpr (std::is_same_v<T, toe::SetTitle>) { fx.title = std::move(x.title); fx.title_set = true; }
            else if constexpr (std::is_same_v<T, toe::SetClipboard>) { fx.clipboard = std::move(x.text); fx.clipboard_set = true; }
            else if constexpr (std::is_same_v<T, toe::RingBell>) fx.bell = true;
        }, c);
    }
    return fx;
}

void Term::resize(int cols, int rows) {
    cols = std::max(cols, 2);
    rows = std::max(rows, 1);
    if (cols == this->cols() && rows == this->rows()) return;
    toe::term::resize(p_->m, toe::Extent{cols, rows});   // prompt-safe rewrap + command rows follow
    ++epoch_; ++content_epoch_;
}

// ── keys ───────────────────────────────────────────────────────────────────

void Term::set_palette(const Palette& pal) {
    auto& s = p_->m.screen;
    for (std::uint8_t i = 0; i < 16; ++i) s.edit_color({Screen::ColorEdit::Target::index, i, false, rgb_of(pal.ansi[i])});
    s.set_default_palette_colors(rgb_of(pal.fg), rgb_of(pal.bg));
    p_->m.cfg.default_fg = rgb_of(pal.fg);
    p_->m.cfg.default_bg = rgb_of(pal.bg);
    ++epoch_; ++content_epoch_;
}

void Term::set_prompt_protect(bool on) { p_->m.cfg.protect_prompt_on_resize = on; }

void Term::set_scrollback(int lines) {
    lines = std::max(lines, 0);
    if (static_cast<std::size_t>(lines) == p_->m.screen.max_history()) return;
    p_->m.screen.set_max_history(static_cast<std::size_t>(lines));
    p_->m.cfg.scrollback_lines = lines;
    ++epoch_;
}

std::string Term::encode_key(mayag::Key key, mayag::Mods mods) const {
    using K = mayag::Key;
    using S = toe::SpecialKey;
    toe::KeyEvent ev;
    ev.mods = mods_of(mods);
    const auto special = [&](S s) { ev.key = s; };
    const auto text = [&](std::string t) { ev.key = toe::TextInput{std::move(t)}; };
    switch (key) {
        case K::enter: special(S::Enter); break;
        case K::backspace: special(S::Backspace); break;
        case K::tab: special(S::Tab); break;
        case K::escape: special(S::Escape); break;
        case K::up: special(S::Up); break;
        case K::down: special(S::Down); break;
        case K::left: special(S::Left); break;
        case K::right: special(S::Right); break;
        case K::home: special(S::Home); break;
        case K::end: special(S::End); break;
        case K::page_up: special(S::PageUp); break;
        case K::page_down: special(S::PageDown); break;
        case K::del: special(S::Delete); break;
        case K::insert: special(S::Insert); break;
        case K::f1: special(S::F1); break;   case K::f2: special(S::F2); break;
        case K::f3: special(S::F3); break;   case K::f4: special(S::F4); break;
        case K::f5: special(S::F5); break;   case K::f6: special(S::F6); break;
        case K::f7: special(S::F7); break;   case K::f8: special(S::F8); break;
        case K::f9: special(S::F9); break;   case K::f10: special(S::F10); break;
        case K::f11: special(S::F11); break; case K::f12: special(S::F12); break;
        case K::space: if (!mods.ctrl && !mods.alt) return {}; text(" "); break;
        default: {
            // Letters/digits/punctuation reach us as Key only with ctrl/alt held
            // (plain typing arrives as TextEvent). Map them to their character.
            if (!mods.ctrl && !mods.alt) return {};
            const auto k = static_cast<int>(key);
            if (k >= static_cast<int>(K::a) && k <= static_cast<int>(K::z)) text(std::string(1, static_cast<char>('a' + k - static_cast<int>(K::a))));
            else if (k >= static_cast<int>(K::n0) && k <= static_cast<int>(K::n9)) text(std::string(1, static_cast<char>('0' + k - static_cast<int>(K::n0))));
            else switch (key) {
                case K::minus: text("-"); break;          case K::equal: text("="); break;
                case K::bracket_left: text("["); break;   case K::bracket_right: text("]"); break;
                case K::backslash: text("\\"); break;     case K::semicolon: text(";"); break;
                case K::quote: text("'"); break;          case K::comma: text(","); break;
                case K::period: text("."); break;         case K::slash: text("/"); break;
                case K::grave: text("`"); break;
                default: return {};
            }
        }
    }
    const auto& s = p_->m.screen;
    const toe::KeyContext ctx{.app_cursor_keys = s.app_cursor_keys(), .kitty_flags = s.kitty_keyboard_flags()};
    toe::KeyBuf buf{};
    const auto out = toe::encode_key(ev, ctx, buf);
    return std::string{out.begin(), out.end()};
}

std::string Term::encode_text(std::string_view utf8, mayag::Mods mods) const {
    if (utf8.empty()) return {};
    toe::KeyEvent ev;
    ev.key = toe::TextInput{std::string{utf8}};
    ev.mods = mods_of(mods);
    ev.mods.shift = false;   // the text already carries shift
    const auto& s = p_->m.screen;
    const toe::KeyContext ctx{.app_cursor_keys = s.app_cursor_keys(), .kitty_flags = s.kitty_keyboard_flags()};
    toe::KeyBuf buf{};
    const auto out = toe::encode_key(ev, ctx, buf);
    if (out.empty()) return std::string{utf8};   // long IME commits: plain UTF-8
    return std::string{out.begin(), out.end()};
}

std::string Term::encode_paste(std::string_view utf8) const {
    std::string body;
    body.reserve(utf8.size());
    for (char ch : utf8) {
        if (ch == '\x1b') continue;   // never let a paste end the bracket early
        body.push_back(ch == '\n' ? '\r' : ch);
    }
    if (p_->m.screen.bracketed_paste()) return "\x1b[200~" + body + "\x1b[201~";
    return body;
}

std::string Term::encode_focus(bool focused) const { return std::string{p_->m.screen.report_focus(focused)}; }

// ── mouse ──────────────────────────────────────────────────────────────────

bool Term::mouse_reporting() const { return p_->m.screen.mouse_mode() != Screen::MouseMode::off; }
bool Term::mouse_any_motion() const { return p_->m.screen.mouse_mode() == Screen::MouseMode::any; }

std::string Term::encode_mouse(int button, int col, int row, bool press, bool motion, mayag::Mods mods) const {
    const auto& s = p_->m.screen;
    const auto mode = s.mouse_mode();
    if (mode == Screen::MouseMode::off) return {};
    if (motion && mode != Screen::MouseMode::button && mode != Screen::MouseMode::any) return {};
    if (!press && !motion && mode == Screen::MouseMode::x10) return {};
    int cb = button;
    if (motion) cb += 32;
    if (mods.shift) cb += 4;
    if (mods.alt) cb += 8;
    if (mods.ctrl) cb += 16;
    char buf[64];
    if (s.mouse_sgr()) {
        const int n = std::snprintf(buf, sizeof buf, "\x1b[<%d;%d;%d%c", cb, col + 1, row + 1, press || motion ? 'M' : 'm');
        return std::string(buf, static_cast<std::size_t>(n));
    }
    if (!press && !motion) cb = 3;   // legacy release
    const auto enc = [](int v) { return static_cast<char>(std::min(v + 32, 255)); };
    return std::string{"\x1b[M"} + enc(cb) + enc(col + 1) + enc(row + 1);
}

// ── scrollback + selection ─────────────────────────────────────────────────

void Term::scroll(int lines) { p_->m.screen.scroll(lines); ++epoch_; }
void Term::scroll_to_bottom() { if (p_->m.screen.scroll_offset() != 0) { p_->m.screen.scroll_to_bottom(); ++epoch_; } }
bool Term::alt_screen() const { return p_->m.screen.on_alt_screen(); }
int Term::scroll_offset() const { return p_->m.screen.scroll_offset(); }
int Term::history() const { return p_->m.screen.history_rows(); }

void Term::select_begin(int col, int row, int clicks, bool block) {
    auto& s = p_->m.screen;
    const Screen::AbsPos at{p_->abs_row(row), std::clamp(col, 0, cols() - 1)};
    if (block) s.selection_begin(at, Screen::SelectMode::block);
    else if (clicks >= 3) s.selection_line(at);
    else if (clicks == 2) s.selection_word(at);
    else s.selection_begin(at, Screen::SelectMode::character);
    ++epoch_;
}
void Term::select_extend(int col, int row) {
    p_->m.screen.selection_extend({p_->abs_row(row), std::clamp(col, 0, cols() - 1)});
    ++epoch_;
}
void Term::select_abs(std::int64_t r0, int c0, std::int64_t r1, int c1, int mode) {
    auto& s = p_->m.screen;
    const Screen::AbsPos a{r0, std::clamp(c0, 0, cols() - 1)}, b{r1, std::clamp(c1, 0, cols() - 1)};
    if (mode == 1) s.selection_line(a);
    else s.selection_begin(a, mode == 2 ? Screen::SelectMode::block : Screen::SelectMode::character);
    s.selection_extend(b);
    ++epoch_;
}

Term::SelInfo Term::selection_info() const {
    const auto& s = p_->m.screen;
    SelInfo i;
    const auto r = s.selection_range();
    if (!r) return i;
    i.any = true;
    i.block = s.selection_mode() == Screen::SelectMode::block;
    i.lines_mode = s.selection_mode() == Screen::SelectMode::line;
    i.r0 = r->first.row; i.c0 = r->first.col;
    i.r1 = r->second.row; i.c1 = r->second.col;
    i.lines = static_cast<int>(i.r1 - i.r0 + 1);
    for (unsigned char ch : s.selected_text()) i.chars += (ch & 0xC0) != 0x80 ? 1 : 0;
    return i;
}

std::int64_t Term::view_to_abs(int row) const { return p_->m.screen.viewport_to_abs(row); }

std::u32string Term::line(std::int64_t abs) const {
    std::u32string out;
    for (const auto& c : p_->m.screen.abs_row_cells(abs)) out.push_back(c.spacer() ? U'\0' : (c.cp == 0 ? U' ' : c.cp));
    return out;
}

bool Term::bracketed() const { return p_->m.screen.bracketed_paste(); }

void Term::select_clear() { if (p_->m.screen.has_selection()) { p_->m.screen.selection_clear(); ++epoch_; } }
bool Term::has_selection() const { return p_->m.screen.has_selection(); }
std::string Term::selection_text() const { return p_->m.screen.selected_text(); }

// ── links + search ────────────────────────────────────────────────────────────

std::string Term::link_at(int col, int row) const {
    const auto& s = p_->m.screen;
    if (col < 0 || row < 0 || col >= cols() || row >= rows()) return {};
    if (auto u = s.link_at(row, col); !u.empty()) return std::string{u};
    return std::string{s.url_at(row, col)};
}

bool Term::hover(int col, int row) {
    const bool changed = p_->m.screen.set_hover(row, col);
    if (changed) ++epoch_;
    return changed;
}

std::size_t Term::search(std::string_view query) {
    // smart case: an upper-case letter makes it case-sensitive
    bool cs = false;
    for (char ch : query) cs = cs || (ch >= 'A' && ch <= 'Z');
    ++epoch_;
    if (query.empty()) { p_->m.screen.search_clear(); return 0; }
    return p_->m.screen.search(query, cs);
}
void Term::search_next() { p_->m.screen.search_next(); ++epoch_; }
void Term::search_prev() { p_->m.screen.search_prev(); ++epoch_; }
void Term::search_clear() { p_->m.screen.search_clear(); ++epoch_; }
std::size_t Term::search_count() const { return p_->m.screen.search_count(); }
std::size_t Term::search_current() const { return p_->m.screen.search_current(); }
std::vector<std::int64_t> Term::search_rows() const {
    std::vector<std::int64_t> v;
    for (const auto& mt : p_->m.screen.search_matches()) v.push_back(mt.start.row);
    return v;
}

// ── shell integration ─────────────────────────────────────────────────────────────────

namespace {
/// OSC 7 "file://host/path%20x" -> "/path x".
std::string uri_to_path(std::string w) {
    if (w.rfind("file://", 0) == 0) {
        w = w.substr(7);
        const auto slash = w.find('/');
        w = slash == std::string::npos ? std::string{} : w.substr(slash);   // drop the host
    }
    // percent-decode
    std::string out;
    for (std::size_t i = 0; i < w.size(); ++i) {
        if (w[i] == '%' && i + 2 < w.size()) {
            out.push_back(static_cast<char>(std::stoi(w.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else out.push_back(w[i]);
    }
    return out;
}
}  // namespace

std::vector<Term::Command> Term::commands() const {
    std::vector<Command> v;
    for (const auto& b : p_->m.commands.blocks()) {
        if (!block_valid(b)) continue;
        Command c;
        c.id = b.id;
        c.prompt_row = b.prompt_row;
        c.output_row = b.output_row;
        c.end_row = b.end_row;
        c.ran = b.output_row >= 0;
        c.finished = b.finished();
        c.exit_code = b.exit_code.value_or(0);
        c.duration_ms = b.duration_ms();
        c.start_ms = b.start_ms;
        c.cwd = uri_to_path(b.cwd);
        v.push_back(c);
    }
    return v;
}

namespace {
std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\n");
    const auto e = s.find_last_not_of(" \t\n");
    return b == std::string::npos ? std::string{} : s.substr(b, e - b + 1);
}
}  // namespace

std::string Term::command_text(const Command& c) const {
    for (const auto& b : p_->m.commands.blocks()) {
        if (!block_valid(b)) continue;
        if (b.id != c.id) continue;
        if (b.input_row < 0 || b.output_row < 0) return {};
        return trim(p_->m.screen.text_between_abs(b.input_row, std::max(b.output_row, b.input_row + 1), b.input_col));
    }
    return {};
}

std::string Term::output_text(const Command& c) const {
    if (c.output_row < 0) return {};
    const std::int64_t end = c.end_row >= 0 ? c.end_row : p_->m.screen.total_rows();
    return trim(p_->m.screen.text_between_abs(c.output_row, end));
}

bool Term::jump_prompt(int dir) {
    auto& s = p_->m.screen;
    const std::int64_t top = s.viewport_to_abs(0);
    std::int64_t best = -1;
    for (const auto& b : p_->m.commands.blocks()) {
        if (!block_valid(b)) continue;
        const std::int64_t r = b.prompt_row;
        if (r < 0) continue;
        if (dir < 0 && r < top && (best < 0 || r > best)) best = r;
        if (dir > 0 && r > top && (best < 0 || r < best)) best = r;
    }
    if (best < 0) {
        if (dir > 0 && s.scroll_offset() != 0) { s.scroll_to_bottom(); ++epoch_; return true; }
        return false;
    }
    s.scroll_to_abs_row(best, 0);
    ++epoch_;
    return true;
}

std::string Term::cwd() const { return uri_to_path(p_->m.working_dir); }

Term::ScrollInfo Term::scroll_info() const {
    const auto& s = p_->m.screen;
    return {s.history_rows() + s.size().rows, s.history_rows() - s.scroll_offset(), s.size().rows};
}

const std::vector<dsl::ScrollbackRun>& Term::minimap(int buckets) const {
    const auto& s = p_->m.screen;
    // content epochs only move on output/resize, not on scrolling: key on the
    // ring's size too, so a glide through history never re-bins
    const std::uint64_t key = (static_cast<std::uint64_t>(content_epoch_) * 1000003u) ^
                              (static_cast<std::uint64_t>(s.total_rows()) << 20) ^ static_cast<std::uint64_t>(buckets);
    if (key == mm_key_) return mm_;
    mm_key_ = key;
    mm_.clear();
    const std::int64_t total = s.total_rows();
    if (total <= 0 || buckets <= 0) return mm_;
    const auto& pal = s.palette();
    const std::uint32_t def = u32(pal.default_fg());
    const float cols = static_cast<float>(std::max(s.size().cols, 1));
    for (int b = 0; b < buckets; ++b) {
        // the densest of up to three lines in the bucket stands for it
        const std::int64_t l0 = total * b / buckets, l1 = std::max(total * (b + 1) / buckets, l0 + 1);
        std::span<const toe::term::Cell> best;
        int best_n = -1;
        for (std::int64_t l = l0; l < l1 && l < l0 + 3; ++l) {
            const auto row = s.abs_row_cells(l);
            int n = 0;
            for (const auto& cell : row) n += (!cell.spacer() && cell.cp != U' ' && cell.cp != 0) ? 1 : 0;
            if (n > best_n) { best_n = n; best = row; }
        }
        // coloured runs of non-blank cells
        int start = -1;
        std::uint32_t col = 0;
        const auto flush = [&](int end) {
            if (start >= 0) mm_.push_back({b, static_cast<float>(start) / cols, static_cast<float>(end) / cols, col});
            start = -1;
        };
        for (int c = 0; c < static_cast<int>(best.size()); ++c) {
            const auto& cell = best[static_cast<std::size_t>(c)];
            const bool ink = !cell.spacer() && cell.cp != U' ' && cell.cp != 0;
            const auto& pen = cell.pen;
            const std::uint32_t fg = std::holds_alternative<toe::term::DefaultColor>(pen.fg) ? def : u32(pal.resolve(pen.fg, true));
            if (!ink) { flush(c); continue; }
            if (start >= 0 && fg != col) flush(c);
            if (start < 0) { start = c; col = fg; }
        }
        flush(static_cast<int>(best.size()));
    }
    return mm_;
}

void Term::scroll_to_line(int first) {
    auto& s = p_->m.screen;
    const int want = std::clamp(s.history_rows() - first, 0, s.history_rows());
    if (want == s.scroll_offset()) return;
    s.scroll(want - s.scroll_offset());
    ++epoch_;
}

Term::Cursor Term::cursor() const {
    const auto& s = p_->m.screen;
    return {s.cursor().col.get(), s.cursor().row.get(), s.cursor_shown() && s.cursor_visible()};
}

// ── hints ──────────────────────────────────────────────────────────────────────────

std::vector<Term::Hint> Term::hints() const {
    using K = Hint::Kind;
    static const std::regex url{R"(((?:https?|ftp|file)://|www\.)[^\s<>"'`]+)"};
    static const std::regex path{R"((?:~|\.{1,2})?/[\w.@+~\-]+(?:/[\w.@+~\-]+)*/?|[\w.@+\-]+(?:/[\w.@+\-]+)+/?)"};
    static const std::regex hash{R"(\b[0-9a-f]{7,40}\b)"};
    static const std::regex ip{R"(\b(?:\d{1,3}\.){3}\d{1,3}(?::\d+)?\b)"};
    const auto& s = p_->m.screen;
    std::vector<Hint> out;
    for (int r = s.size().rows - 1; r >= 0; --r) {
        // the row as text, with each byte's column
        const auto cells = s.row(toe::Row{r});
        std::string line;
        std::vector<int> col_of;
        for (int c = 0; c < static_cast<int>(cells.size()); ++c) {
            const auto& cell = cells[static_cast<std::size_t>(c)];
            if (cell.spacer()) continue;
            const std::size_t before = line.size();
            put_utf8(line, cell.cp == 0 ? U' ' : cell.cp);
            col_of.resize(line.size(), c);
            (void)before;
        }
        col_of.push_back(static_cast<int>(cells.size()));
        std::vector<std::pair<std::size_t, std::size_t>> taken;
        const auto add = [&](const std::regex& re, K kind) {
            for (auto it = std::sregex_iterator(line.begin(), line.end(), re); it != std::sregex_iterator{}; ++it) {
                std::size_t b = static_cast<std::size_t>(it->position()), e = b + static_cast<std::size_t>(it->length());
                while (e > b && std::string_view{".,;:)]}'\""}.find(line[e - 1]) != std::string_view::npos) --e;
                if (e - b < 3) continue;
                bool clash = false;
                for (auto [tb, te] : taken) clash = clash || (b < te && e > tb);
                if (clash) continue;
                std::string text = line.substr(b, e - b);
                if (kind == K::hash) {   // a hash has digits and letters
                    bool dig = false, let = false;
                    for (char ch : text) {
                        dig = dig || (ch >= '0' && ch <= '9');
                        let = let || (ch >= 'a' && ch <= 'f');
                    }
                    if (!dig || !let) continue;
                }
                if (kind == K::path && text.find('/') == std::string::npos) continue;
                taken.push_back({b, e});
                const int c0 = col_of[b], c1 = col_of[e - 1] + 1;
                out.push_back({kind, r, c0, c1 - c0, std::move(text)});
            }
        };
        add(url, K::url);
        add(ip, K::ip);
        add(path, K::path);
        add(hash, K::hash);
    }
    return out;
}

// ── snapshot ───────────────────────────────────────────────────────────────

bool Term::app_cursor_shape() const { return p_->m.screen.cursor_style() != Screen::CursorStyle{}; }

std::int64_t Term::total_rows() const { return p_->m.screen.total_rows(); }
std::int64_t Term::cursor_abs() const {
    const auto& s = p_->m.screen;
    return static_cast<std::int64_t>(s.history_rows()) + s.cursor().row.get();
}

dsl::TermGrid Term::grid_at(std::int64_t first_abs, int rows, int max_cols) const {
    const auto& s = p_->m.screen;
    const auto& pal = s.palette();
    dsl::TermGrid g;
    g.cols = max_cols > 0 ? std::min(max_cols, s.size().cols) : s.size().cols;
    g.rows = std::max(rows, 0);
    g.cursor_visible = false;
    g.lines.resize(static_cast<std::size_t>(g.rows));
    for (int r = 0; r < g.rows; ++r) {
        auto& out = g.lines[static_cast<std::size_t>(r)].cells;
        out.resize(static_cast<std::size_t>(g.cols));
        const auto cells = s.abs_row_cells(first_abs + r);
        for (int c = 0; c < g.cols && c < static_cast<int>(cells.size()); ++c)
            convert_cell(s, pal, cells[static_cast<std::size_t>(c)], out[static_cast<std::size_t>(c)]);
    }
    // prompt marks in the peek too -- their rows are the NORMAL screen's; the
    // alternate screen (vim, htop, cmatrix) has its own rows and no prompts
    if (!s.on_alt_screen()) for (const auto& b : p_->m.commands.blocks()) {
        if (!block_valid(b)) continue;
        const std::int64_t vr = b.prompt_row - first_abs;
        if (b.prompt_row < 0 || vr < 0 || vr >= g.rows) continue;
        g.lines[static_cast<std::size_t>(vr)].mark = b.output_row < 0 ? dsl::TermMark::prompt
                                                     : !b.finished()  ? dsl::TermMark::running
                                                     : b.succeeded()  ? dsl::TermMark::ok
                                                                      : dsl::TermMark::error;
    }
    return g;
}

void Term::set_scroll_offset(int offset) {
    auto& s = p_->m.screen;
    offset = std::clamp(offset, 0, s.history_rows());
    if (offset == s.scroll_offset()) return;
    s.scroll(offset - s.scroll_offset());
    ++epoch_;
}

std::optional<Term::Command> Term::command_at_top() const {
    const auto& s = p_->m.screen;
    if (s.on_alt_screen()) return std::nullopt;
    const std::int64_t top = s.viewport_to_abs(0);
    for (const auto& c : commands()) {
        if (!c.ran || c.prompt_row < 0 || c.prompt_row >= top) continue;
        const std::int64_t end = c.finished && c.end_row >= 0 ? c.end_row : s.total_rows();
        if (top < end) return c;
    }
    return std::nullopt;
}

dsl::TermGrid Term::snapshot(bool above) const {
    using toe::term::Attr;
    using toe::term::has;
    auto& s = p_->m.screen;   // `above` peeks one row up by scrolling and back: state is restored
    const auto& pal = s.palette();
    above = above && s.scroll_offset() < s.history_rows();
    const int shift = above ? 1 : 0;
    dsl::TermGrid g;
    g.cols = s.size().cols;
    g.rows = s.size().rows + shift;
    g.lines.resize(static_cast<std::size_t>(g.rows));
    const bool searching = s.searching();
    const auto emit = [&](int vrow, int r) {
        const auto cells = s.row(toe::Row{vrow});
        auto& out = g.lines[static_cast<std::size_t>(r)].cells;
        out.resize(static_cast<std::size_t>(g.cols));
        const std::int64_t abs = s.viewport_to_abs(vrow);
        // selection and search matches, as runs on this row
        int sel_start = -1, hit_start = -1;
        for (int c = 0; c < g.cols; ++c) {
            dsl::TermCell& d = out[static_cast<std::size_t>(c)];
            const bool in_sel = s.has_selection() && s.is_selected(abs, c);
            if (in_sel && sel_start < 0) sel_start = c;
            if (!in_sel && sel_start >= 0) {
                g.selection.push_back({r, sel_start, c - sel_start});
                sel_start = -1;
            }
            const bool hit = searching && s.is_search_match(abs, c);
            if (hit && hit_start < 0) hit_start = c;
            if (hit && s.is_current_search_match(abs, c)) g.current_match = static_cast<int>(g.matches.size());
            if (!hit && hit_start >= 0) {
                g.matches.push_back({r, hit_start, c - hit_start});
                hit_start = -1;
            }
            if (c >= static_cast<int>(cells.size())) continue;
            convert_cell(s, pal, cells[static_cast<std::size_t>(c)], d);
        }
        if (sel_start >= 0) g.selection.push_back({r, sel_start, g.cols - sel_start});
        if (hit_start >= 0) g.matches.push_back({r, hit_start, g.cols - hit_start});
    };
    if (above) { s.scroll(1); emit(0, 0); s.scroll(-1); }
    for (int r = 0; r < s.size().rows; ++r) emit(r, r + shift);
    g.hover_link = s.hover_link();
    // shell-integration marks on prompt rows (never on the alternate screen:
    // command rows are the normal screen's history, not the full-screen app's grid)
    if (!s.on_alt_screen()) for (const auto& b : p_->m.commands.blocks()) {
        if (!block_valid(b)) continue;
        if (b.prompt_row < 0) continue;
        const std::int64_t vr = b.prompt_row - s.viewport_to_abs(0) + shift;
        if (vr < 0 || vr >= g.rows) continue;
        g.lines[static_cast<std::size_t>(vr)].mark =
            b.output_row < 0 ? dsl::TermMark::prompt
            : !b.finished()  ? dsl::TermMark::running
            : b.succeeded()  ? dsl::TermMark::ok : dsl::TermMark::error;
    }
    const auto cur = s.cursor();
    g.cursor_row = cur.row.get() + shift;
    g.cursor_col = cur.col.get();
    g.cursor_visible = s.cursor_shown() && s.cursor_visible();
    switch (s.cursor_style().shape) {
        case Screen::CursorShape::bar: g.cursor_shape = dsl::TermCursorShape::beam; break;
        case Screen::CursorShape::underline: g.cursor_shape = dsl::TermCursorShape::underscore; break;
        default: g.cursor_shape = dsl::TermCursorShape::block; break;
    }
    return g;
}

}  // namespace hand
