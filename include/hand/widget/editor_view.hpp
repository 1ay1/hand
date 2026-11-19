#pragma once
// hand::editor_view — a file, open in a pane, next to its terminal.
//
//   ┌  theme.hpp  ~/src/hand/include/hand/core   ● ─────── ln 12, col 31  ⨯ ┐
//   │  1  #pragma once                                                         │  mayag code_view:
//   │  2                                                                       │  gutter, syntax, folds,
//   │  3  // the whole configuration, as one value                             │  multi-cursor, brackets,
//   └──────────────────────────────────────────────────────────────────────────┘  occurrences, find
//
// hand is a terminal that HAS an editor: this is a pane like any other in
// the split tree (it zooms, closes and resizes the same way), so editing a
// file never takes you out of the terminal you opened it from. The editing
// engine is mayag's (editor/buffer.hpp + widget/code_view.hpp); this widget
// is its header strip, the theme mapping and the pointer wiring -- carried
// inline as handlers, so the app has no editor ids to decode.

#include "prelude.hpp"
#include "../core/theme.hpp"

#include <mayag/app/handlers.hpp>
#include <mayag/widget/code_view.hpp>
#include <mayag/widget/file_icon.hpp>

#include <cstring>
#include <filesystem>
#include <string>

namespace hand {

/// One open file.
struct Doc {
    std::filesystem::path       path;
    mayag::editor::EditorState  ed;
    std::string                 error;   ///< why it could not be read/saved
};

/// The editor surface's colours: mayag's dark code theme, its chrome pulled
/// onto the scheme's paper so an editor pane and a terminal pane are the
/// same material, and tokens from the scheme's own ANSI palette -- a file
/// in `dusk` is coloured like `dusk`'s terminal is.
[[nodiscard]] inline CodeTheme code_theme_of(const Appearance& a, const mayag::Theme& t) {
    using mayag::editor::Tok;
    const Scheme s = effective_scheme(a);
    CodeTheme c = CodeTheme::dark(t);
    const auto set = [&](Tok k, std::uint32_t v) { c.tok[static_cast<std::size_t>(k)] = hex(v); };
    set(Tok::text, s.ink);       set(Tok::keyword, s.ansi[5]);  set(Tok::type, s.ansi[11]);
    set(Tok::function, s.ansi[4]); set(Tok::number, s.ansi[9]); set(Tok::string, s.ansi[2]);
    set(Tok::comment, s.ansi[8]); set(Tok::op, s.ansi[6]);     set(Tok::punct, s.ansi[7]);
    set(Tok::constant, s.ansi[3]); set(Tok::macro, s.ansi[13]); set(Tok::property, s.ansi[14]);
    set(Tok::special, s.ansi[1]); set(Tok::heading, s.ansi[12]);
    const auto paper = hex(s.paper), ink = hex(s.ink);
    c.bg = paper;
    c.gutter_bg = paper;
    c.line_no = mayag::mix(paper, ink, 0.28f);
    c.line_no_active = ink;
    c.current_line = mayag::mix(paper, ink, 0.035f);
    c.selection = mayag::mix(paper, t.accent, 0.30f);
    c.selection_inactive = mayag::mix(paper, ink, 0.12f);
    c.cursor = t.accent;
    c.bracket = mayag::mix(paper, ink, 0.18f);
    c.occurrence = mayag::mix(paper, ink, 0.10f);
    c.indent_guide = mayag::mix(paper, ink, 0.06f);
    c.indent_guide_active = mayag::mix(paper, ink, 0.16f);
    c.ruler = mayag::mix(paper, ink, 0.05f);
    c.whitespace = mayag::mix(paper, ink, 0.14f);
    c.folded_bg = mayag::mix(paper, t.accent, 0.12f);
    return c;
}

/// The model the editor pane edits through: find a Doc by pane.
template <class Model>
concept HasDocs = requires(Model& m, PaneId p) {
    { m.doc(p) } -> std::same_as<Doc*>;
};

struct EditorViewProps {
    PaneId pane{};
    bool   focused = true;
    bool   cursor_on = true;
};

template <class Model>
    requires HasDocs<Model>
[[nodiscard]] Node editor_view(const mayag::Ctx& c, const Doc& d, const Appearance& a, const EditorViewProps& p) {
    using mayag::editor::EditorState;
    const mayag::Theme& t = c.theme;
    const std::string id = "edit." + std::to_string(p.pane.v);
    const PaneId pane = p.pane;

    // header: icon, name, folder, dirty dot, caret position, language
    const std::string name = d.path.filename().string();
    std::string folder = d.path.parent_path().string();
    if (const char* home = std::getenv("HOME"); home && folder.starts_with(home)) folder = "~" + folder.substr(std::strlen(home));
    const auto cur = d.ed.cursor();
    const std::string pos = "ln " + std::to_string(cur.line + 1) + ", col " + std::to_string(cur.col + 1)
                          + (d.ed.sels.size() > 1 ? "  \u00B7 " + std::to_string(d.ed.sels.size()) + " cursors" : "");
    auto header = h(node(file_icon(t, name, t.font_size * 0.95f).build()),
                    text_owned(name) | font(t.font_size * 0.92f) | semibold
                        | fg(p.focused ? t.text_primary : t.text_secondary),
                    text_owned(folder) | font(t.font_size * 0.82f) | fg(var::text_disabled) | ellipsis | shrink() | min_size(0, 0),
                    when(d.ed.dirty(), node((box() | size(6, 6) | radius(3) | bg(t.warning)).build())),
                    spacer(),
                    when(!d.error.empty(), node((text_owned(d.error) | font(t.font_size * 0.8f) | fg(var::danger)).build())),
                    text_owned(pos) | font(t.font_size * 0.8f) | monospace | fg(var::text_disabled),
                    text_owned(std::string{d.ed.language ? d.ed.language->name : std::string_view{"text"}}) | font(t.font_size * 0.8f) | fg(var::text_disabled))
                | gap(t.spaced(8)) | align(mayag::Align::center) | pad(0, t.spaced(10)) | height(26) | width(pct(100))
                | bg(p.focused ? t.surface : t.surface.fade(0.5f)) | border(t.stroke(1), t.border.fade(0.6f));

    CodeViewOptions o{.font_size = a.font_size.get(), .line_height = std::max(1.3f, a.line_height.get()),
                      .focused = p.focused, .line_numbers = a.line_numbers, .relative_numbers = a.relative_numbers,
                      .indent_guides = a.indent_guides, .current_line = a.current_line, .cursor_on = p.cursor_on,
                      .cursor_style = a.cursor == CursorStyle::block ? 1 : a.cursor == CursorStyle::underline ? 2 : 0};
    if (a.ruler.get() > 0) o.rulers = {a.ruler.get()};
    o.family = mayag::custom_family(a.mono.v);
    o.ligatures = a.ligatures;
    Node body = code_view(c, id, d.ed, code_theme_of(a, t), o);

    // Pointer: press places / adds (alt) / extends (shift) carets, drag
    // selects, double/triple click word/line, wheel scrolls. Inline, so the
    // gesture always reaches THIS pane's document.
    using E = mayag::Edit<Model>;
    auto surface = node(std::move(body))
        | on_press([pane](const mayag::Pressed& e) {
              // count 1 caret / alt adds a caret / shift extends; 2 word; 3 line --
              // decided AT the press, so the drag that follows goes by the
              // same unit (mayag EditorState::press_at / drag_to).
              return E{[pane, local = e.local, n = e.count, mods = e.mods](Model& m) {
                  m.focus(pane);
                  if (Doc* doc = m.doc(pane))
                      doc->ed.press_at(code_view_hit(doc->ed, local), n, mods.shift, mods.alt);
              }};
          })
        | on_drag([pane](const mayag::Dragged& e) {
              return E{[pane, local = e.local](Model& m) {
                  if (Doc* doc = m.doc(pane)) doc->ed.drag_to(code_view_hit(doc->ed, local));
              }};
          })
        | on_click([pane, lines = d.ed.line_count()](const mayag::Clicked& e) -> std::optional<E> {
              // A fold chevron in the gutter (code_view names them
              // part_fold_base + line): the click bubbled here from it.
              const std::string base = "edit." + std::to_string(pane.v);
              for (int l = 0; l < lines; ++l)
                  if (e.node == part_id(base, part_fold_base + static_cast<std::size_t>(l)))
                      return E{[pane, l](Model& m) { if (Doc* doc = m.doc(pane)) doc->ed.toggle_fold(l); }};
              return std::nullopt;
          })
        | on_scroll([pane](const mayag::Scrolled& e) {
              return E{[pane, d = e.delta](Model& m) { if (Doc* doc = m.doc(pane)) doc->ed.scroll.scroll_by(d); }};
          });

    return (v(std::move(header), std::move(surface) | grow() | width(pct(100)) | min_size(0, 0))
            | grow() | fill_both() | min_size(0, 0) | clip).build();
}

}  // namespace hand
