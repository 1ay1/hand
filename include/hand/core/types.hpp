#pragma once
// hand::types — the vocabulary every layer speaks.
//
// A value says what it is in its TYPE, not in a comment beside an int:
//
//   SessionId    which shell (routes pty output, keys its reader stream)
//   TabIndex     a slot in the tab strip (a position, not an identity)
//   Step         one step either way: backward / forward, never "-1 / 1"
//   Confirm      a request that may still ask (asking) or already did (given)
//
// Strong<Tag, T> is the newtype: same bits as T, but a SessionId never
// converts to a TabIndex, or to a bare integer, without saying so (.get()).
// No layer depends on anything here except the standard library.

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

namespace hand {

// ── newtype ────────────────────────────────────────────────────────────────

/// T, but distinct per Tag. Explicit in (`SessionId{7}`), explicit out
/// (.get()); compares and hashes like T. No arithmetic: an id is not a number.
/// An aggregate on purpose: jaal proves a message Sendable by walking its
/// public fields, so the one field is public -- read it through get().
template <class Tag, class T>
struct Strong {
    using value_type = T;
    T value{};
    [[nodiscard]] constexpr const T& get() const noexcept { return value; }
    friend constexpr bool operator==(const Strong&, const Strong&) = default;
    friend constexpr auto operator<=>(const Strong&, const Strong&) = default;
};

// ── identities ─────────────────────────────────────────────────────────────

/// A shell, for as long as it runs: stable across tab moves. 0 = none.
using SessionId = Strong<struct SessionIdTag, std::uint64_t>;
inline constexpr SessionId kNoSession{0};

/// A slot in the tab strip, left to right. Changes when tabs move.
using TabIndex = Strong<struct TabIndexTag, std::size_t>;

/// Hands out SessionIds, never the same one twice.
class SessionIds {
  public:
    [[nodiscard]] constexpr SessionId next() noexcept { return SessionId{n_++}; }

  private:
    std::uint64_t n_ = 1;
};

// ── small closed sets ─────────────────────────────────────────────────────

/// One step through an ordered list.
enum class Step : std::int8_t { backward = -1, forward = 1 };

/// The step as a signed offset.
[[nodiscard]] constexpr int delta(Step s) noexcept { return static_cast<int>(s); }
/// The other way.
[[nodiscard]] constexpr Step reverse(Step s) noexcept { return s == Step::forward ? Step::backward : Step::forward; }
/// i stepped by s around [0, n) (wraps).
[[nodiscard]] constexpr std::size_t wrap(std::size_t i, Step s, std::size_t n) noexcept {
    if (n == 0) return 0;
    const auto k = static_cast<long long>(n);
    return static_cast<std::size_t>(((static_cast<long long>(i) + delta(s)) % k + k) % k);
}

/// Whether a request may still stop to ask the user, or the user already
/// answered. Replaces `bool confirmed`: `CloseTab{i, Confirm::given}` reads.
enum class Confirm : std::uint8_t { asking, given };

// ── sum-type plumbing ──────────────────────────────────────────────────────

/// Lambdas as one visitor: std::visit(overloaded{[](A){}, [](B){}}, v).
template <class... F>
struct overloaded : F... {
    using F::operator()...;
};
template <class... F>
overloaded(F...) -> overloaded<F...>;

}  // namespace hand

template <class Tag, class T>
struct std::hash<hand::Strong<Tag, T>> {
    std::size_t operator()(const hand::Strong<Tag, T>& s) const noexcept { return std::hash<T>{}(s.get()); }
};
