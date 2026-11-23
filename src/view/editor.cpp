// hand::view — a file tab: the editor (mayag text_editor) in the
// terminal's place, in the terminal's colours and face.

#include "view.hpp"

#include <mayag/widget/text_editor.hpp>

namespace hand::view {

using namespace mayag;
using namespace mayag::dsl;
using namespace hand::detail;

Node editor(const Ctx& c, const Model& m) {
    const Session& se = m.tab();
    const FileDoc& d = *se.file;
    const Palette& p = m.palette;
    std::array<std::uint32_t, 16> ansi{};
    for (std::size_t i = 0; i < 16; ++i) ansi[i] = p.ansi[i];
    const CodeTheme th = CodeTheme::from_palette(c.theme, ansi, p.fg, p.bg, p.cursor, p.selection);

    // not open: why, on the file's own paper
    if (d.loading || !d.error.empty()) {
        const Color<Srgb> paper = rgb(p.bg), ink = rgb(p.fg);
        const std::string name = d.path.substr(d.path.find_last_of("/\\") + 1);
        Node msg = d.loading
            ? (text_owned("Opening " + name + (d.host ? " on " + d.host->alias : std::string{}) + "\u2026") | type(Tier::body) | fg(mix(ink, paper, 0.4f))).build()
            : (v(text_owned("Can\u2019t open " + name) | type(Tier::large) | semibold | fg(ink),
                 text_owned(d.error) | type(Tier::body) | fg(mix(ink, paper, 0.35f)),
                 text_owned(d.host ? d.host->alias + ":" + d.path : short_path(m, d.path)) | type(Tier::small) | fg(mix(ink, paper, 0.55f)))
               | gap(c.theme.spaced(6)) | align(Align::center)).build();
        return (h(node(std::move(msg))) | center | size(m.size.x, m.size.y) | bg(paper)).build();
    }

    TextEditorProps props;
    props.path = d.path;
    props.keymap = m.env.keymap;
    props.shown_path = d.path.empty() ? d.path : (d.host ? d.path : short_path(m, d.path));
    if (d.host) {
        props.host = d.host->alias;
        props.host_danger = m.cfg.ssh_prod_guard && remote_env(m, *d.host) == HostEnv::prod;
    }
    props.font_size = m.font_size;   // the terminal's size and face: code reads the same in both
    props.line_height = std::max(m.cfg.line_height, 1.3f);
    props.family = family_of(m.cfg);
    props.theme = th;
    props.focused = m.focused && !(m.pane.open && m.pane.ui.focused) && !m.settings.open && !m.dialog.ask;
    props.cursor_on = m.cursor_on;
    props.motion = m.cfg.motion;
    props.saving = d.saving.has_value();
    props.error = d.save_error;
    return (node(text_editor(c, "doc", d.st, props, [](TextEditorOp op) -> App::Msg { return App::EditorOp{std::move(op)}; }))
            | size(m.size.x, m.size.y)).build();
}

}  // namespace hand::view
