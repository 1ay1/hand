#pragma once
// hand::Config — everything configurable, and the one table describing it.
//
// Every setting is a row in src/settings.cpp: section, key, hint, kind,
// range, and a get/set pair over Config. That single table generates the
// settings panel (mayag settings_panel), reads and writes ~/.config/hand/
// hand.vibe, and validates input. Adding a setting is adding a row.

#include <mayag/widget/settings_panel.hpp>
#include <mayag/widget/tab_strip.hpp>
#include <mayag/widget/terminal/cursor_fx.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace hand {

/// The 16 ANSI colours plus default ink/paper/cursor/selection, as 0xRRGGBB.
struct Palette {
    std::uint32_t fg = 0xABB2BF, bg = 0x282C34, cursor = 0x74ADE8, selection = 0x3E4451;
    std::uint32_t ansi[16] = {0x282C34, 0xE06C75, 0x98C379, 0xE5C07B, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xABB2BF,
                              0x5C6370, 0xE06C75, 0x98C379, 0xE5C07B, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xFFFFFF};
    bool light = false;
};

// What the widgets already name, hand uses as is: one type per idea.
using CursorAnim = mayag::dsl::CursorMotion;
using CursorFx   = mayag::dsl::CursorFx;
using TabStyle   = mayag::dsl::TabStripStyle;
using TabAnim    = mayag::dsl::TabMotion;
using TabClose   = mayag::dsl::TabClose;
namespace motion = mayag::dsl::cursor_fx;   ///< cursor physics and particle effects

enum class CursorShape : std::uint8_t { block, beam, underline };
enum class BlinkStyle : std::uint8_t { hard, fade, breathe };
enum class TabTitle : std::uint8_t { smart, folder, program, shell };   ///< what a tab is named after
/// When a paste asks first.
enum class PasteConfirm : std::uint8_t { never, risky, multiline, unbracketed };
/// When closing a tab / the window asks first.
enum class CloseConfirm : std::uint8_t { never, running, always };

struct Config {
    // font
    int   font_family = 0;          ///< index into font_names()
    float font_size = 13.0f;
    float line_height = 1.2f;
    // colors
    int   scheme = -1;              ///< index into the catalogue; -1 = default (One Dark)
    bool  bold_is_bright = true;
    float dim_unfocused = 0.0f;
    // cursor
    CursorShape cursor = CursorShape::block;
    bool  cursor_blink = true;
    float blink_ms = 500.0f;
    // motion
    CursorAnim cursor_anim = CursorAnim::glide;   ///< how the cursor travels between cells
    CursorFx   cursor_fx = CursorFx::none;        ///< a little show at the cursor as you type
    BlinkStyle blink_style = BlinkStyle::fade;
    float      motion_speed = 1.0f;               ///< scales every animation's clock
    float      cursor_trail = 1.0f;               ///< tail length (× default); 0 = no tail
    bool  cursor_glow = false;
    bool  motion = true;            ///< fades and slides: cursor blink, panels, new lines       ///< a soft light under the cursor
    // window
    float padding = 8.0f;
    float opacity = 1.0f;
    float roundness = 1.0f;         ///< every corner radius (panels, menus, tabs, controls); 0 = square
    // scrollback
    float scrollback = 10000.0f;
    float scroll_lines = 3.0f;
    bool  minimap = true;
    bool  smooth_scroll = true;     ///< glide through history, pixel by pixel
    float live_tail = 4.0f;         ///< rows of live output kept at the bottom while scrolled back (0 = off)
    bool  peek = true;              ///< hovering the minimap previews the command and lines there           ///< scrollbar map: prompts, failures, search hits
    // shell
    bool  shell_integration = true; ///< inject OSC 133/7 into fish/zsh/bash
    bool  shell_marks = true;       ///< prompt marks in the gutter
    float notify_after = 10.0f;     ///< seconds; notify when a longer command ends unfocused (0 = off)
    // behaviour
    bool  tabs_always = true;       ///< show the tab strip with one tab too (else: from the second)
    bool  decorations = false;      ///< the system title bar; off: the tab strip is the title area (restart to apply)
    bool  option_as_alt = false;    ///< macOS: ⌥ is Alt/Meta for the shell, not a character key (restart to apply)
    /// The strip shows with one tab too. Off on an undecorated window, the
    /// window's buttons float small in the top-right corner instead.
    [[nodiscard]] bool strip_always() const noexcept { return tabs_always; }
    TabStyle tab_style = TabStyle::line;
    TabTitle tab_title = TabTitle::smart;
    TabClose tab_close = TabClose::hover;
    TabAnim  tab_anim = TabAnim::slide;    ///< how the active marker moves between tabs
    float tab_height = 26.0f;
    float tab_width = 220.0f;       ///< the widest a tab grows
    float tab_radius = -1.0f;       ///< tab corners, px; -1 = follow window.roundness
    bool  tab_bottom = false;       ///< the strip under the terminal instead of above
    bool  tab_icon = false;         ///< the process icon with its status ring
    bool  tab_subtitle = false;     ///< the second line: directory / elapsed
    bool  tab_activity = false;     ///< the output sparkline
    bool  tab_colors = false;       ///< each tab tinted by its directory
    bool  tab_new_button = true;
    bool  copy_on_select = false;
    bool  sel_toolbar = true;       ///< a chip by the selection: size, copy, find, open
    bool  sel_flash = true;         ///< the selection flashes when it is copied
    PasteConfirm paste_confirm = PasteConfirm::multiline;
    CloseConfirm close_confirm = CloseConfirm::running;
    bool  visual_bell = true;
    bool  bell_shake = false;       ///< the window shakes on BEL
    bool  links_need_ctrl = true;   ///< ctrl+click opens links (plain click selects)
    // ssh (ctrl+shift+s): the host picker and connections
    bool  ssh_multiplex = true;     ///< share one connection per host (ControlMaster) unless ~/.ssh/config chooses
    bool  ssh_keepalive = true;     ///< ServerAliveInterval 15: notice a dead link in a minute, not hours
    bool  ssh_known_hosts = true;   ///< list machines from ~/.ssh/known_hosts too
    bool  ssh_prod_guard = true;    ///< a prod host (by name or hosts.vibe) is marked in red on its tab and frame
    bool  ssh_reconnect = true;     ///< a dropped ssh tab offers to reconnect instead of closing
    // files: the pane beside the terminal (ctrl+shift+e)
    bool  files_left = true;        ///< dock on the left (off: the right)
    float files_width = 300.0f;     ///< px; dragging its edge sets this
    bool  files_open = false;       ///< open when hand starts
    bool  files_follow = true;      ///< move with the shell's cd
    bool  files_hidden = false;     ///< show dotfiles
    bool  files_dirs_first = true;
    int   files_sort = 0;           ///< name / modified / size / type
    bool  files_git = true;         ///< colour files and folders by git status (local folders in a repo)
    bool  files_size_bars = true;   ///< a bar under each file: its size against the folder's largest
    bool  files_age = true;         ///< how long ago each file changed ("4m", "2d")
    bool  files_type_colors = true; ///< icons tinted by kind: code, config, docs, images, archives
    bool  files_watch = true;       ///< re-read the folder while open; new and changed files glow
    bool  files_preview = true;     ///< the selected file's first lines under the list
    float files_preview_lines = 12.0f;

    // not settings: launch options
    std::string shell;          ///< [main] shell (from the file)
    std::string shell_cli;      ///< -e CMD: this run only, never saved
    std::string url_launcher = "xdg-open";   ///< [main] url_launcher=
    bool        demo = false;
};

// ── the table ──────────────────────────────────────────────────────────────
[[nodiscard]] std::size_t setting_count();
[[nodiscard]] std::vector<mayag::dsl::SettingSpec> setting_specs(const Config& c);
/// Apply a panel edit. Returns true when the config changed.
bool setting_apply(Config& c, const mayag::dsl::SettingEdit& e);

// ── derived values ─────────────────────────────────────────────────────────
[[nodiscard]] Palette palette_of(const Config& c);
[[nodiscard]] std::size_t scheme_count();
/// Monospace faces found at startup (main sets them; family index → name).
void set_font_names(std::vector<std::string> names);
[[nodiscard]] const std::vector<std::string>& font_names();
/// Apply a font.family that hand.vibe named before the faces were known
/// (they are found on a worker during startup). Returns a problem, or "".
std::string resolve_font_family(Config& c);

// ── the file ───────────────────────────────────────────────────────────────
[[nodiscard]] std::string config_path();   ///< $HAND_CONFIG or ~/.config/hand/hand.vibe
/// Read the file over `c`. Unknown keys/bad values are skipped; returns a
/// short message for each, empty when clean.
std::string config_load(Config& c, const std::string& path);
bool config_save(const Config& c, const std::string& path);

}  // namespace hand
