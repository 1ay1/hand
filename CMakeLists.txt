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

# ── the app, by layer (each a folder; the UI is only ever mayag widgets) ────
#   core/    the terminal itself: toe (term), the pty, shell integration
#   config/  the settings table, hand.vibe, the 913-scheme catalogue
#   ssh/     hosts: ~/.ssh/config, known_hosts, hosts.vibe, the ssh argv
#   app/     the program: update and input, one file per concern
#   view/    the widget tree: model -> mayag widget props, composed
#   main.cpp fonts, config, modes (window / --png / --headless)
file(GLOB HAND_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/config/*.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/src/ssh/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/app/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/view/*.cpp)
add_executable(hand src/main.cpp ${HAND_SOURCES})
target_include_directories(hand PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hand PRIVATE mayag::mayag toe::toe vibe)
target_compile_options(hand PRIVATE ${HAND_WARNINGS})

# ── the OS seam ──────────────────────────────────────────────────────────
# The pty is the only part of hand that is not portable C++: forkpty lives in
# libutil on glibc, and on Windows it is ConPTY (kernel32) plus shlwapi for
# the host-glob fallback. src/core/platform_{posix,win}.cpp are guarded
# internally, so the glob above can stay dumb.
if(WIN32)
    target_link_libraries(hand PRIVATE shlwapi)
    # ConPTY needs Windows 10 1809 (the API that makes a real terminal
    # possible at all); say so rather than inheriting MinGW's older default.
    target_compile_definitions(hand PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00
                                            WIN32_LEAN_AND_MEAN NOMINMAX NOGDI)
elseif(NOT APPLE)
    target_link_libraries(hand PRIVATE util)   # glibc forkpty
endif()

if(HAND_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
