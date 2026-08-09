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

# ===========================================================================
# hand::platform — the native window/input/GL-context backends.
#
# THIS is where every window-system dependency lives: Wayland, X11, EGL,
# xkbcommon, xcb. toe (the engine) has none of these. Each backend models the
# `toe::Surface` concept and tail-calls `toe::run` — the engine's frame loop,
# instantiated once per backend on its concrete surface, so the hot path is
# monomorphic — no vtable.
# ===========================================================================
find_package(PkgConfig REQUIRED)

if(APPLE)
  # ---------------------------------------------------------------------------
  # macOS: Cocoa + Metal (via sokol_gfx). No OpenGL, no epoxy. forkpty lives in
  # posix_pty.
  # ---------------------------------------------------------------------------
  add_library(hand-platform STATIC
    src/platform/backend.cpp
    src/platform/posix_pty.cpp
    src/platform/cocoa/fonts_mac.cpp
    src/platform/cocoa/cocoa.mm
    src/settings_panel.cpp)
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
  pkg_check_modules(EPOXY   REQUIRED IMPORTED_TARGET epoxy)
  pkg_check_modules(EGL     REQUIRED IMPORTED_TARGET egl)
  pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client wayland-egl wayland-cursor)
  pkg_check_modules(XKB     REQUIRED IMPORTED_TARGET xkbcommon)
  pkg_check_modules(X11     REQUIRED IMPORTED_TARGET x11 x11-xcb xcb xkbcommon-x11)

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
    PRIVATE PkgConfig::EGL PkgConfig::WAYLAND PkgConfig::XKB PkgConfig::X11 PkgConfig::EPOXY)

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
if(NOT APPLE)
  # epoxy is only needed by the Linux GL/EGL backends.
  target_link_libraries(hand PRIVATE PkgConfig::EPOXY)
endif()
target_compile_options(hand PRIVATE -Wall -Wextra -Wpedantic)

# ===========================================================================
# Renderer tests. These need a live GL context (from hand's offscreen backend)
# to exercise toe's GPU renderer, so they live here rather than in toe (which
# no longer links any window system). They SKIP (exit 77) when EGL is absent.
# ===========================================================================
option(HAND_BUILD_TESTS "Build hand's GL renderer tests" ON)
# The renderer tests need the EGL offscreen backend for a headless GL context,
# which macOS doesn't provide (no EGL). Skip them on Apple.
if(HAND_BUILD_TESTS AND NOT APPLE)
  enable_testing()
  add_executable(render_test tests/render_test.cpp)
  target_include_directories(render_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(render_test PRIVATE hand::platform toe::toe PkgConfig::EPOXY)
  add_test(NAME render COMMAND render_test)

  add_executable(cache_test tests/cache_test.cpp)
  target_include_directories(cache_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(cache_test PRIVATE hand::platform toe::toe PkgConfig::EPOXY)
  add_test(NAME cache COMMAND cache_test)

  set_tests_properties(render cache PROPERTIES ENVIRONMENT "TOE_HEADLESS=1")
endif()
