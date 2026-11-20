// hand settings — the table, hand.vibe, the scheme catalogue.
// The only TU that compiles the 913-scheme catalogue.

#include <hand/config/config.hpp>
#include <hand/config/schemes_gen.hpp>
#include <vibe.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace hand {

namespace dsl = mayag::dsl;
using Kind = dsl::SettingSpec::Kind;

namespace {

std::vector<std::string> g_fonts;
std::string g_pending_family;   ///< font.family from hand.vibe, before the faces are known

std::size_t default_scheme() {
    static const std::size_t i = [] {
        for (std::size_t k = 0; k < presets::all.size(); ++k)
            if (presets::all[k].name == "Atom One Dark") return k;
        return std::size_t{0};
    }();
    return i;
}
std::size_t scheme_index(const Config& c) {
    return c.scheme < 0 || static_cast<std::size_t>(c.scheme) >= presets::all.size() ? default_scheme()
                                                                                       : static_cast<std::size_t>(c.scheme);
}

std::vector<std::string> scheme_names() {
    std::vector<std::string> v;
    v.reserve(presets::all.size());
    for (const auto& p : presets::all) v.emplace_back(p.name);
    return v;
}

struct Row {
    const char* section;
    const char* key;
    const char* hint;
    Kind        kind;
    float       lo, hi, step;
    int         decimals;
    const char* unit;
    float       scale;
    std::vector<std::string> (*options)();
    float (*get)(const Config&);
    void (*set)(Config&, float);
};

#define GET(expr) [](const Config& c) -> float { return static_cast<float>(expr); }
#define SET(stmt) [](Config& c, float v) { (void)v; stmt; }
using Opts = std::vector<std::string> (*)();
constexpr Opts none = nullptr;

const std::vector<Row>& rows() {
    static const std::vector<Row> r = {
        {"font", "family", "the monospace face (found on this machine)", Kind::cycle, 0, 0, 0, 0, "", 1,
         [] { return g_fonts.empty() ? std::vector<std::string>{"default"} : g_fonts; },
         GET(c.font_family), SET(c.font_family = static_cast<int>(v))},
        {"font", "size", "text size in points (ctrl +/- / 0)", Kind::number, 6, 32, 0.5f, 1, "pt", 1, none,
         GET(c.font_size), SET(c.font_size = v)},
        {"font", "line_height", "row height as a multiple of the size", Kind::number, 1.0f, 2.0f, 0.05f, 2, "\u00D7", 1, none,
         GET(c.line_height), SET(c.line_height = v)},

        {"colors", "scheme", "913 schemes from iTerm2-Color-Schemes", Kind::cycle, 0, 0, 0, 0, "", 1, scheme_names,
         GET(scheme_index(c)), SET(c.scheme = static_cast<std::size_t>(v) == default_scheme() ? -1 : static_cast<int>(v))},
        {"colors", "bold_is_bright", "bold text in brighter colours", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.bold_is_bright), SET(c.bold_is_bright = v > 0.5f)},
        {"colors", "dim_unfocused", "darken the terminal when the window loses focus", Kind::number, 0, 0.6f, 0.05f, 0, "%", 100, none,
         GET(c.dim_unfocused), SET(c.dim_unfocused = v)},

        {"window", "padding", "space around the grid", Kind::number, 0, 40, 1, 0, "px", 1, none,
         GET(c.padding), SET(c.padding = v)},
        {"window", "roundness", "every corner: panels, menus, tabs, controls (0 = square)", Kind::number, 0, 2.5f, 0.05f, 2, "\u00D7", 1, none,
         GET(c.roundness), SET(c.roundness = v)},
        {"window", "opacity", "background opacity (compositor permitting)", Kind::number, 0.5f, 1.0f, 0.02f, 0, "%", 100, none,
         GET(c.opacity), SET(c.opacity = v)},

        {"cursor", "style", "the default shape (apps may change it)", Kind::choice, 0, 2, 1, 0, "", 1,
         [] { return std::vector<std::string>{"block", "beam", "underline"}; },
         GET(static_cast<int>(c.cursor)), SET(c.cursor = static_cast<CursorShape>(std::clamp(static_cast<int>(v), 0, 2)))},
        {"cursor", "blink", "blink while the window is focused", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.cursor_blink), SET(c.cursor_blink = v > 0.5f)},
        {"cursor", "blink_rate", "half a blink period", Kind::number, 200, 1500, 50, 0, "ms", 1, none,
         GET(c.blink_ms), SET(c.blink_ms = v)},
        {"cursor", "glow", "a soft light around the cursor", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.cursor_glow), SET(c.cursor_glow = v > 0.5f)},

        {"motion", "cursor", "how the cursor travels: glide, spring (overshoots), smear (stretches)", Kind::choice, 0, 3, 1, 0, "", 1,
         [] { return std::vector<std::string>{"none", "glide", "spring", "smear"}; },
         GET(static_cast<int>(c.cursor_anim)), SET(c.cursor_anim = static_cast<CursorAnim>(std::clamp(static_cast<int>(v), 0, 3)))},
        {"motion", "effect", "a little show at the cursor as you type (fireworks: on Enter)", Kind::cycle, 0, 0, 0, 0, "", 1,
         [] { return std::vector<std::string>{"none", "ripple", "sonicboom", "wireframe", "pixiedust", "railgun", "torpedo", "fireworks"}; },
         GET(static_cast<int>(c.cursor_fx)), SET(c.cursor_fx = static_cast<CursorFx>(std::clamp(static_cast<int>(v), 0, 7)))},
        {"motion", "trail", "how long the cursor's tail is -- 0 none, 4 a long comet", Kind::number, 0.0f, 4.0f, 0.1f, 1, "\u00D7", 1, none,
         GET(c.cursor_trail), SET(c.cursor_trail = v)},
        {"motion", "blink", "how the cursor blinks", Kind::choice, 0, 2, 1, 0, "", 1,
         [] { return std::vector<std::string>{"hard", "fade", "breathe"}; },
         GET(static_cast<int>(c.blink_style)), SET(c.blink_style = static_cast<BlinkStyle>(std::clamp(static_cast<int>(v), 0, 2)))},
        {"motion", "speed", "how fast every animation runs", Kind::number, 0.25f, 3.0f, 0.05f, 2, "\u00D7", 1, none,
         GET(c.motion_speed), SET(c.motion_speed = v)},
        {"motion", "interface", "panels fade in and out, new lines glide up", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.motion), SET(c.motion = v > 0.5f)},

        {"scrollback", "lines", "history kept above the screen (changing it clears history)", Kind::number, 0, 100000, 1000, 0, "", 1, none,
         GET(c.scrollback), SET(c.scrollback = v)},
        {"scrollback", "wheel_lines", "lines per wheel notch", Kind::number, 1, 15, 1, 0, "", 1, none,
         GET(c.scroll_lines), SET(c.scroll_lines = v)},
        {"scrollback", "smooth", "glide through history instead of jumping a row at a time", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.smooth_scroll), SET(c.smooth_scroll = v > 0.5f)},
        {"scrollback", "live_tail", "rows of live output pinned at the bottom while you read history (0 = off)", Kind::number, 0, 10, 1, 0, "", 1, none,
         GET(c.live_tail), SET(c.live_tail = v)},
        {"scrollback", "peek", "hovering the minimap previews the command and lines there", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.peek), SET(c.peek = v > 0.5f)},
        {"scrollback", "minimap", "a scrollbar that maps prompts, failures and search hits", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.minimap), SET(c.minimap = v > 0.5f)},

        {"shell", "integration", "mark prompts and exit codes in fish, zsh, bash (new windows)", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.shell_integration), SET(c.shell_integration = v > 0.5f)},
        {"shell", "marks", "show each command's result in the gutter", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.shell_marks), SET(c.shell_marks = v > 0.5f)},
        {"shell", "notify_after", "notify when a command this long ends while away (0 = off)", Kind::number, 0, 120, 1, 0, "s", 1, none,
         GET(c.notify_after), SET(c.notify_after = v)},

        {"window", "decorations", "the system title bar and border (off: the tab strip is the title area, with \u2212 \u25A1 \u00D7 on its right; restart to apply)", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.decorations), SET(c.decorations = v > 0.5f)},
        {"keyboard", "option_as_alt", "macOS: \u2325 is Alt/Meta for the shell (\u2325b, \u2325f in readline) instead of typing \u00E9 \u2122 \u2026 (restart to apply)", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.option_as_alt), SET(c.option_as_alt = v > 0.5f)},
        {"tabs", "always", "show the tab strip even with a single tab (ctrl+shift+t opens a tab)", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.tabs_always), SET(c.tabs_always = v > 0.5f)},
        {"tabs", "style", "line: compact mono cells, tmux-like; pill: a lifted card; underline: a sliding bar; flat: a filled block", Kind::choice, 0, 3, 1, 0, "", 1,
         [] { return std::vector<std::string>{"pill", "underline", "flat", "line"}; },
         GET(static_cast<int>(c.tab_style)), SET(c.tab_style = static_cast<TabStyle>(std::clamp(static_cast<int>(v), 0, 3)))},
        {"tabs", "animation", "how the active marker moves: stretch (elastic), slide, instant", Kind::choice, 0, 2, 1, 0, "", 1,
         [] { return std::vector<std::string>{"stretch", "slide", "instant"}; },
         GET(static_cast<int>(c.tab_anim)), SET(c.tab_anim = static_cast<TabAnim>(std::clamp(static_cast<int>(v), 0, 2)))},
        {"tabs", "position", "the strip above or below the terminal", Kind::choice, 0, 1, 1, 0, "", 1,
         [] { return std::vector<std::string>{"top", "bottom"}; },
         GET(c.tab_bottom ? 1 : 0), SET(c.tab_bottom = v > 0.5f)},
        {"tabs", "title", "what a tab is named after (smart: the running command, else the program, else the folder)", Kind::choice, 0, 3, 1, 0, "", 1,
         [] { return std::vector<std::string>{"smart", "folder", "program", "shell"}; },
         GET(static_cast<int>(c.tab_title)), SET(c.tab_title = static_cast<TabTitle>(std::clamp(static_cast<int>(v), 0, 3)))},
        {"tabs", "height", "the strip's height (line: ~26, pill: ~40)", Kind::number, 22, 60, 1, 0, "px", 1, none,
         GET(c.tab_height), SET(c.tab_height = v)},
        {"tabs", "width", "the widest a tab grows", Kind::number, 110, 400, 2, 0, "px", 1, none,
         GET(c.tab_width), SET(c.tab_width = v)},
        {"tabs", "corners", "tab corner radius (-1: follow window.roundness)", Kind::number, -1, 20, 1, 0, "px", 1, none,
         GET(c.tab_radius), SET(c.tab_radius = v)},
        {"tabs", "close_button", "when a tab shows its close button", Kind::choice, 0, 2, 1, 0, "", 1,
         [] { return std::vector<std::string>{"hover", "always", "never"}; },
         GET(static_cast<int>(c.tab_close)), SET(c.tab_close = static_cast<TabClose>(std::clamp(static_cast<int>(v), 0, 2)))},
        {"tabs", "icon", "the program icon, with a ring that spins while a command runs", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.tab_icon), SET(c.tab_icon = v > 0.5f)},
        {"tabs", "subtitle", "a second line: the directory, and the time while running", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.tab_subtitle), SET(c.tab_subtitle = v > 0.5f)},
        {"tabs", "activity", "a sparkline of recent output", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.tab_activity), SET(c.tab_activity = v > 0.5f)},
        {"tabs", "colors", "tint each tab by its directory (off: the accent)", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.tab_colors), SET(c.tab_colors = v > 0.5f)},
        {"tabs", "new_button", "the + button at the end of the strip", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.tab_new_button), SET(c.tab_new_button = v > 0.5f)},

        {"selection", "toolbar", "a chip by the selection: its size, and copy / find / open", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.sel_toolbar), SET(c.sel_toolbar = v > 0.5f)},
        {"selection", "flash", "the selection flashes when it is copied", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.sel_flash), SET(c.sel_flash = v > 0.5f)},

        {"confirm", "paste", "ask before pasting: never, risky (sudo, rm -rf, curl|sh, control chars), any multi-line, or only into programs without bracketed paste", Kind::choice, 0, 3, 1, 0, "", 1,
         [] { return std::vector<std::string>{"never", "risky", "multiline", "unbracketed"}; },
         GET(static_cast<int>(c.paste_confirm)), SET(c.paste_confirm = static_cast<PasteConfirm>(std::clamp(static_cast<int>(v), 0, 3)))},
        {"confirm", "close", "ask before closing a tab or the window: never, while something runs, always", Kind::choice, 0, 2, 1, 0, "", 1,
         [] { return std::vector<std::string>{"never", "running", "always"}; },
         GET(static_cast<int>(c.close_confirm)), SET(c.close_confirm = static_cast<CloseConfirm>(std::clamp(static_cast<int>(v), 0, 2)))},

        {"mouse", "copy_on_select", "selecting text copies it", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.copy_on_select), SET(c.copy_on_select = v > 0.5f)},
        {"mouse", "links_need_ctrl", "links open on ctrl+click (off: any click)", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.links_need_ctrl), SET(c.links_need_ctrl = v > 0.5f)},

        {"bell", "visual", "flash the screen on BEL", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.visual_bell), SET(c.visual_bell = v > 0.5f)},
        {"bell", "shake", "the window shakes on BEL", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.bell_shake), SET(c.bell_shake = v > 0.5f)},

        {"ssh", "multiplex", "one connection per host: a second tab opens instantly, one password prompt (off when ~/.ssh/config sets ControlMaster; not available with Windows' OpenSSH)", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.ssh_multiplex), SET(c.ssh_multiplex = v > 0.5f)},
        {"ssh", "keepalive", "probe the link every 15 s, so a dead connection says so within a minute", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.ssh_keepalive), SET(c.ssh_keepalive = v > 0.5f)},
        {"ssh", "known_hosts", "also list machines from ~/.ssh/known_hosts in the picker (ctrl+shift+s)", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.ssh_known_hosts), SET(c.ssh_known_hosts = v > 0.5f)},
        {"ssh", "prod_guard", "mark production hosts in red on the tab and around the terminal", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.ssh_prod_guard), SET(c.ssh_prod_guard = v > 0.5f)},
        {"ssh", "reconnect", "when an ssh tab drops, keep it open and offer to reconnect", Kind::toggle, 0, 1, 1, 0, "", 1, none,
         GET(c.ssh_reconnect), SET(c.ssh_reconnect = v > 0.5f)},
    };
    return r;
}
#undef GET
#undef SET

const char* icon_of(std::string_view section) {
    const bool nerd = !g_fonts.empty() && g_fonts.front().find("Nerd") != std::string::npos;
    if (!nerd) {   // plain glyphs every mono face has
        if (section == "font") return "Aa";
        if (section == "colors") return "\u25D0";
        if (section == "cursor") return "\u2588";
        if (section == "window") return "\u25A2";
        if (section == "scrollback") return "\u2195";
        if (section == "mouse") return "\u2196";
        if (section == "bell") return "!";
        if (section == "shell") return "$";
        if (section == "motion") return "~";
        if (section == "tabs") return "#";
        if (section == "selection") return "[";
        if (section == "confirm") return "?";
        return "\u2022";
    }
    if (section == "font") return "\uF031";
    if (section == "colors") return "\uF1FC";
    if (section == "cursor") return "\uF246";
    if (section == "window") return "\uF2D0";
    if (section == "scrollback") return "\uF1DA";
    if (section == "mouse") return "\uF245";
    if (section == "bell") return "\uF0F3";
    if (section == "shell") return "\uF489";
    if (section == "motion") return "\uF135";
    if (section == "tabs") return "\uF2D2";
    if (section == "selection") return "\uF246";
    if (section == "confirm") return "\uF071";
    return "\u2022";
}

/// Every scheme as 8 preview chips: paper, ink, red, green, yellow, blue, magenta, cyan.
const std::vector<std::array<std::uint32_t, 8>>& scheme_swatches() {
    static const auto v = [] {
        std::vector<std::array<std::uint32_t, 8>> out;
        out.reserve(presets::all.size());
        for (const auto& p : presets::all)
            out.push_back({p.paper, p.ink, p.ansi[1], p.ansi[2], p.ansi[3], p.ansi[4], p.ansi[5], p.ansi[6]});
        return out;
    }();
    return v;
}

dsl::SettingSpec spec_of(const Row& r, const Config& c, const Config& d) {
    dsl::SettingSpec s;
    s.section = r.section;
    s.section_icon = icon_of(r.section);
    // five pages, like a preferences window: what you change most comes first
    const std::string_view sec = r.section;
    using I = dsl::Icon;
    if (sec == "font" || sec == "colors" || sec == "window") { s.page = "Appearance"; s.page_icon = I::eye; }
    else if (sec == "cursor" || sec == "motion") { s.page = "Cursor & motion"; s.page_icon = I::sparkles; }
    else if (sec == "scrollback" || sec == "shell") { s.page = "Terminal"; s.page_icon = I::terminal; }
    else if (sec == "tabs") { s.page = "Tabs"; s.page_icon = I::layout; }
    else if (sec == "ssh") { s.page = "SSH"; s.page_icon = I::server; }
    else { s.page = "Behaviour"; s.page_icon = I::settings; }
    s.key = r.key;
    // clearer names where a key alone is ambiguous or terse (the file keys stay)
    if (sec == "motion" && std::string_view{r.key} == "blink") s.label = "Blink style";
    if (sec == "motion" && std::string_view{r.key} == "cursor") s.label = "Cursor movement";
    if (sec == "cursor" && std::string_view{r.key} == "style") s.label = "Shape";
    s.hint = r.hint;
    s.kind = r.kind;
    s.value = r.get(c);
    s.lo = r.lo; s.hi = r.hi; s.step = r.step;
    s.decimals = r.decimals;
    s.unit = r.unit;
    s.scale = r.scale;
    if (r.options) s.options = r.options();
    const std::string_view key = r.key;
    if (key == "scheme") s.option_swatches = scheme_swatches();
    if (std::string_view{r.section} == "font" && key == "family")
        for (std::size_t i = 0; i < g_fonts.size(); ++i) s.option_families.push_back(mayag::custom_family(i));
    s.modified = std::fabs(r.get(c) - r.get(d)) > 1e-4f;
    return s;
}

}  // namespace

std::size_t setting_count() { return rows().size(); }

std::vector<dsl::SettingSpec> setting_specs(const Config& c) {
    const Config d{};
    std::vector<dsl::SettingSpec> v;
    v.reserve(rows().size());
    for (const auto& r : rows()) v.push_back(spec_of(r, c, d));
    return v;
}

bool setting_apply(Config& c, const dsl::SettingEdit& e) {
    using Op = dsl::SettingEdit::Op;
    const auto& rs = rows();
    if (e.op == Op::reset_all) {
        Config d{};
        d.shell = c.shell; d.shell_cli = c.shell_cli; d.demo = c.demo; d.url_launcher = c.url_launcher;
        c = d;
        return true;
    }
    if (e.index >= rs.size()) return false;
    const Row& r = rs[e.index];
    const float before = r.get(c);
    float v = before;
    switch (e.op) {
        case Op::set: v = e.value; break;
        case Op::step: v = dsl::setting_stepped(spec_of(r, c, Config{}), e.delta); break;
        case Op::reset: v = r.get(Config{}); break;
        default: break;
    }
    if (r.kind == Kind::number) v = std::clamp(v, r.lo, r.hi);
    r.set(c, v);
    return std::fabs(r.get(c) - before) > 1e-6f;
}

Palette palette_of(const Config& c) {
    const auto& p = presets::all[scheme_index(c)];
    Palette out;
    out.fg = p.ink; out.bg = p.paper; out.cursor = p.cursor; out.selection = p.selection; out.light = p.light;
    for (int i = 0; i < 16; ++i) out.ansi[i] = p.ansi[static_cast<std::size_t>(i)];
    return out;
}

std::size_t scheme_count() { return presets::all.size(); }
void set_font_names(std::vector<std::string> names) { g_fonts = std::move(names); }
const std::vector<std::string>& font_names() { return g_fonts; }

std::string resolve_font_family(Config& c) {
    if (g_pending_family.empty()) return {};
    std::string val = std::move(g_pending_family);
    g_pending_family.clear();
    const auto f = std::find(g_fonts.begin(), g_fonts.end(), val);
    if (f == g_fonts.end()) return "font.family: no option \"" + val + "\"\n";
    c.font_family = static_cast<int>(f - g_fonts.begin());
    return {};
}

// ── hand.vibe ──────────────────────────────────────────────────────────────
//
//   # hand.vibe -- every key is a setting (ctrl+, edits them live)
//   main { url_launcher xdg-open }
//   font { family "JetBrains Mono"  size 13.0  line_height 1.2 }
//   colors { scheme "Atom One Dark"  bold_is_bright true ... }
//
// The file is GENERATED from the settings table: a row's section is the
// object, its key the field, its kind the VIBE type (number -> float or
// integer, toggle -> boolean, choice/cycle -> the option's name).

std::string config_path() {
    if (const char* x = std::getenv("HAND_CONFIG"); x && *x) return x;
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return std::string{x} + "/hand/hand.vibe";
    if (const char* h = std::getenv("HOME"); h && *h) return std::string{h} + "/.config/hand/hand.vibe";
    return "hand.vibe";
}

namespace {

/// Owns a VibeValue tree.
struct Doc {
    VibeValue* v = nullptr;
    explicit Doc(VibeValue* x) : v(x) {}
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
    ~Doc() { if (v) vibe_value_free(v); }
};

/// Integral numbers (whole-step rows) are VIBE integers; the rest floats.
bool integral(const Row& r) { return r.decimals == 0 && r.scale == 1.0f && std::fmod(r.step, 1.0f) == 0.0f && r.step >= 1.0f; }

VibeObject* section_of(VibeObject* root, const char* name) {
    if (VibeValue* x = vibe_object_get(root, name); x && vibe_is_object(x)) return vibe_value_object(x);
    VibeValue* o = vibe_value_new_object();
    vibe_object_set(root, name, o);
    return vibe_value_object(o);
}

}  // namespace

std::string config_load(Config& c, const std::string& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    VibeError err{};
    Doc doc{vibe_parse(text.data(), text.size(), &err)};
    if (!doc.v) {
        std::string msg = "line " + std::to_string(err.line) + ":" + std::to_string(err.column) + ": " +
                          (err.message ? err.message : "syntax error") + " (using defaults)\n";
        vibe_error_free(&err);
        return msg;
    }
    if (!vibe_is_object(doc.v)) return "the file is not a VIBE object\n";
    std::string problems;
    VibeObject* root = vibe_value_object(doc.v);

    // main: launch options
    if (VibeValue* m = vibe_object_get(root, "main"); m && vibe_is_object(m)) {
        VibeObject* mo = vibe_value_object(m);
        if (VibeValue* x = vibe_object_get(mo, "shell"); x && vibe_is_string(x)) c.shell = vibe_value_string(x);
        if (VibeValue* x = vibe_object_get(mo, "url_launcher"); x && vibe_is_string(x)) c.url_launcher = vibe_value_string(x);
    }

    // every table row, by section.key
    const auto& rs = rows();
    for (std::size_t si = 0; si < vibe_object_size(root); ++si) {
        const std::string sec = vibe_object_key_at(root, si);
        VibeValue* sv = vibe_object_value_at(root, si);
        if (sec == "main") continue;
        if (!vibe_is_object(sv)) { problems += sec + ": expected an object { ... }\n"; continue; }
        VibeObject* so = vibe_value_object(sv);
        for (std::size_t ki = 0; ki < vibe_object_size(so); ++ki) {
            const std::string key = vibe_object_key_at(so, ki);
            VibeValue* x = vibe_object_value_at(so, ki);
            const auto it = std::find_if(rs.begin(), rs.end(), [&](const Row& r) { return sec == r.section && key == r.key; });
            if (it == rs.end()) { problems += "unknown setting " + sec + "." + key + "\n"; continue; }
            const Row& r = *it;
            const std::string name = sec + "." + key;
            switch (r.kind) {
                case Kind::toggle:
                    if (!vibe_is_boolean(x)) { problems += name + ": expected true or false\n"; continue; }
                    r.set(c, vibe_value_bool(x) ? 1.0f : 0.0f);
                    break;
                case Kind::choice: case Kind::cycle: {
                    if (!vibe_is_string(x)) { problems += name + ": expected a name\n"; continue; }
                    const auto o = r.options ? r.options() : std::vector<std::string>{};
                    const std::string val = vibe_value_string(x);
                    // font.family names a face, and the faces are discovered
                    // on a worker while the window opens: keep the name and
                    // resolve it once they are known (resolve_font_family).
                    if (sec == "font" && key == "family" && g_fonts.empty()) { g_pending_family = val; continue; }
                    const auto f = std::find(o.begin(), o.end(), val);
                    if (f == o.end()) { problems += name + ": no option \"" + val + "\"\n"; continue; }
                    r.set(c, static_cast<float>(f - o.begin()));
                    break;
                }
                default: {
                    double d = 0.0;
                    if (vibe_is_integer(x)) d = static_cast<double>(vibe_value_int(x));
                    else if (vibe_is_float(x)) d = vibe_value_float(x);
                    else { problems += name + ": expected a number\n"; continue; }
                    const float v = static_cast<float>(d);
                    if (v < r.lo || v > r.hi) problems += name + ": " + std::to_string(v) + " clamped to range\n";
                    r.set(c, std::clamp(v, r.lo, r.hi));
                }
            }
        }
    }
    return problems;
}

bool config_save(const Config& c, const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path{path}.parent_path(), ec);
    Doc doc{vibe_value_new_object()};
    VibeObject* root = vibe_value_object(doc.v);
    VibeObject* main = section_of(root, "main");
    if (!c.shell.empty()) vibe_object_set_string(main, "shell", c.shell.c_str());
    vibe_object_set_string(main, "url_launcher", c.url_launcher.c_str());
    for (const auto& r : rows()) {
        VibeObject* so = section_of(root, r.section);
        const float v = r.get(c);
        switch (r.kind) {
            case Kind::toggle: vibe_object_set_bool(so, r.key, v > 0.5f); break;
            case Kind::choice: case Kind::cycle: {
                const auto o = r.options ? r.options() : std::vector<std::string>{};
                const auto i = static_cast<std::size_t>(std::max(v, 0.0f));
                vibe_object_set_string(so, r.key, i < o.size() ? o[i].c_str() : "");
                break;
            }
            default:
                if (integral(r)) vibe_object_set_int(so, r.key, static_cast<std::int64_t>(std::lround(v)));
                else vibe_object_set_float(so, r.key, std::round(static_cast<double>(v) * 1000.0) / 1000.0);
        }
    }
    char* body = vibe_emit(doc.v);
    if (!body) return false;
    const std::string text = std::string{"# hand.vibe -- every key is a setting; ctrl+, edits them live.\n"
                                         "# Format: VIBE (https://github.com/1ay1/vibe). Unknown keys are reported, not fatal.\n\n"} + body;
    vibe_free(body);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out{tmp, std::ios::binary};
        out << text;
        if (!out) return false;
    }
    fs::rename(tmp, path, ec);   // atomic
    return !ec;
}

}  // namespace hand
