// hand::App — the dialog: asking before a paste, a tab close or quitting, and
// acting on the answer.
//
//   RequestQuit ──(busy, or close.confirm = always)──▶ Ask{quit}
//   Answer ──▶ Dismiss            nothing happens; the question goes away
//          └─▶ Choose{action}     the action the dialog offered at that index
//   RememberToggle ──▶ the "don't ask again" box

#include "detail.hpp"
#include "fx/fx.hpp"

#include <utility>

namespace hand {

using namespace hand::detail;

namespace {

/// The paste dialog's actions, in the order the view lists them.
enum class PasteAction : std::size_t { cancel = 0, one_line = 1, paste = 2 };

/// Saving quits the app: hand.vibe first (a demo never writes it).
App::Cmd save_and_quit(const Model& m) {
    if (m.cfg.demo) return App::Cmd::quit();
    return App::Cmd::batch(App::Cmd::run(fx::save_config, m.cfg, m.env.config_path), App::Cmd::quit());   // in order: saved, then gone
}

/// "Don't ask again": the next question of this kind is not asked.
App::Cmd remember(Model& m, Ask::Kind kind) {
    if (kind == Ask::Kind::paste) m.cfg.paste_confirm = PasteConfirm::risky;
    else m.cfg.close_confirm = CloseConfirm::never;
    if (m.cfg.demo) return {};
    return App::Cmd::run(fx::save_config, m.cfg, m.env.config_path);
}

/// A paste as one line: newlines become spaces, nothing runs until enter.
std::string one_line(const std::string& text) {
    std::string one;
    for (const auto& l : split_lines(text)) {
        if (!one.empty() && !l.empty()) one += ' ';
        one += l;
    }
    return one;
}

}  // namespace

App::Cmd App::update(Model& m, RequestQuit) {
    if (m.cfg.close_confirm != CloseConfirm::never && !m.dialog.ask) {
        bool busy = false;
        for (std::size_t i = 0; i < m.tab_count(); ++i) busy = busy || !busy_of(m, m.at(i), nullptr).empty();
        if (busy || m.cfg.close_confirm == CloseConfirm::always) {
            m.dialog.ask = Ask{.kind = Ask::Kind::quit, .focus = 1};
            return {};
        }
    }
    return save_and_quit(m);
}

App::Cmd App::update(Model& m, RememberToggle) {
    if (m.dialog.ask) m.dialog.ask->remember = !m.dialog.ask->remember;
    return {};
}

App::Cmd App::update(Model& m, DialogFocus f) {
    if (!m.dialog.ask) return {};
    const std::size_t n = m.dialog.ask->kind == Ask::Kind::paste ? 3 : 2;   // the actions it offers
    m.dialog.ask->focus = wrap(m.dialog.ask->focus, f.step, n);
    return {};
}

App::Cmd App::update(Model& m, Answer a) {
    if (!m.dialog.ask) return {};
    const Ask asked = *std::exchange(m.dialog.ask, std::nullopt);
    m.dialog.was = asked;   // what fades out
    const auto* choice = std::get_if<Answer::Choose>(&a.pick);
    const std::size_t action = choice ? choice->action : 0;   // dismissing is cancelling
    const bool yes = action != 0;
    // "don't ask again": saved before whatever the answer does (a quit saves anyway)
    Cmd saved = yes && asked.remember && asked.kind != Ask::Kind::quit ? remember(m, asked.kind) : Cmd{};
    if (yes && asked.remember && asked.kind == Ask::Kind::quit) (void)remember(m, asked.kind);
    switch (asked.kind) {
        case Ask::Kind::paste: {
            std::string text = asked.paste;
            if (!yes) return {};
            if (action == std::to_underlying(PasteAction::one_line)) text = one_line(text);
            return Cmd::batch(std::move(saved), update(m, Pasted{std::move(text), Confirm::given}));
        }
        case Ask::Kind::close_tab:
            return yes ? Cmd::batch(std::move(saved), update(m, CloseTab{asked.tab, Confirm::given})) : Cmd{};
        case Ask::Kind::quit:
            return yes ? save_and_quit(m) : Cmd{};
    }
    return {};
}

}  // namespace hand
