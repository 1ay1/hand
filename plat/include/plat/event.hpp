// SPDX-License-Identifier: LGPL-2.0-or-later
//
// plat/event.hpp — the unified input/window Event sum type.
//
// Everything the OS delivers to the host is ONE closed variant. A closed sum is
// the type-theoretic right answer for "input": std::visit is exhaustive, so a
// new event kind is a COMPILE error at every handler until handled — you can
// never silently drop one. Mirrors toe::win::Event but is owned by plat (the
// platform layer), decoupled from the terminal engine.

#ifndef PLAT_EVENT_HPP
#define PLAT_EVENT_HPP

#include <cstdint>
#include <string>
#include <variant>

#include "plat/gpu.hpp" // Size

namespace plat {

// --- modifiers + keys -------------------------------------------------------

struct Mods {
    bool ctrl = false, alt = false, shift = false, super = false;
    constexpr auto operator<=>(const Mods &) const = default;
};

// Named non-text keys. Text-producing keys arrive as TextInput instead.
enum class Key : std::uint16_t {
    Enter, Backspace, Tab, Escape, Up, Down, Left, Right,
    Home, End, PageUp, PageDown, Delete, Insert,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    KpEnter,
};

enum class MouseButton : std::uint8_t { left, middle, right };

// --- the events -------------------------------------------------------------

struct CloseRequested {};                 // the user asked the window to close
struct Resized { Size px; };              // framebuffer resized (pixels)
struct FocusChanged { bool focused; };
struct ScaleChanged { float scale; };     // HiDPI / fractional-scale change

struct KeyDown { Key key; Mods mods; bool repeat = false; };
struct KeyUp   { Key key; Mods mods; };
struct TextInput { std::string utf8; Mods mods; }; // committed text (one+ scalars)
struct Preedit { std::string utf8; int caret = 0; }; // IME composition (uncommitted)

struct MouseDown { MouseButton button; int x, y; int clicks; Mods mods; };
struct MouseUp   { MouseButton button; int x, y; Mods mods; };
struct MouseMove { int x, y; bool button_down; Mods mods; };
struct MouseWheel { int dx, dy; Mods mods; };

// The closed set. Adding a kind here forces every std::visit handler to grow a
// case — the compiler enforces completeness.
using Event = std::variant<CloseRequested, Resized, FocusChanged, ScaleChanged, KeyDown, KeyUp,
                           TextInput, Preedit, MouseDown, MouseUp, MouseMove, MouseWheel>;

} // namespace plat

#endif // PLAT_EVENT_HPP
