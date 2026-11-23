// hand::hint_scan against its spec: the std::regex each scanner replaced
// (include/hand/core/hint_scan.hpp). Random strings built from the pieces
// the patterns care about; every match the regex walk finds, the scanner
// walk must find, and nothing else.

#include <hand/core/hint_scan.hpp>

#include <cstdio>
#include <random>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace {

using Match = std::pair<std::size_t, std::size_t>;
namespace hs = hand::hint_scan;

std::vector<Match> by_regex(const std::regex& re, const std::string& line) {
    std::vector<Match> out;
    for (auto it = std::sregex_iterator(line.begin(), line.end(), re); it != std::sregex_iterator{}; ++it)
        out.emplace_back(static_cast<std::size_t>(it->position()), static_cast<std::size_t>(it->position() + it->length()));
    return out;
}
std::vector<Match> by_scan(std::size_t (*scan)(std::string_view, std::size_t), const std::string& line) {
    std::vector<Match> out;
    for (std::size_t i = 0; i < line.size();) {
        const std::size_t e = scan(line, i);
        if (e != hs::npos && e > i) { out.emplace_back(i, e); i = e; }
        else ++i;
    }
    return out;
}

}  // namespace

int main() {
    const std::regex url{R"(((?:https?|ftp|file)://|www\.)[^\s<>"'`]+)"};
    const std::regex path{R"((?:~|\.{1,2})?/[\w.@+~\-]+(?:/[\w.@+~\-]+)*/?|[\w.@+\-]+(?:/[\w.@+\-]+)+/?)"};
    const std::regex hash{R"(\b[0-9a-f]{7,40}\b)"};
    const std::regex ip{R"(\b(?:\d{1,3}\.){3}\d{1,3}(?::\d+)?\b)"};
    const std::vector<std::string> atoms = {"a", "f", "0", "1", "9", "g", "Z", "_", ".", "/", "~", ":", "@", "+", "-", " ", "\t",
                                            "<", "\"", "'", "`", ")", "http://", "https://", "ftp://", "file://", "www.", "..",
                                            "\xc3\xa9", "abcdef0", "1234567", "192.168.", "3f9a2c1d", "~/", "./", "../", "::"};
    std::mt19937_64 rng(42);   // fixed: a failure reproduces
    int bad = 0;
    for (int n = 0; n < 40000; ++n) {
        std::string s;
        const int len = static_cast<int>(rng() % 24);
        for (int k = 0; k < len; ++k) s += atoms[rng() % atoms.size()];
        const auto cmp = [&](const char* what, const std::regex& re, std::size_t (*scan)(std::string_view, std::size_t)) {
            if (by_regex(re, s) != by_scan(scan, s) && ++bad <= 10) std::fprintf(stderr, "FAIL %s: [%s]\n", what, s.c_str());
        };
        cmp("url", url, hs::scan_url);
        cmp("path", path, hs::scan_path);
        cmp("hash", hash, hs::scan_hash);
        cmp("ip", ip, hs::scan_ip);
    }
    if (bad) std::fprintf(stderr, "hint_scan: %d strings where a scanner and its regex disagree\n", bad);
    return bad != 0;
}
