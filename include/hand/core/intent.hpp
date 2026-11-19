#pragma once
// hand::intents — what a widget can ASK FOR, as a type.
//
// A hand widget never reaches into the app. It declares the messages a user
// gesture on it produces, as plain structs in its own namespace, and lists
// them:
//
//     namespace hand::tabs {
//         struct Select { std::size_t index; };
//         struct Close  { std::size_t index; };
//         using intents = hand::intents<Select, Close>;
//     }
//
// The app builds its Msg from those lists, so "the app handles everything
// the tab strip can say" is a compile-time fact, not a convention:
//
//     using Msg = hand::msg_of<tabs::intents, palette::intents, Tick, Quit>;
//     static_assert(hand::Accepts<Msg, tabs::intents>);
//
// A gesture's handler returns one of these; mayag delivers it to the update()
// overload for that exact type on the same turn. No ids, no strings, no
// switch on "what was clicked".

#include <concepts>
#include <type_traits>
#include <variant>

namespace hand {

template <class... T>
struct intents {
    static constexpr std::size_t size = sizeof...(T);
};

namespace detail {
template <class T> struct as_list                 { using type = intents<T>; };
template <class... T> struct as_list<intents<T...>> { using type = intents<T...>; };

template <class... Ls> struct join                 { using type = intents<>; };
template <class... A> struct join<intents<A...>>   { using type = intents<A...>; };
template <class... A, class... B, class... Rest>
struct join<intents<A...>, intents<B...>, Rest...> : join<intents<A..., B...>, Rest...> {};

template <class L> struct to_variant;
template <class... T> struct to_variant<intents<T...>> { using type = std::variant<T...>; };

template <class T, class V> inline constexpr bool in_variant = false;
template <class T, class... A>
inline constexpr bool in_variant<T, std::variant<A...>> = (std::same_as<T, A> || ...);

template <class... T> inline constexpr bool unique = true;
template <class T, class... R>
inline constexpr bool unique<T, R...> = (!std::same_as<T, R> && ...) && unique<R...>;
template <class L> inline constexpr bool unique_list = false;
template <class... T> inline constexpr bool unique_list<intents<T...>> = unique<T...>;
}  // namespace detail

/// Flatten intent lists and loose message types into one list.
template <class... Xs>
using intents_of = typename detail::join<typename detail::as_list<Xs>::type...>::type;

/// The app's Msg: every listed intent and loose type, once each. Listing a
/// type twice is a compile error naming the mistake (std::variant would
/// otherwise accept it and then refuse to construct from it).
template <class... Xs>
    requires detail::unique_list<intents_of<Xs...>>
using msg_of = typename detail::to_variant<intents_of<Xs...>>::type;

/// Msg can carry every intent in L.
template <class Msg, class L>
concept Accepts = []<class... T>(intents<T...>*) {
    return (detail::in_variant<T, Msg> && ...);
}(static_cast<L*>(nullptr));

/// A widget's intents as one value: what its `intent(gesture)` returns.
template <class L>
using variant_of = typename detail::to_variant<L>::type;

/// Widen a widget's intent into the app's Msg (which Accepts all of them).
template <class Msg, class... T>
    requires (detail::in_variant<T, Msg> && ...)
[[nodiscard]] constexpr Msg lift(std::variant<T...> v) {
    return std::visit([](auto&& x) -> Msg { return Msg{std::move(x)}; }, std::move(v));
}

/// `lift` through an optional: the shape `on(const Model&, const Clicked&)`
/// returns.
template <class Msg, class V>
[[nodiscard]] constexpr std::optional<Msg> lift(std::optional<V> v) {
    if (!v) return std::nullopt;
    return lift<Msg>(std::move(*v));
}

}  // namespace hand
