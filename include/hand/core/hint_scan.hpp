#pragma once
// hand::hint_scan — the patterns hint mode labels (URLs, paths, git hashes,
// IPs), matched by hand instead of by std::regex.
//
// std::regex took ~27 ms to scan one screen (the slowest update in the app:
// jaal's fold budget named it, "slow update: hand::msg::OpenHints"). These
// scanners take ~0.15 ms. Each regex below is the SPEC: scan_*(s, i) returns
// the end of the match std::regex (ECMAScript) finds starting at i, or npos,
// and tests/hint_scan_test.cpp checks that on random strings against the
// regex itself.
//
//   url   ((?:https?|ftp|file)://|www\.)[^\s<>"'`]+
//   path  (?:~|\.{1,2})?/[\w.@+~\-]+(?:/[\w.@+~\-]+)*/?|[\w.@+\-]+(?:/[\w.@+\-]+)+/?
//   hash  \b[0-9a-f]{7,40}\b
//   ip    \b(?:\d{1,3}\.){3}\d{1,3}(?::\d+)?\b
//
// Walking a line: at each i try the scanner; on a match take [i, end) and
// continue from end, else step one byte (what sregex_iterator does).

#include <cstddef>
#include <string_view>

namespace hand::hint_scan {

inline constexpr std::size_t npos = std::string_view::npos;

namespace detail {
constexpr bool digit(char c) { return c >= '0' && c <= '9'; }
constexpr bool word(char c) { return digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
constexpr bool hex(char c) { return digit(c) || (c >= 'a' && c <= 'f'); }
constexpr bool path_ch(char c, bool tilde) {
    return word(c) || c == '.' || c == '@' || c == '+' || c == '-' || (tilde && c == '~');
}
constexpr bool url_ch(char c) {
    switch (c) {
        case ' ': case '\t': case '\n': case '\v': case '\f': case '\r':
        case '<': case '>': case '"': case '\'': case '`': return false;
        default: return true;
    }
}
/// \b at i: a word character on exactly one side (the edges count as not).
constexpr bool boundary(std::string_view s, std::size_t i) {
    const bool l = i > 0 && word(s[i - 1]), r = i < s.size() && word(s[i]);
    return l != r;
}
template <class Pred>
constexpr std::size_t run(std::string_view s, std::size_t i, Pred pred) {
    while (i < s.size() && pred(s[i])) ++i;
    return i;
}
}  // namespace detail

constexpr std::size_t scan_url(std::string_view s, std::size_t i) {
    const std::string_view t = s.substr(i);
    std::size_t p = npos;
    for (std::string_view lead : {"https://", "http://", "ftp://", "file://", "www."})
        if (t.starts_with(lead)) { p = i + lead.size(); break; }
    if (p == npos) return npos;
    const std::size_t e = detail::run(s, p, detail::url_ch);
    return e > p ? e : npos;
}

constexpr std::size_t scan_path(std::string_view s, std::size_t i) {
    using namespace detail;
    const auto seg = [&](std::size_t p, bool tilde) {   // "/" then one or more path characters
        if (p >= s.size() || s[p] != '/') return npos;
        const std::size_t e = run(s, p + 1, [&](char c) { return path_ch(c, tilde); });
        return e > p + 1 ? e : npos;
    };
    const auto rest = [&](std::size_t e, bool tilde) {   // (?:/seg)* /?
        for (std::size_t n; (n = seg(e, tilde)) != npos;) e = n;
        return e < s.size() && s[e] == '/' ? e + 1 : e;
    };
    // the first alternative: an optional ~ . .. then an absolute path
    const std::string_view t = s.substr(i);
    std::size_t p = npos;
    if (t.starts_with("~/")) p = i + 1;
    else if (t.starts_with("../")) p = i + 2;
    else if (t.starts_with("./")) p = i + 1;
    else if (t.starts_with("/")) p = i;
    if (p != npos)
        if (const std::size_t e = seg(p, true); e != npos) return rest(e, true);
    // the second: a relative path with at least one slash (and no ~)
    const std::size_t e0 = run(s, i, [](char c) { return path_ch(c, false); });
    if (e0 == i) return npos;
    const std::size_t e1 = seg(e0, false);
    return e1 == npos ? npos : rest(e1, false);
}

constexpr std::size_t scan_hash(std::string_view s, std::size_t i) {
    using namespace detail;
    if (!boundary(s, i)) return npos;
    const std::size_t e = run(s, i, hex);
    return e - i >= 7 && e - i <= 40 && boundary(s, e) ? e : npos;
}

constexpr std::size_t scan_ip(std::string_view s, std::size_t i) {
    using namespace detail;
    if (!boundary(s, i)) return npos;
    std::size_t p = i;
    for (int k = 0; k < 4; ++k) {
        const std::size_t e = run(s, p, digit);
        if (e == p || e - p > 3) return npos;
        p = e;
        if (k < 3) {
            if (p >= s.size() || s[p] != '.') return npos;
            ++p;
        }
    }
    if (p < s.size() && s[p] == ':') {   // a port, when \b holds after it
        const std::size_t e = run(s, p + 1, digit);
        if (e > p + 1 && boundary(s, e)) return e;
    }
    return boundary(s, p) ? p : npos;
}

}  // namespace hand::hint_scan
