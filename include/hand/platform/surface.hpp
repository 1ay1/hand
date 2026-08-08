// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand::platform vocabulary shim.
//
// The whole host contract now lives in the engine as `toe::App` (toe/app.hpp) —
// the loop that drives it is `toe::run<App>`. hand's native backends model that
// contract (via distinct hand::WaylandApp/X11App/OffscreenApp handles). This
// re-exports the toe value types and windowing-event names into hand::platform,
// letting the backend translation units keep naming `Event`, `KeyPressed`,
// `MouseButton`, `PixelSize`, … unqualified as before.

#ifndef HAND_PLATFORM_SURFACE_HPP
#define HAND_PLATFORM_SURFACE_HPP

#include "toe/app.hpp"

namespace hand::platform {

// Engine value types the backends name unqualified.
using toe::DamageRect;
using toe::Extent;
using toe::KeyEvent;
using toe::Modifiers;
using toe::PixelSize;
using toe::Result;
using toe::SpecialKey;
using toe::TextInput;
using toe::WindowConfig;
using toe::fail;

// The platform-neutral event sum type and its members. These live in toe::win
// (nested to avoid clashing with the engine's identically-named TEA messages),
// so the backends can keep naming them unqualified inside hand::platform.
using toe::win::CloseRequested;
using toe::win::Event;
using toe::win::EventSink;
using toe::win::FocusChanged;
using toe::win::KeyPressed;
using toe::win::MouseButton;
using toe::win::MouseDown;
using toe::win::MouseMove;
using toe::win::MouseUp;
using toe::win::MouseWheel;
using toe::win::Preedit;
using toe::win::Resized;
using toe::win::TextEntered;

} // namespace hand::platform

#endif // HAND_PLATFORM_SURFACE_HPP
