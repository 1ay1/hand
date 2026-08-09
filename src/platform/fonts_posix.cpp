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

    // Rank a candidate's suitability as the REGULAR face (higher = better):
    // an exact "-regular"/no-weight stem beats a "-medium"/"-book", which beats
    // other weights (Light/Thin/ExtraLight) — so a family whose files are all
    // weight-suffixed doesn't accidentally resolve to ExtraLight.
    auto regular_rank = [](const std::string &lstem) -> int {
        if (lstem.find("italic") != std::string::npos ||
            lstem.find("oblique") != std::string::npos || lstem.find("bold") != std::string::npos)
            return -1; // not a regular face at all
        if (lstem.find("regular") != std::string::npos) return 100;
        if (lstem.find("medium") != std::string::npos || lstem.find("book") != std::string::npos ||
            lstem.find("roman") != std::string::npos)
            return 80;
        // No explicit weight word — e.g. "DejaVuSansMono" — is the canonical
        // regular for families that don't suffix their base file.
        if (lstem.find("light") == std::string::npos && lstem.find("thin") == std::string::npos &&
            lstem.find("black") == std::string::npos && lstem.find("heavy") == std::string::npos &&
            lstem.find("semi") == std::string::npos && lstem.find("extra") == std::string::npos)
            return 90;
        return 20; // some off-weight (Light/Thin/…) — usable but last resort
    };

    std::string best;
    int best_rank = -1;
    std::string best_mono;
    int best_mono_rank = -1;

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
            const int rank = regular_rank(stem);
            if (rank < 0) continue; // bold/italic variant — not a regular face

            if (!generic && stem.find(needle) != std::string::npos) {
                if (rank > best_rank) { best_rank = rank; best = p.string(); }
            }
            if (preferred_mono(stem) && rank > best_mono_rank) {
                best_mono_rank = rank;
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

namespace {
// Classify a font stem's style: bit0=bold, bit1=italic/oblique.
int style_bits(const std::string &lstem) {
    int b = 0;
    if (lstem.find("bold") != std::string::npos) b |= 1;
    if (lstem.find("italic") != std::string::npos || lstem.find("oblique") != std::string::npos)
        b |= 2;
    // "semibold"/"extrabold" also count as bold; "demibold" too — all contain
    // "bold". "black"/"heavy" are heavier weights we treat as bold as well.
    if (lstem.find("black") != std::string::npos || lstem.find("heavy") != std::string::npos)
        b |= 1;
    return b;
}
// How "plain" a styled candidate's weight is (higher = prefer). We want the
// design bold, not ExtraBold/Black, and the plain italic, not Light-Italic.
int weight_plainness(const std::string &lstem) {
    int score = 10;
    for (const char *w : {"extra", "semi", "demi", "ultra", "black", "heavy", "thin", "light"}) {
        if (lstem.find(w) != std::string::npos) score -= 3;
    }
    return score;
}
} // namespace

FontStyleFiles resolve_font_styles(std::string_view family, std::string_view regular_file) {
    FontStyleFiles out;
    int rank_b = -1, rank_i = -1, rank_bi = -1; // best weight-plainness seen per slot
    // The base family we're matching. Prefer the regular file's own stem (most
    // reliable), else the requested family name.
    std::string base;
    if (!regular_file.empty()) {
        base = lower(base_family(fs::path(regular_file).stem().string()));
    }
    if (base.empty() || base == "monospace") base = lower(family);
    if (base.empty() || base == "monospace" || base == "mono") return out; // can't match generically

    // Search the regular file's directory subtree first (a family's variants
    // almost always sit together), then the standard roots as a fallback.
    std::vector<fs::path> roots;
    if (!regular_file.empty()) {
        fs::path dir = fs::path(regular_file).parent_path();
        if (!dir.empty()) roots.push_back(dir);
    }
    for (const auto &r : font_roots()) roots.push_back(r);

    std::error_code ec;
    for (const auto &root : roots) {
        if (!fs::exists(root, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            const fs::path &p = it->path();
            if (!it->is_regular_file(ec) || !is_font_file(p)) continue;
            const std::string stem = lower(p.stem().string());
            if (lower(base_family(p.stem().string())) != base) continue; // different family
            const int plain = weight_plainness(stem);
            switch (style_bits(stem)) {
            case 1: if (plain > rank_b) { rank_b = plain; out.bold = p.string(); } break;
            case 2: if (plain > rank_i) { rank_i = plain; out.italic = p.string(); } break;
            case 3: if (plain > rank_bi) { rank_bi = plain; out.bold_italic = p.string(); } break;
            default: break; // 0 = regular, ignore
            }
        }
        // Keep scanning the fallback roots too — a plainer weight may live
        // elsewhere — but stop once every slot has a design-weight match.
        if (rank_b >= 10 && rank_i >= 10 && rank_bi >= 10) break;
    }
    return out;
}

} // namespace hand
