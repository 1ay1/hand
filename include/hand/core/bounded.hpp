#pragma once
// hand::Bounded — a number that cannot leave its range.
//
// Every setting a user can drag has a legal range, and every place that
// reads it would otherwise re-clamp it (or forget to). Putting the range in
// the TYPE makes an out-of-range font size unrepresentable, and lets a
// control derive its own track from the field it edits:
//
//     using FontSize = Bounded<float, 8.0f, 28.0f>;
//     FontSize s = 40.0f;          // holds 28
//     s.fraction()                 // 0..1 along the range, for a slider
//     FontSize::from_fraction(0.5) // the middle

#include <algorithm>
#include <compare>
#include <concepts>
#include <type_traits>

namespace hand {

template <class T, T Lo, T Hi>
    requires std::is_arithmetic_v<T> && (Lo < Hi)
class Bounded {
  public:
    using value_type = T;
    static constexpr T lo = Lo;
    static constexpr T hi = Hi;

    constexpr Bounded() noexcept : v_{Lo} {}
    constexpr Bounded(T v) noexcept : v_{std::clamp(v, Lo, Hi)} {}   // NOLINT: clamping is the point

    [[nodiscard]] constexpr T get() const noexcept { return v_; }
    constexpr operator T() const noexcept { return v_; }               // NOLINT

    [[nodiscard]] constexpr float fraction() const noexcept {
        return static_cast<float>(v_ - Lo) / static_cast<float>(Hi - Lo);
    }
    [[nodiscard]] static constexpr Bounded from_fraction(float f) noexcept {
        const float c = std::clamp(f, 0.0f, 1.0f);
        if constexpr (std::is_integral_v<T>)
            return Bounded{static_cast<T>(static_cast<float>(Lo) + c * static_cast<float>(Hi - Lo) + 0.5f)};
        else
            return Bounded{static_cast<T>(static_cast<float>(Lo) + c * static_cast<float>(Hi - Lo))};
    }

    friend constexpr auto operator<=>(const Bounded&, const Bounded&) = default;

  private:
    T v_;
};

template <class B>
concept BoundedValue = requires {
    typename B::value_type;
    { B::lo } -> std::convertible_to<typename B::value_type>;
    { B::hi } -> std::convertible_to<typename B::value_type>;
} && std::same_as<B, Bounded<typename B::value_type, B::lo, B::hi>>;

static_assert(Bounded<int, 0, 10>{42}.get() == 10);
static_assert(Bounded<float, 8.0f, 28.0f>::from_fraction(0.5f).get() == 18.0f);
static_assert(BoundedValue<Bounded<float, 0.0f, 1.0f>>);
static_assert(!BoundedValue<float>);

}  // namespace hand
