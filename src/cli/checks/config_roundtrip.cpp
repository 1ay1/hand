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
