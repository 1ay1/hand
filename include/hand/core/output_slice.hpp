#pragma once
// hand::output_slice — how much pty output to hand the UI in one message.
//
// The loop folds one Output at a time, and a keystroke or a due frame waits
// behind the fold in progress. So a slice is bounded by what it COSTS toe to
// parse, not by its size: bytes are the wrong unit, because a line costs far
// more than a byte. Measured on toe (release, M-class core):
//
//   `yes`            ~12 ms/MB   (350k lines a MB: the line feed is the cost)
//   build log        ~2.5 ms/MB  (colour, ~85-byte lines)
//   wide text, CJK   ~4-9 ms/MB
//
// A 512 KB slice of `yes` was an 8-17 ms fold: a dropped frame and a late key
// under every flood (jaal's fold budget named it). The model below is fitted
// pessimistically to those numbers -- 5 ns a byte plus 30 ns a line -- and a
// slice ends when it reaches kSliceCost (4 ms, a quarter of a frame), or at
// kSliceMax bytes, whichever comes first. Ordinary text keeps its 512 KB
// slices (throughput unchanged); `yes` gets ~120 KB ones. Either way no fold
// holds a key for more than a few milliseconds.
//
// The cut never splits a UTF-8 character.

#include <cstddef>
#include <cstring>
#include <string_view>

namespace hand {

inline constexpr std::size_t kSliceMax = 512u << 10;
inline constexpr double kSliceCostNs = 4.0e6;
inline constexpr double kByteNs = 5.0, kLineNs = 30.0;

/// Parse cost of `s` in ns, as modelled above.
[[nodiscard]] inline double parse_cost_ns(std::string_view s) noexcept {
    std::size_t lines = 0;
    for (const char* p = s.data(), *e = s.data() + s.size();
         (p = static_cast<const char*>(std::memchr(p, '\n', static_cast<std::size_t>(e - p)))) != nullptr; ++p)
        ++lines;
    return kByteNs * static_cast<double>(s.size()) + kLineNs * static_cast<double>(lines);
}

/// Bytes of `buf` to send as the next Output: all of it when it is within
/// the budget, else the longest prefix that is, cut on a UTF-8 boundary.
/// Never 0 for a non-empty buffer.
[[nodiscard]] inline std::size_t output_slice(std::string_view buf) noexcept {
    std::size_t n = buf.size() < kSliceMax ? buf.size() : kSliceMax;
    // one pass: walk lines until the budget is spent
    double cost = 0.0;
    std::size_t at = 0;
    while (at < n) {
        const void* nl = std::memchr(buf.data() + at, '\n', n - at);
        const std::size_t end = nl ? static_cast<std::size_t>(static_cast<const char*>(nl) - buf.data()) + 1 : n;
        const double c = kByteNs * static_cast<double>(end - at) + (nl ? kLineNs : 0.0);
        if (cost + c > kSliceCostNs) {
            // the budget ends inside this line: take the bytes that fit
            const double left = kSliceCostNs - cost;
            const std::size_t fit = left > 0.0 ? static_cast<std::size_t>(left / kByteNs) : 0;
            n = at + (fit < end - at ? fit : end - at);
            break;
        }
        cost += c;
        at = end;
    }
    if (n == 0) n = 1;
    if (n < buf.size())   // cut on a UTF-8 boundary
        for (std::size_t back = 0; back < 4 && n > 1 && (static_cast<unsigned char>(buf[n]) & 0xC0) == 0x80; ++back) --n;
    return n;
}

}  // namespace hand
