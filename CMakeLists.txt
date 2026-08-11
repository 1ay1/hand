# SPDX-License-Identifier: LGPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)
project(hand VERSION 0.1 LANGUAGES CXX C)

# macOS's Cocoa backend is Objective-C++ (.mm). Enable the language only there.
if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to an optimized build — a terminal's parse/render hot paths are 10x
# slower under -O0, and the flood throughput lives here.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(FetchContent)

# --- vendored dependencies ---------------------------------------------------
# hand pulls both of its dependencies the same way: a sibling checkout when one
# exists (local development), otherwise fetched from GitHub. Override a source
# with -D<DEP>_DIR=/path or a ref with -D<DEP>_TAG=<git-ref>.

# libtoe — the terminal ENGINE hand is a frontend over. toe is now pure
# "terminal work" (VT parse, screen model, GL renderer); it has no window-system
# dependency at all. hand owns the platform (window/input/clipboard), below.
set(TOE_TAG "main" CACHE STRING "toe git ref to fetch when no sibling exists")
if(NOT DEFINED TOE_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../toe/CMakeLists.txt")
  set(TOE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../toe")
endif()
if(DEFINED TOE_DIR)
  message(STATUS "hand: using libtoe from ${TOE_DIR}")
  set(TOE_BUILD_DEMO  OFF CACHE BOOL "" FORCE)
  set(TOE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  add_subdirectory("${TOE_DIR}" "${CMAKE_BINARY_DIR}/toe")
else()
  message(STATUS "hand: fetching libtoe (${TOE_TAG}) from github.com/1ay1/toe")
  FetchContent_Declare(toe
    GIT_REPOSITORY https://github.com/1ay1/toe.git
    GIT_TAG        ${TOE_TAG})
  FetchContent_MakeAvailable(toe)
endif()

# vibe — the config format parser (header-only, stb-style; no CMake upstream, so
# we grab its source tree and compile the single implementation TU ourselves).
set(VIBE_TAG "main" CACHE STRING "vibe git ref to fetch when no sibling exists")
if(NOT DEFINED VIBE_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../vibe/vibe.h")
  set(VIBE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vibe")
endif()
if(DEFINED VIBE_DIR)
  message(STATUS "hand: using vibe from ${VIBE_DIR}")
  set(vibe_SOURCE_DIR "${VIBE_DIR}")
else()
  message(STATUS "hand: fetching vibe (${VIBE_TAG}) from github.com/1ay1/vibe")
  FetchContent_Declare(vibe
    GIT_REPOSITORY https://github.com/1ay1/vibe.git
    GIT_TAG        ${VIBE_TAG})
  FetchContent_MakeAvailable(vibe)
endif()

# One TU compiles libvibe's implementation (stb-style header-only library).
add_library(vibe STATIC vendor/vibe_impl.c)
target_include_directories(vibe PUBLIC "${vibe_SOURCE_DIR}")

# plat — hand's type-theoretic platform abstraction (graphics/input/window/OS)
# behind capability concepts. Header-only contract for now; backends land later.
# Added after enable_testing() below so its tests register.

# ===========================================================================
# hand::platform — the native window/input/GL-context backends.
#
# THIS is where every window-system dependency lives: Wayland, X11, EGL,
# xkbcommon, xcb. toe (the engine) has none of these. Each backend models the
# `toe::Surface` concept and tail-calls `toe::run` — the engine's frame loop,
# instantiated once per backend on its concrete surface, so the hot path is
# monomorphic — no vtable.
# ===========================================================================
if(NOT WIN32)
  find_package(PkgConfig REQUIRED)
endif()

if(WIN32)
  # ---------------------------------------------------------------------------
  # Windows: Win32 + D3D11 (via sokol_gfx). Fully native — no GL, no ANGLE, no
  # translation layer. Process creation is ConPTY (conpty.cpp); fonts come from
  # DirectWrite; readiness is MsgWaitForMultipleObjectsEx (win_reactor.hpp).
  # ---------------------------------------------------------------------------
  add_library(hand-platform STATIC
    src/platform/backend.cpp
    src/platform/win/conpty.cpp
    src/platform/win/win32.cpp
    src/platform/win/fonts_win.cpp
    src/settings_panel.cpp
    src/help_panel.cpp
    src/workspace.cpp
    src/gui/update.cpp
    src/theme/themes.cpp)
  add_library(hand::platform ALIAS hand-platform)

  target_include_directories(hand-platform
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

  # d3d11/dxgi: the GPU path. dwrite: font discovery. shell32: open_url.
  target_link_libraries(hand-platform
    PUBLIC  toe::toe vibe
    PRIVATE d3d11 dxgi dwrite shell32 user32 gdi32)

  # Target Windows 10 1809+ (ConPTY needs it) and keep the min-version headers
  # consistent across every TU.
  target_compile_definitions(hand-platform PUBLIC _WIN32_WINNT=0x0A00 WINVER=0x0A00)

  target_compile_features(hand-platform PUBLIC cxx_std_23)
  target_compile_options(hand-platform PRIVATE -Wall -Wextra)
elseif(APPLE)
  # ---------------------------------------------------------------------------
  # macOS: Cocoa + Metal (via sokol_gfx). No OpenGL, no epoxy. forkpty lives in
  # posix_pty.
  # ---------------------------------------------------------------------------
  add_library(hand-platform STATIC
    src/platform/backend.cpp
    src/platform/posix_pty.cpp
    src/platform/cocoa/fonts_mac.cpp
    src/platform/cocoa/cocoa.mm
    src/settings_panel.cpp
    src/help_panel.cpp
    src/workspace.cpp
    src/gui/update.cpp
    src/actor/tab_actor.cpp
    src/theme/themes.cpp)
  add_library(hand::platform ALIAS hand-platform)

  target_include_directories(hand-platform
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

  # ARC on for the Objective-C++ TU. The Cocoa backend drives a CAMetalLayer +
  # sokol_gfx (Metal); the sokol header comes from toe's include/sokol.
  set_source_files_properties(src/platform/cocoa/cocoa.mm PROPERTIES
    COMPILE_OPTIONS "-fobjc-arc")
  find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
  find_library(METAL_FRAMEWORK Metal REQUIRED)
  find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
  find_library(CORETEXT_FRAMEWORK CoreText REQUIRED)

  target_link_libraries(hand-platform
    PUBLIC  toe::toe vibe
    PRIVATE ${COCOA_FRAMEWORK} ${METAL_FRAMEWORK} ${QUARTZCORE_FRAMEWORK} ${CORETEXT_FRAMEWORK})

  target_compile_features(hand-platform PUBLIC cxx_std_23)
  target_compile_options(hand-platform PRIVATE -Wall -Wextra)
else()
  # ---------------------------------------------------------------------------
  # Linux: Wayland + X11 + offscreen(EGL). THIS is where every window-system
  # dependency lives.
  # ---------------------------------------------------------------------------
  pkg_check_modules(EGL     REQUIRED IMPORTED_TARGET egl)
  pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client wayland-egl wayland-cursor)
  pkg_check_modules(XKB     REQUIRED IMPORTED_TARGET xkbcommon)
  pkg_check_modules(X11     REQUIRED IMPORTED_TARGET x11 x11-xcb xcb xkbcommon-x11)
  # Fully-native GL: sokol GLCORE calls the real libGL entry points directly, so
  # we link OpenGL::GL. No epoxy — sokol owns GL end-to-end.
  find_package(OpenGL REQUIRED)

  # Generate the xdg-shell protocol glue from wayland-protocols.
  find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
  pkg_get_variable(WLPROTO_DIR wayland-protocols pkgdatadir)
  set(XDG_XML "${WLPROTO_DIR}/stable/xdg-shell/xdg-shell.xml")
  set(GEN "${CMAKE_CURRENT_BINARY_DIR}/gen")
  file(MAKE_DIRECTORY "${GEN}")
  add_custom_command(
    OUTPUT "${GEN}/xdg-shell-client-protocol.h"
    COMMAND ${WAYLAND_SCANNER} client-header "${XDG_XML}" "${GEN}/xdg-shell-client-protocol.h"
    DEPENDS "${XDG_XML}" VERBATIM)
  add_custom_command(
    OUTPUT "${GEN}/xdg-shell-protocol.c"
    COMMAND ${WAYLAND_SCANNER} private-code "${XDG_XML}" "${GEN}/xdg-shell-protocol.c"
    DEPENDS "${XDG_XML}" VERBATIM)

  # text-input-unstable-v3: real IME (fcitx/ibus). Optional.
  set(TI_XML "${WLPROTO_DIR}/unstable/text-input/text-input-unstable-v3.xml")
  set(_ti_sources "")
  if(EXISTS "${TI_XML}")
    add_custom_command(
      OUTPUT "${GEN}/text-input-unstable-v3-client-protocol.h"
      COMMAND ${WAYLAND_SCANNER} client-header "${TI_XML}"
              "${GEN}/text-input-unstable-v3-client-protocol.h"
      DEPENDS "${TI_XML}" VERBATIM)
    add_custom_command(
      OUTPUT "${GEN}/text-input-unstable-v3-protocol.c"
      COMMAND ${WAYLAND_SCANNER} private-code "${TI_XML}"
              "${GEN}/text-input-unstable-v3-protocol.c"
      DEPENDS "${TI_XML}" VERBATIM)
    set(_ti_sources
      "${GEN}/text-input-unstable-v3-client-protocol.h"
      "${GEN}/text-input-unstable-v3-protocol.c")
  endif()

  add_library(hand-platform STATIC
    src/platform/backend.cpp
    src/platform/posix_pty.cpp
    src/platform/posix_url.cpp
    src/platform/wayland/wayland.cpp
    src/platform/x11/x11.cpp
    src/platform/offscreen.cpp
    src/platform/fonts_posix.cpp
    src/settings_panel.cpp
    src/help_panel.cpp
    src/workspace.cpp
    src/gui/update.cpp
    src/actor/tab_actor.cpp
    src/theme/themes.cpp
    "${GEN}/xdg-shell-client-protocol.h"
    "${GEN}/xdg-shell-protocol.c"
    ${_ti_sources})
  add_library(hand::platform ALIAS hand-platform)

  if(_ti_sources)
    target_compile_definitions(hand-platform PRIVATE TOE_HAVE_TEXT_INPUT_V3=1)
  endif()

  target_include_directories(hand-platform
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${GEN}")

  target_link_libraries(hand-platform
    PUBLIC  toe::toe vibe
    PRIVATE PkgConfig::EGL PkgConfig::WAYLAND PkgConfig::XKB PkgConfig::X11 OpenGL::GL)

  target_compile_features(hand-platform PUBLIC cxx_std_23)
  target_compile_options(hand-platform PRIVATE -Wall -Wextra)
endif()

# ===========================================================================
# hand — the executable. A thin shim: parse config, dispatch to a backend.
# ===========================================================================
add_executable(hand
  src/main.cpp
  src/config/config.cpp)
target_include_directories(hand PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(hand PRIVATE hand::platform toe::toe vibe)
if(NOT APPLE AND NOT WIN32)
  # sokol GLCORE links the real libGL for the Linux GL/EGL backends.
  find_package(OpenGL REQUIRED)
  target_link_libraries(hand PRIVATE OpenGL::GL)
endif()
if(WIN32)
  # Subsystem:windows — launching the terminal must not pop a console window
  # behind it (the whole point is that hand IS the console).
  target_link_options(hand PRIVATE -mwindows)
endif()
target_compile_options(hand PRIVATE -Wall -Wextra -Wpedantic)

# ===========================================================================
# hand-agent — the headless, GPU-free "terminal for AI" driver. Spawns a PTY,
# runs toe's VT engine with no renderer/window, and speaks line-delimited JSON
# on stdio so an agent (or an MCP shim) can drive interactive programs. It needs
# only toe + posix_pty (forkpty), so it builds on any POSIX host — no GL, no
# Cocoa. (A Windows port would swap posix_pty for a conpty sibling.)
# ===========================================================================
if(NOT WIN32)
  add_executable(hand-agent
    src/agent/driver.cpp
    src/platform/posix_pty.cpp)
  target_include_directories(hand-agent PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(hand-agent PRIVATE toe::toe)
  target_compile_features(hand-agent PRIVATE cxx_std_23)
  target_compile_options(hand-agent PRIVATE -Wall -Wextra)

  # hand-agent-mcp — the same driver, exposed as a stdio MCP server so it drops
  # straight into Claude Code / Codex / OpenCode / any MCP client.
  add_executable(hand-agent-mcp
    src/agent/mcp.cpp
    src/platform/posix_pty.cpp)
  target_include_directories(hand-agent-mcp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(hand-agent-mcp PRIVATE toe::toe)
  target_compile_features(hand-agent-mcp PRIVATE cxx_std_23)
  target_compile_options(hand-agent-mcp PRIVATE -Wall -Wextra)
endif()

# ===========================================================================
# Renderer tests. These need a live GL context (from hand's offscreen backend)
# to exercise toe's GPU renderer, so they live here rather than in toe (which
# no longer links any window system). They SKIP (exit 77) when EGL is absent.
# ===========================================================================
option(HAND_BUILD_TESTS "Build hand's tests" ON)
if(HAND_BUILD_TESTS)
  enable_testing()

  # plat: the platform abstraction (header-only contract + its type-theory
  # tests, incl. compile-fail cases). Added here so its add_test() registers
  # under the same enable_testing() scope.
  add_subdirectory(plat)

  # --- GL renderer tests (Linux only) --------------------------------------
  # These need the EGL offscreen backend for a headless GL context, which
  # neither macOS (no EGL) nor Windows (D3D11, no offscreen GL backend)
  # provides. The logic tests below run everywhere.
  if(NOT APPLE AND NOT WIN32)
    add_executable(render_test tests/render_test.cpp)
    target_include_directories(render_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(render_test PRIVATE hand::platform toe::toe OpenGL::GL)
    add_test(NAME render COMMAND render_test)

    add_executable(cache_test tests/cache_test.cpp)
    target_include_directories(cache_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(cache_test PRIVATE hand::platform toe::toe OpenGL::GL)
    add_test(NAME cache COMMAND cache_test)

    add_executable(atlas_grow_test tests/atlas_grow_test.cpp)
    target_include_directories(atlas_grow_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(atlas_grow_test PRIVATE hand::platform toe::toe OpenGL::GL)
    add_test(NAME atlas_grow COMMAND atlas_grow_test)

    # Window opacity: verifies bg alpha reaches the framebuffer (regression guard
    # for "opacity only works on the settings pane").
    add_executable(opacity_test tests/opacity_test.cpp)
    target_include_directories(opacity_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(opacity_test PRIVATE hand::platform toe::toe OpenGL::GL)
    add_test(NAME opacity COMMAND opacity_test)

    # Font rebuild leak: repeatedly rebuilding the Renderer (font/ligature change)
    # must not leak GL shaders ("shader pool exhausted", sokol id 154).
    add_executable(font_rebuild_test tests/font_rebuild_test.cpp)
    target_include_directories(font_rebuild_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(font_rebuild_test PRIVATE hand::platform toe::toe OpenGL::GL)
    add_test(NAME font_rebuild COMMAND font_rebuild_test)

    set_tests_properties(render cache atlas_grow opacity font_rebuild PROPERTIES ENVIRONMENT "TOE_HEADLESS=1")
  endif()

  # --- platform-neutral logic tests (all OSes, incl. macOS) ----------------

  # Config round-trip: no GL needed — pure config/settings logic. config.cpp
  # (the VIBE parse/save) lives in the hand exe, so compile it in directly.
  add_executable(config_roundtrip_test tests/config_roundtrip_test.cpp src/config/config.cpp)
  target_include_directories(config_roundtrip_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(config_roundtrip_test PRIVATE hand::platform vibe)
  add_test(NAME config_roundtrip COMMAND config_roundtrip_test)

  # Theme table + terminal->UI derivation. Pulls config.cpp for the round-trip
  # and hand::platform for the theme table (themes.cpp).
  add_executable(themes_test tests/themes_test.cpp src/config/config.cpp)
  target_include_directories(themes_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(themes_test PRIVATE hand::platform vibe)
  add_test(NAME themes COMMAND themes_test)

  # Glyph IMGUI toolkit: header-only, paints into a plain cell Buffer (no GL).
  # Exercises the dropdown filter/select flow.
  add_executable(glyph_test tests/glyph_test.cpp)
  target_include_directories(glyph_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(glyph_test PRIVATE toe)
  add_test(NAME glyph COMMAND glyph_test)

  # Activity Tabs status brain: pure TabModel logic (auto-label, done-attention,
  # unseen dot). Header-only, no GL / no engine — synthetic TabSignals in.
  add_executable(tab_model_test tests/tab_model_test.cpp)
  target_include_directories(tab_model_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  add_test(NAME tab_model COMMAND tab_model_test)

  # Tab container: Zipper<T> as pure algebra (non-empty + valid-focus invariants
  # under insert/close/navigate). Move-only element, no engine.
  add_executable(zipper_test tests/zipper_test.cpp)
  target_include_directories(zipper_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  add_test(NAME zipper COMMAND zipper_test)

  # Cross-thread Mailbox: many senders / one receiver, exactly-once delivery of
  # move-only messages. The sole thread-boundary surface for the actor model.
  add_executable(mailbox_test tests/mailbox_test.cpp)
  target_include_directories(mailbox_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  find_package(Threads REQUIRED)
  target_link_libraries(mailbox_test PRIVATE Threads::Threads)
  add_test(NAME mailbox COMMAND mailbox_test)

  # The GUI Elm reducer: pure gui_update(GuiModel, GuiMsg) -> GuiCmds. Whole GUI
  # control flow (tab lifecycle/focus/routing/quit) proven with no threads/GL.
  add_executable(gui_update_test tests/gui_update_test.cpp src/gui/update.cpp)
  target_include_directories(gui_update_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(gui_update_test PRIVATE toe::toe)
  add_test(NAME gui_update COMMAND gui_update_test)

  # The SHARED chord classifier: one place decides key bindings; proving it here
  # means every backend gets identical, correct chords (no per-backend drift).
  add_executable(chord_test tests/chord_test.cpp)
  target_include_directories(chord_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(chord_test PRIVATE hand::platform toe::toe)
  add_test(NAME chord COMMAND chord_test)

  # Render-decision core: FrameGate (frame-skip key + time-derived frame index).
  # Pure, no engine/threads — the efficiency logic proven in isolation.
  add_executable(frame_gate_test tests/frame_gate_test.cpp)
  target_include_directories(frame_gate_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  add_test(NAME frame_gate COMMAND frame_gate_test)

  # Animation authority: Animator (deadline aggregation + time-derived frame).
  # Pure, lock-free, no engine — the animation timing proven in isolation.
  add_executable(animator_test tests/animator_test.cpp)
  target_include_directories(animator_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  add_test(NAME animator COMMAND animator_test)

  # Command-minimap flyout: the rail-hover -> selected-command rule (pure).
  add_executable(command_flyout_test tests/command_flyout_test.cpp)
  target_include_directories(command_flyout_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(command_flyout_test PRIVATE toe)
  add_test(NAME command_flyout COMMAND command_flyout_test)

  # Config watcher: pure filesystem-watch logic (inotify on Linux, kqueue on
  # macOS/BSD), no GL / no deps. Guards live config hot-reload on every OS.
  add_executable(config_watch_test tests/config_watch_test.cpp)
  target_include_directories(config_watch_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  add_test(NAME config_watch COMMAND config_watch_test)

  # Smart font discovery: fc-list-style dir globbing on POSIX, Core Text on
  # macOS (fonts_mac.cpp). Environment-dependent; returns 77 (skip) on a
  # font-less box.
  add_executable(fonts_test tests/fonts_test.cpp)
  target_include_directories(fonts_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(fonts_test PRIVATE hand::platform vibe)
  add_test(NAME fonts COMMAND fonts_test)
endif()
