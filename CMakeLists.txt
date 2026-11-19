# hand — a terminal, like foot
#
#   app → mayag (UI) → jaal (runtime)        toe (VT engine)
#
# hand names mayag only. jaal is mayag's implementation detail and is never
# included or linked by anything in this tree; `tests/layering` enforces it.
cmake_minimum_required(VERSION 3.24)
project(hand VERSION 0.1.0 LANGUAGES C CXX
        DESCRIPTION "A terminal you set up by looking at it")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HAND_BUILD_TESTS "Build hand's headless test suite" ON)

# ── mayag, as a library: no examples, no tests of its own ─────────────────
set(MAYAG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MAYAG_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/mayag/CMakeLists.txt")
    message(FATAL_ERROR "hand: external/mayag is missing. Run: git submodule update --init --recursive")
endif()
add_subdirectory(external/mayag)

# Same discipline as mayag itself: a narrowing float is a pixel bug.
if(MSVC)
    set(HAND_WARNINGS /W4 /permissive-)
else()
    set(HAND_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
                      -Wnon-virtual-dtor -Wno-missing-field-initializers)
endif()

# ── toe: the VT engine (links nothing but libstdc++) ─────────────────────
set(TOE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TOE_INSTALL     OFF CACHE BOOL "" FORCE)
set(TOE_LTO         OFF CACHE BOOL "" FORCE)   # keep incremental links fast
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/toe/CMakeLists.txt")
    message(FATAL_ERROR "hand: external/toe is missing. Run: git submodule update --init --recursive")
endif()
add_subdirectory(external/toe)

# ── vibe: the config format (https://github.com/1ay1/vibe), one C file ────
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/vibe/vibe.h")
    message(FATAL_ERROR "hand: external/vibe is missing. Run: git submodule update --init --recursive")
endif()
add_library(vibe STATIC external/vibe/vibe.c)
target_include_directories(vibe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/external/vibe)
set_target_properties(vibe PROPERTIES C_STANDARD 11)

# ── the app: small translation units, so an edit rebuilds one of them ─────
#   term.cpp  the only TU that includes toe
#   pty.cpp   the only TU that touches POSIX ptys
#   shell.cpp shell integration (OSC 133/7 injected into fish/zsh/bash)
#   app.cpp   update / input / view (mayag)
#   settings.cpp  the settings table, hand.vibe, and the 913-scheme catalogue
#   main.cpp  fonts, config, modes
add_executable(hand src/main.cpp src/app.cpp src/term.cpp src/pty.cpp src/settings.cpp src/shell.cpp)
target_include_directories(hand PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hand PRIVATE mayag::mayag toe::toe vibe util)
target_compile_options(hand PRIVATE ${HAND_WARNINGS})

if(HAND_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
