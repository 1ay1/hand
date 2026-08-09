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
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

// stb_truetype is vendored by toe (its impl is compiled into libtoe, which hand
// links) — we include the header for DECLARATIONS only (no IMPLEMENTATION) to
// read font metrics and detect monospace by ADVANCE WIDTH, not by name. This is
// what lets us find Iosevka, Terminus, Berkeley Mono, PragmataPro, etc. — great
// mono fonts whose names don't contain "mono".
#include "stb/stb_truetype.h"

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

// The standard search roots, most-specific first (user overrides system). We
// cast a WIDE net so fonts installed anywhere a modern Linux stashes them are
// found: user dirs, XDG_DATA_DIRS, Flatpak/host, and Nix profiles.
std::vector<fs::path> font_roots() {
    std::vector<fs::path> roots;
    auto add = [&](fs::path p) {
        for (const auto &r : roots) if (r == p) return; // dedup
        roots.emplace_back(std::move(p));
    };
    if (const char *home = std::getenv("HOME"); home && *home) {
        add(fs::path(home) / ".local/share/fonts");
        add(fs::path(home) / ".fonts");
        add(fs::path(home) / ".nix-profile/share/fonts"); // Nix (per-user)
    }
    if (const char *xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        add(fs::path(xdg) / "fonts");
    // Every dir in $XDG_DATA_DIRS (colon-separated) may hold a fonts/ subdir.
    if (const char *dirs = std::getenv("XDG_DATA_DIRS"); dirs && *dirs) {
        std::string s(dirs);
        std::size_t i = 0;
        while (i < s.size()) {
            std::size_t j = s.find(':', i);
            if (j == std::string::npos) j = s.size();
            if (j > i) add(fs::path(s.substr(i, j - i)) / "fonts");
            i = j + 1;
        }
    }
    add("/usr/share/fonts");
    add("/usr/local/share/fonts");
    add("/run/host/usr/share/fonts");        // Flatpak: host fonts
    add("/run/host/usr/local/share/fonts");
    add("/nix/var/nix/profiles/default/share/fonts"); // Nix (system)
    add("/opt/homebrew/share/fonts");        // Linuxbrew / brew
    add("/Library/Fonts");                   // (harmless on Linux; a no-op)
    return roots;
}

// Read a font file and decide whether it's MONOSPACE by comparing the advance
// widths of a narrow glyph ('i'/'l') and a wide one ('M'/'W'): in a monospaced
// face they're identical. Metric-based, so it works regardless of the family
// name. `.ttc` collections: check face 0. Returns false on any read error.
bool is_monospace_file(const fs::path &path) {
    std::error_code ec;
    const auto sz = fs::file_size(path, ec);
    if (ec || sz == 0 || sz > 64u * 1024 * 1024) return false; // sanity cap
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<unsigned char> buf(static_cast<std::size_t>(sz));
    if (!f.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(sz)))
        return false;
    const int off = stbtt_GetFontOffsetForIndex(buf.data(), 0);
    if (off < 0) return false;
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, buf.data(), off)) return false;
    auto adv = [&](int cp) {
        int a = 0, lsb = 0;
        const int g = stbtt_FindGlyphIndex(&info, cp);
        if (g == 0) return -1;
        stbtt_GetGlyphHMetrics(&info, g, &a, &lsb);
        return a;
    };
    const int narrow = adv('i') > 0 ? adv('i') : adv('l');
    const int wide = adv('M') > 0 ? adv('M') : adv('W');
    const int space = adv(' ');
    if (narrow <= 0 || wide <= 0) return false;
    // Exactly equal narrow/wide advance is the monospace signature. Also require
    // the space to match (rules out a few proportional fonts that happen to have
    // equal i/M). A tiny tolerance absorbs rounding in odd faces.
    if (std::abs(narrow - wide) > 1) return false;
    if (space > 0 && std::abs(space - wide) > 1) return false;
    return true;
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

    // Fast path: ask fontconfig to resolve the family to a concrete file. This
    // is the AUTHORITATIVE mapping (respects the user's fontconfig, aliases,
    // and every install dir), so a name we listed from `fc-list` resolves to
    // the right face — including families with no "mono" in the name. We only
    // trust an EXACT-ish match (fc-match falls back to a default otherwise, so
    // verify the returned file's family relates to what was asked).
    if (!generic) {
        std::string cmd = "fc-match -f '%{file}' ";
        // Shell-quote the family (single quotes; escape any embedded quote).
        std::string q = "'";
        for (char c : family) { if (c == '\'') q += "'\\''"; else q += c; }
        q += ":spacing=100'"; // prefer the monospaced face of that family
        cmd += q + " 2>/dev/null";
        if (FILE *pipe = ::popen(cmd.c_str(), "r")) {
            char buf[4096];
            std::string file;
            if (std::fgets(buf, sizeof buf, pipe)) file = buf;
            ::pclose(pipe);
            while (!file.empty() && (file.back() == '\n' || file.back() == '\r')) file.pop_back();
            // Accept only a real, readable font file whose name plausibly
            // matches (fc-match ALWAYS returns something — its fallback default
            // when the family is unknown; guard against that).
            std::error_code fec;
            if (!file.empty() && fs::exists(file, fec) && is_font_file(fs::path(file))) {
                // Collapse spaces/hyphens on BOTH sides so "JetBrains Mono"
                // matches a "JetBrainsMono-Regular.ttf" stem. fc-match ALWAYS
                // returns something (its default when the family is unknown),
                // so require the returned file's family to actually contain the
                // requested one — else fall through to the metric-ranked walk.
                auto collapse = [](std::string s) {
                    std::string o;
                    for (char c : s)
                        if (c != ' ' && c != '-' && c != '_')
                            o += static_cast<char>(std::tolower((unsigned char)c));
                    return o;
                };
                const std::string want = collapse(std::string(family));
                const std::string got = collapse(fs::path(file).stem().string());
                if (!want.empty() && got.find(want) != std::string::npos) return file;
            }
        }
    }

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

// --- fontconfig fast path --------------------------------------------------
// When the `fc-list` CLI is available (nearly every Linux desktop), it is the
// AUTHORITATIVE source: it knows every installed family, honours the user's
// fontconfig, and can filter to monospaced faces (spacing=mono=100) directly.
// We parse `fc-list :spacing=100 family` — one family per line, comma-separated
// localised names (we keep the first). Returns empty if fc-list isn't present.
std::vector<std::string> fc_list_mono() {
    std::vector<std::string> out;
    // -f prints just the family; :spacing=100 restricts to monospaced faces.
    FILE *pipe = ::popen("fc-list :spacing=100 family 2>/dev/null", "r");
    if (!pipe) return out;
    char line[1024];
    while (std::fgets(line, sizeof line, pipe)) {
        std::string s(line);
        // Trim trailing newline/CR.
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.empty()) continue;
        // fc-list may print several comma-separated localised names; take the
        // first (usually the English/latin one).
        if (auto comma = s.find(','); comma != std::string::npos) s.erase(comma);
        // Some builds append a trailing "=" style token — strip anything after it.
        if (auto eq = s.find("="); eq != std::string::npos) s.erase(eq);
        while (!s.empty() && s.back() == ' ') s.pop_back();
        if (!s.empty()) out.push_back(std::move(s));
    }
    ::pclose(pipe);
    std::sort(out.begin(), out.end(),
              [](const std::string &a, const std::string &b) { return lower(a) < lower(b); });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::string> list_monospace_families() {
    std::vector<std::string> out;
    out.push_back("monospace"); // the system-default alias, always first

    // 1) fontconfig: authoritative + fast when present. Use it if it returns
    //    anything useful.
    if (auto fc = fc_list_mono(); !fc.empty()) {
        for (auto &f : fc) out.push_back(std::move(f));
        return out;
    }

    // 2) Fallback: walk the font roots and detect monospace by METRICS (advance
    //    width), not by name — so Iosevka, Terminus, Berkeley Mono, PragmataPro,
    //    Departure Mono, etc. are all found even without "mono" in the name.
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
            // Quick accept for obvious mono names / our preferred list; otherwise
            // fall back to the (more expensive) metric probe.
            const bool named_mono = lstem.find("mono") != std::string::npos || preferred_mono(lstem);
            if (!named_mono && !is_monospace_file(p)) continue;
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
