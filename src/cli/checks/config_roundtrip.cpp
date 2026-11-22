// hand::cli::checks — hand.vibe: every row survives save and load.

#include "suites.hpp"

#include <hand/core/platform.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace hand::cli::checks {

using namespace mayag;

void config_roundtrip([[maybe_unused]] Runtime& rt, [[maybe_unused]] Vec2 size, Check& ok) {
    // ── hand.vibe: every row survives save + load ──────────────────────────
    using SE = dsl::SettingEdit;
    hand::Config a;
    a.url_launcher = "firefox";
    const auto n = hand::setting_specs(a).size();
    for (std::size_t i = 0; i < n; ++i) (void)hand::setting_apply(a, {SE::Op::step, i, 0.0f, 2});
    const std::string path = (std::filesystem::temp_directory_path() / ("hand-test-" + std::to_string(hand::self_pid()) + ".vibe")).string();
    const bool saved = hand::config_save(a, path);
    hand::Config b;
    const std::string problems = hand::config_load(b, path);
    ::unlink(path.c_str());
    const auto sa = hand::setting_specs(a), sb = hand::setting_specs(b);
    std::string diff;
    for (std::size_t i = 0; i < n; ++i)
        if (std::fabs(sa[i].value - sb[i].value) > 1e-3f) diff += " " + sa[i].section + "." + sa[i].key;
    if (!problems.empty()) std::printf("    load problems: %s", problems.c_str());
    if (!diff.empty()) std::printf("    differs after reload:%s\n", diff.c_str());
    ok(saved && problems.empty() && diff.empty() && b.url_launcher == "firefox",
       "hand.vibe round-trips every setting");
    {   // font.ui: saved by NAME, matched to this machine's faces at start, and
        // the theme's face is that face's role (every label inherits it)
        hand::Config u;
        u.ui_fonts = {"Inter", "Geist", "Noto Sans"};
        u.ui_font = 2;
        const bool s2 = hand::config_save(u, path);
        hand::Config v;
        (void)hand::config_load(v, path);
        ::unlink(path.c_str());
        v.ui_fonts = {"Noto Sans", "Inter"};   // another machine, another order
        const std::string pr = hand::resolve_font_family(v);
        ok(s2 && pr.empty() && v.ui_font == 0 && hand::ui_family_of(v) == hand::ui_role(0),
           "font.ui is kept by name and found on another machine's list");
        hand::Config w;
        ok(hand::ui_family_of(w) == mayag::FontFamily::ui, "no UI faces found: the chrome uses the stack's own UI chain");
        const auto& m = rt.model();
        ok(hand::App::theme(m).font_family == hand::ui_family_of(m.cfg) && hand::App::theme(m).font_size == m.cfg.ui_font_size,
           "the theme carries font.ui and font.ui_size to every widget");
    }
    // a broken or partial file is reported, never fatal
    const std::string bad = path + ".bad";
    { std::FILE* f = std::fopen(bad.c_str(), "w"); std::fputs("font { size 15.5  nope 3 }\ncolors { scheme \"Nord\" bold_is_bright yes }\n", f); std::fclose(f); }
    hand::Config c;
    const std::string p2 = hand::config_load(c, bad);
    ::unlink(bad.c_str());
    ok(c.font_size == 15.5f && hand::palette_of(c).bg == 0x2E3440 && p2.find("font.nope") != std::string::npos &&
       p2.find("bold_is_bright") != std::string::npos,
       "good keys apply; unknown keys and wrong types are reported");
    { std::FILE* f = std::fopen(bad.c_str(), "w"); std::fputs("font { size ", f); std::fclose(f); }
    hand::Config e;
    const std::string p3 = hand::config_load(e, bad);
    ::unlink(bad.c_str());
    ok(!p3.empty() && e.font_size == hand::Config{}.font_size, "a syntax error keeps the defaults");
}

}  // namespace hand::cli::checks
