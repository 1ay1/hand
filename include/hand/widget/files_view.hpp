#pragma once
// hand::files_view — the focused terminal's folder, beside the panes.
//
//   ┌  ~/src/hand                 ⟳  ⨯ ┐
//   │ ▾  include                        │   mayag file_tree: virtualised rows,
//   │   ▾  hand                         │   indent guides, devicons (nerd),
//   │       app.hpp                ●   │   the open file marked
//   │       demo.hpp                    │
//   └───────────────────────────────────┘
//
// Not a project sidebar: it shows wherever the FOCUSED TERMINAL is (`cd`
// somewhere and it follows), opens a file into an editor pane next to that
// terminal, and is gone with one key when you do not need it. Rows carry
// their behaviour inline; keys go through mayag's tree_key.

#include "prelude.hpp"
#include "../core/files.hpp"

#include <mayag/app/handlers.hpp>
#include <mayag/widget/file_tree.hpp>
#include <mayag/widget/split.hpp>

namespace hand {

/// The model the files panel edits.
template <class Model>
concept HasFiles = requires(Model& m, const std::string& rel, bool keep) {
    { m.files } -> std::convertible_to<FileTree&>;
    m.open_file(rel, keep);
    m.files_focused = true;
    m.files_width = 1.0f;
};

template <class Model>
    requires HasFiles<Model>
[[nodiscard]] Node files_view(const mayag::Ctx& c, const Model& m) {
    using E = mayag::Edit<Model>;
    const mayag::Theme& t = c.theme;
    const FileTree& ft = m.files;
    const auto rows = ft.rows();

    // header: the root, a refresh and a close button
    const auto small_btn = [&](std::string_view id, std::string_view glyph, std::string_view what, auto msg) {
        const auto nid = node_id(id);
        return (h(text_owned(std::string{glyph}) | font(t.font_size * 0.95f)
                  | fg(c.hovered(nid) ? t.text_primary : t.text_disabled))
                | size(24, 24) | center | radius(t.rounded(4)) | bg(c.hovered(nid) ? t.surface_raised : t.surface.fade(0.0f))
                | transition() | pointer_cursor | role(mayag::a11y::Role::button) | label(what) | id_of(id)
                | on_click([msg] { return msg; })).build();
    };
    const bool nerd = t.icons == mayag::IconSet::nerd;
    auto header = h(text_owned(nerd ? "\uF07C" : "\u25BE") | font(t.font_size) | fg(var::accent),
                    text_owned(ft.display_root()) | font(t.font_size * 0.88f) | semibold | fg(var::text_primary)
                        | ellipsis | shrink() | min_size(0, 0),
                    spacer(),
                    node(small_btn("files.hidden", ft.hidden ? "\u25C9" : "\u25CB", "show hidden",
                                   E{[](Model& mm) { mm.files.hidden = !mm.files.hidden; mm.files.refresh(); }})),
                    node(small_btn("files.refresh", nerd ? "\uF021" : "\u27F3", "refresh", E{[](Model& mm) { mm.files.refresh(); }})),
                    node(small_btn("files.close", "\u2715", "close", E{[](Model& mm) { mm.files_open = false; }})))
                | gap(t.spaced(6)) | align(mayag::Align::center) | pad(0, t.spaced(8), 0, t.spaced(12)) | height(34)
                | width(pct(100)) | with_look(sheet_of(t).strip, t) | inner_shadow(0.0f, t.border, {0.0f, -1.0f});

    // the tree: mayag file_tree; a click on row i toggles a folder or opens
    // a file (single = preview into the editor pane, double = keep)
    Node tree = file_tree(c, "files", rows, ft.selected, ft.scroll, m.files_focused, [](int row, int count) {
        return E{[row, count](Model& mm) {
            mm.files_focused = true;
            const auto flat = mm.files.flat();
            if (row < 0 || row >= static_cast<int>(flat.size())) return;
            mm.files.selected = row;
            const auto& f = flat[static_cast<std::size_t>(row)];
            if (f.row.dir) mm.files.toggle(f.rel);
            else mm.open_file(f.rel, count >= 2);
        }};
    });
    auto wired = node(std::move(tree))
        | on_scroll([](const mayag::Scrolled& s) {
              return E{[d = s.delta](Model& mm) { mm.files.scroll.scroll_by(d); }};
          });

    // the empty state, honestly
    Node body = rows.empty()
        ? (v(text<"empty folder"> | font(t.font_size * 0.9f) | fg(var::text_disabled)) | pad(t.spaced(16)) | center).build()
        : std::move(wired).build();

    return (v(std::move(header), node(std::move(body)) | grow() | width(pct(100)) | min_size(0, 0))
            | width(m.files_width) | height(pct(100)) | min_size(0, 0) | clip
            | bg(var::surface) | role(mayag::a11y::Role::group) | label("files")).build();
}

}  // namespace hand
