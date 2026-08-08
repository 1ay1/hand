// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand::platform surface shim.
//
// The Surface contract now lives in the engine, as `toe::Surface` (see
// toe/core/surface.hpp) — the loop that consumes it is `toe::run`, so the
// concept belongs with the engine that drives it, not the frontend. hand's
// native backends (Wayland/X11/offscreen) still MODEL that concept, so this
// header simply re-exports the toe names into `hand::platform`, letting the
// backend translation units keep naming `Event`, `KeyPressed`, `MouseButton`,
// … unqualified (or `platform::Surface`) exactly as before.

#ifndef HAND_PLATFORM_SURFACE_HPP
#define HAND_PLATFORM_SURFACE_HPP

#include "toe/core/surface.hpp"

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

// The concept + its refinements and uniform accessors.
using toe::Surface;
using toe::ClipboardSurface;
using toe::DamageableSurface;
using toe::FlushableSurface;
using toe::RepeatingSurface;
using toe::TitledSurface;

using toe::clipboard_get;
using toe::clipboard_set;
using toe::flush;
using toe::present;
using toe::repeat_fd;
using toe::title;

} // namespace hand::platform

#endif // HAND_PLATFORM_SURFACE_HPP
