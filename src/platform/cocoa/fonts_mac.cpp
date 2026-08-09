// SPDX-License-Identifier: LGPL-2.0-or-later
//
// macOS font resolution — HOST policy. The Cocoa host knows where macOS keeps
// its faces (/System/Library/Fonts, /Library/Fonts, ~/Library/Fonts) and which
// built-ins make a good default terminal font (SF Mono, Menlo, Monaco). It
// resolves a concrete file and hands it to toe via Config::font_file, so the
// portable engine carries no macOS font path.

#include "hand/platform/fonts.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <vector>

#include <CoreText/CoreText.h>

namespace hand {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string_view s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s)
        if (c != ' ' && c != '-')
            r += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

bool is_font(const fs::path &p) {
    std::string e = lower(p.extension().string());
    return e == ".ttf" || e == ".otf" || e == ".ttc";
}

} // namespace

std::string resolve_font_file(std::string_view family) {
    const std::string needle = lower(family);
    const bool generic = needle.empty() || needle == "monospace" || needle == "mono" ||
                         needle == "sans" || needle == "serif" || needle == "sansserif";

    // macOS built-in monospace faces, best first. Direct paths so the common
    // case needs no directory walk at all.
    static const char *builtins[] = {
        "/System/Library/Fonts/SFNSMono.ttf",
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/Monaco.ttf",
        "/System/Library/Fonts/Courier.ttc",
    };
    if (generic) {
        for (const char *b : builtins) {
            if (fs::exists(b)) return b;
        }
    }

    // Otherwise (or if no built-in was present) walk the macOS font roots for a
    // file whose name contains the requested family.
    const char *home = std::getenv("HOME");
    std::vector<fs::path> roots = {"/System/Library/Fonts", "/Library/Fonts"};
    if (home) roots.emplace_back(std::string{home} + "/Library/Fonts");

    std::string best;
    for (const fs::path &root : roots) {
        std::error_code ec;
        if (!fs::exists(root, ec)) continue;
        for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            const fs::path &p = it->path();
            if (!is_font(p)) continue;
            const std::string name = lower(p.filename().string());
            // Skip bold/italic files as the base face.
            if (name.find("bold") != std::string::npos || name.find("italic") != std::string::npos ||
                name.find("oblique") != std::string::npos)
                continue;
            if (!generic && name.find(needle) != std::string::npos) return p.string();
            if (generic && best.empty() && name.find("mono") != std::string::npos)
                best = p.string();
        }
    }
    return best;
}

// List installed monospace font families via CoreText. A family is "monospace"
// if its representative font advertises the fixed-pitch symbolic trait. Names
// are sorted case-insensitively and de-duped; "monospace" (the system-default
// alias) is always first so there's a sane default choice.
std::vector<std::string> list_monospace_families() {
    std::vector<std::string> out;
    out.push_back("monospace");

    CFArrayRef families = CTFontManagerCopyAvailableFontFamilyNames();
    if (!families) return out;

    std::set<std::string> mono; // sorted, de-duped
    const CFIndex n = CFArrayGetCount(families);
    for (CFIndex i = 0; i < n; ++i) {
        CFStringRef fam = static_cast<CFStringRef>(CFArrayGetValueAtIndex(families, i));
        if (!fam) continue;
        // Build a font for this family and test the fixed-pitch trait.
        CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionaryAddValue(attrs, kCTFontFamilyNameAttribute, fam);
        CTFontDescriptorRef desc = CTFontDescriptorCreateWithAttributes(attrs);
        CFRelease(attrs);
        if (!desc) continue;
        CTFontRef font = CTFontCreateWithFontDescriptor(desc, 12.0, nullptr);
        CFRelease(desc);
        if (!font) continue;
        const CTFontSymbolicTraits traits = CTFontGetSymbolicTraits(font);
        CFRelease(font);
        if (!(traits & kCTFontTraitMonoSpace)) continue;

        // Skip families starting with a dot (system-hidden, e.g. ".SF NS Mono").
        char buf[256];
        if (CFStringGetCString(fam, buf, sizeof buf, kCFStringEncodingUTF8)) {
            if (buf[0] == '.') continue;
            mono.insert(buf);
        }
    }
    CFRelease(families);

    for (const std::string &m : mono) out.push_back(m);
    return out;
}

} // namespace hand
