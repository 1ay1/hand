// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Test/tooling support for the platform layer. NOT part of the normal frontend
// path — only the renderer tests use it: they need a current GL context to draw
// into an offscreen FBO and read pixels back.

#ifndef HAND_PLATFORM_TESTING_HPP
#define HAND_PLATFORM_TESTING_HPP

#include "toe/core/types.hpp"

namespace hand::platform {

// Create an offscreen EGL context and make it current on the calling thread.
// Returns true on success; the context is leaked for the process lifetime.
[[nodiscard]] bool make_offscreen_context_current(toe::PixelSize size);

} // namespace hand::platform

#endif // HAND_PLATFORM_TESTING_HPP
