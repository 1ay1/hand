// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Font discovery on Windows, via DirectWrite — the sibling of fonts_posix.cpp
// (directory globbing) and fonts_mac.cpp (Core Text).
//
// toe rasterises glyphs itself with stb_truetype, so it needs a FILE PATH, not
// a DWrite font object. DirectWrite is still the right way to find that path:
// it knows the installed collection, the real family names, and which faces are
// monospaced — none of which can be recovered reliably by globbing
// C:\Windows\Fonts.
//
// The path itself comes out through IDWriteFontFace::GetFiles ->
// IDWriteLocalFontFileLoader::GetFilePathFromKey, which is the documented way
// to get from a font face back to the file on disk.

#include "hand/platform/fonts.hpp"

#include <algorithm>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwrite.h>
#include <dwrite_1.h> // IDWriteFontFace1::IsMonospacedFont

namespace hand {

namespace {

[[nodiscard]] std::string narrow(const wchar_t *w) {
    if (!w || !*w) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

[[nodiscard]] std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                        nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// A scoped DirectWrite factory. Created per call: discovery happens a handful of
// times at startup, never on a hot path.
struct Factory {
    IDWriteFactory *p = nullptr;
    Factory() {
        ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown **>(&p));
    }
    ~Factory() { if (p) p->Release(); }
    explicit operator bool() const { return p != nullptr; }
};

// The localised family name, preferring en-us then the first available locale.
[[nodiscard]] std::string family_name(IDWriteFontFamily *fam) {
    IDWriteLocalizedStrings *names = nullptr;
    if (FAILED(fam->GetFamilyNames(&names))) return {};
    UINT32 idx = 0;
    BOOL exists = FALSE;
    if (FAILED(names->FindLocaleName(L"en-us", &idx, &exists)) || !exists) idx = 0;
    UINT32 len = 0;
    names->GetStringLength(idx, &len);
    std::wstring buf(len + 1, L'\0');
    names->GetString(idx, buf.data(), len + 1);
    names->Release();
    return narrow(buf.c_str());
}

// Walk a face back to the file it lives in.
[[nodiscard]] std::string face_file(IDWriteFont *font) {
    IDWriteFontFace *face = nullptr;
    if (FAILED(font->CreateFontFace(&face))) return {};

    UINT32 n = 1;
    IDWriteFontFile *file = nullptr;
    if (FAILED(face->GetFiles(&n, &file)) || n == 0 || !file) {
        face->Release();
        return {};
    }

    const void *key = nullptr;
    UINT32 key_size = 0;
    IDWriteFontFileLoader *loader = nullptr;
    std::string out;
    if (SUCCEEDED(file->GetReferenceKey(&key, &key_size)) &&
        SUCCEEDED(file->GetLoader(&loader)) && loader) {
        IDWriteLocalFontFileLoader *local = nullptr;
        if (SUCCEEDED(loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader),
                                             reinterpret_cast<void **>(&local))) &&
            local) {
            UINT32 len = 0;
            if (SUCCEEDED(local->GetFilePathLengthFromKey(key, key_size, &len))) {
                std::wstring path(len + 1, L'\0');
                if (SUCCEEDED(local->GetFilePathFromKey(key, key_size, path.data(), len + 1))) {
                    out = narrow(path.c_str());
                }
            }
            local->Release();
        }
        loader->Release();
    }
    file->Release();
    face->Release();
    return out;
}

// Find a family by name; `generic` picks the system default monospace instead.
[[nodiscard]] IDWriteFontFamily *find_family(IDWriteFontCollection *coll, std::string_view family) {
    // "monospace" is a generic alias, not a real Windows family. Consolas is the
    // universally-installed monospace face; Cascadia Mono ships with modern
    // Windows and is nicer, so prefer it when present.
    static const wchar_t *kFallbacks[] = {L"Cascadia Mono", L"Consolas", L"Courier New"};

    std::vector<std::wstring> tries;
    if (!family.empty() && family != "monospace") tries.push_back(widen(family));
    for (const wchar_t *f : kFallbacks) tries.emplace_back(f);

    for (const auto &t : tries) {
        UINT32 idx = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(coll->FindFamilyName(t.c_str(), &idx, &exists)) && exists) {
            IDWriteFontFamily *fam = nullptr;
            if (SUCCEEDED(coll->GetFontFamily(idx, &fam))) return fam;
        }
    }
    return nullptr;
}

// Pick the face matching a weight/style pair, or null.
[[nodiscard]] IDWriteFont *pick(IDWriteFontFamily *fam, DWRITE_FONT_WEIGHT w,
                                DWRITE_FONT_STYLE s) {
    IDWriteFont *font = nullptr;
    if (FAILED(fam->GetFirstMatchingFont(w, DWRITE_FONT_STRETCH_NORMAL, s, &font))) return nullptr;
    return font;
}

// True when the face reports fixed advance widths (what "monospace" means).
[[nodiscard]] bool is_monospace(IDWriteFont *font) {
    IDWriteFontFace *face = nullptr;
    if (FAILED(font->CreateFontFace(&face)) || !face) return false;
    IDWriteFontFace1 *f1 = nullptr;
    bool mono = false;
    if (SUCCEEDED(face->QueryInterface(__uuidof(IDWriteFontFace1),
                                       reinterpret_cast<void **>(&f1))) &&
        f1) {
        mono = f1->IsMonospacedFont() != FALSE;
        f1->Release();
    }
    face->Release();
    return mono;
}

} // namespace

std::string resolve_font_file(std::string_view family) {
    Factory f;
    if (!f) return {};
    IDWriteFontCollection *coll = nullptr;
    if (FAILED(f.p->GetSystemFontCollection(&coll, FALSE)) || !coll) return {};

    std::string out;
    if (IDWriteFontFamily *fam = find_family(coll, family)) {
        if (IDWriteFont *font = pick(fam, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL)) {
            out = face_file(font);
            font->Release();
        }
        fam->Release();
    }
    coll->Release();
    return out;
}

std::vector<std::string> list_monospace_families() {
    // "monospace" (the system-default alias) always leads, matching the POSIX
    // and macOS backends so the settings dropdown is consistent everywhere.
    std::vector<std::string> out{"monospace"};

    Factory f;
    if (!f) return out;
    IDWriteFontCollection *coll = nullptr;
    if (FAILED(f.p->GetSystemFontCollection(&coll, FALSE)) || !coll) return out;

    const UINT32 n = coll->GetFontFamilyCount();
    for (UINT32 i = 0; i < n; ++i) {
        IDWriteFontFamily *fam = nullptr;
        if (FAILED(coll->GetFontFamily(i, &fam)) || !fam) continue;
        if (IDWriteFont *font = pick(fam, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL)) {
            if (is_monospace(font)) {
                std::string name = family_name(fam);
                if (!name.empty()) out.push_back(std::move(name));
            }
            font->Release();
        }
        fam->Release();
    }
    coll->Release();

    // Sort + dedupe everything after the leading "monospace" alias.
    std::sort(out.begin() + 1, out.end());
    out.erase(std::unique(out.begin() + 1, out.end()), out.end());
    return out;
}

FontStyleFiles resolve_font_styles(std::string_view family, std::string_view regular_file) {
    (void)regular_file; // DirectWrite resolves variants by family, not by path
    FontStyleFiles out;

    Factory f;
    if (!f) return out;
    IDWriteFontCollection *coll = nullptr;
    if (FAILED(f.p->GetSystemFontCollection(&coll, FALSE)) || !coll) return out;

    if (IDWriteFontFamily *fam = find_family(coll, family)) {
        // Only report a variant when the family really has it: DirectWrite
        // returns the nearest match, so compare against the regular file and
        // drop a "variant" that is merely the regular face again — otherwise
        // toe would skip its own (better) synthesized bold/italic.
        std::string regular;
        if (IDWriteFont *r = pick(fam, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL)) {
            regular = face_file(r);
            r->Release();
        }
        auto variant = [&](DWRITE_FONT_WEIGHT w, DWRITE_FONT_STYLE s) -> std::string {
            IDWriteFont *font = pick(fam, w, s);
            if (!font) return {};
            std::string p = face_file(font);
            font->Release();
            return (p == regular) ? std::string{} : p;
        };
        out.bold = variant(DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL);
        out.italic = variant(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC);
        out.bold_italic = variant(DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_ITALIC);
        fam->Release();
    }
    coll->Release();
    return out;
}

} // namespace hand
