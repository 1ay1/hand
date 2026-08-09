// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Linux font discovery for the host layer — the POSIX counterpart of
// fonts_mac.cpp. Zero dependencies (no fontconfig): we glob the standard
// system + user font directories for .ttf/.otf/.ttc files and match by family
// substring, mirroring toe's own built-in resolver. Used by the settings panel
// (family dropdown + live font swap).

#include "hand/platform/fonts.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace hand {

namespace {

std::string lower(std::string_view s) {
    std::string o(s);
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}

bool is_font_file(const fs::path &p) {
    std::string e = lower(p.extension().string());
    return e == ".ttf" || e == ".otf" || e == ".ttc";
}

// The standard search roots, most-specific first (user overrides system).
std::vector<fs::path> font_roots() {
    std::vector<fs::path> roots;
    if (const char *home = std::getenv("HOME"); home && *home) {
        roots.emplace_back(fs::path(home) / ".local/share/fonts");
        roots.emplace_back(fs::path(home) / ".fonts");
    }
    if (const char *xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        roots.emplace_back(fs::path(xdg) / "fonts");
    }
    roots.emplace_back("/usr/share/fonts");
    roots.emplace_back("/usr/local/share/fonts");
    return roots;
}

// Strip a trailing style descriptor ("-Bold", " Italic", "-Regular", "BoldOblique",
// weight words, ...) and separators from a font file stem, leaving the base
// family name. "JetBrainsMono-BoldItalic" -> "JetBrainsMono".
std::string base_family(std::string stem) {
    static const char *kStyles[] = {
        "bolditalic", "boldoblique", "regular", "italic", "oblique", "bold",
        "light",      "medium",      "semibold", "thin",   "black",   "heavy",
        "extralight", "extrabold",   "condensed", "retina", "book",   "roman",
    };
    for (;;) {
        // Trim trailing separators.
        while (!stem.empty() && (stem.back() == '-' || stem.back() == '_' ||
                                 stem.back() == ' ' || stem.back() == '.')) {
            stem.pop_back();
        }
        std::string low = stem;
        std::transform(low.begin(), low.end(), low.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        bool stripped = false;
        for (const char *st : kStyles) {
            const std::size_t n = std::char_traits<char>::length(st);
            if (low.size() > n && low.compare(low.size() - n, n, st) == 0) {
                stem.erase(stem.size() - n);
                stripped = true;
                break;
            }
        }
        if (!stripped) break;
    }
    return stem.empty() ? std::string{"monospace"} : stem;
}

// Well-known programming/monospace faces, preferred when the family is a
// generic alias ("monospace") or unspecified.
bool preferred_mono(const std::string &lname) {
    static const char *kPrefs[] = {
        "jetbrainsmono", "jetbrains mono", "firacode", "fira code", "cascadia",
        "hack", "sourcecodepro", "source code pro", "dejavusansmono",
        "dejavu sans mono", "liberationmono", "liberation mono", "notosansmono",
        "noto sans mono", "ubuntumono", "ubuntu mono", "inconsolata", "menlo",
        "consolas", "monospace", "mono",
    };
    for (const char *p : kPrefs) {
        if (lname.find(p) != std::string::npos) return true;
    }
    return false;
}

} // namespace

std::string resolve_font_file(std::string_view family) {
    const std::string needle = lower(family);
    const bool generic = needle.empty() || needle == "monospace" || needle == "mono";

    std::string best;      // exact/substring family match
    std::string best_mono; // fallback: any preferred monospace face

    std::error_code ec;
    for (const auto &root : font_roots()) {
        if (!fs::exists(root, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            const fs::path &p = it->path();
            if (!it->is_regular_file(ec) || !is_font_file(p)) continue;

            const std::string stem = lower(p.stem().string());
            // Skip bold/italic variants for the default face — the atlas
            // synthesizes those from the regular.
            const bool variant = stem.find("bold") != std::string::npos ||
                                 stem.find("italic") != std::string::npos ||
                                 stem.find("oblique") != std::string::npos;

            if (!generic && stem.find(needle) != std::string::npos) {
                if (!variant) return p.string();
                if (best.empty()) best = p.string();
            }
            if (best_mono.empty() && !variant && preferred_mono(stem)) {
                best_mono = p.string();
            }
        }
    }
    if (!best.empty()) return best;
    return best_mono; // "" if nothing usable found
}

std::vector<std::string> list_monospace_families() {
    std::vector<std::string> out;
    out.push_back("monospace"); // the system-default alias, always first

    std::vector<std::string> found;
    std::error_code ec;
    for (const auto &root : font_roots()) {
        if (!fs::exists(root, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            const fs::path &p = it->path();
            if (!it->is_regular_file(ec) || !is_font_file(p)) continue;
            const std::string stem = p.stem().string();
            const std::string lstem = lower(stem);
            if (lstem.find("mono") == std::string::npos && !preferred_mono(lstem)) continue;
            // Present the base family (strip -Bold/-Italic/-Regular/etc. and any
            // trailing separators) so the dropdown lists one row per family;
            // the atlas synthesizes bold/italic from the regular face.
            found.push_back(base_family(stem));
        }
    }
    std::sort(found.begin(), found.end(), [](const std::string &a, const std::string &b) {
        return lower(a) < lower(b);
    });
    found.erase(std::unique(found.begin(), found.end()), found.end());
    for (auto &f : found) out.push_back(std::move(f));
    return out;
}

} // namespace hand
