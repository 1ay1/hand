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

# ── the app, by layer ── each a library that links only the layers below it ──
#
#   hand_core    src/core     the terminal itself: toe (term), the pty, procs,
#                             shell integration, the OS seam, vocabulary types
#   hand_config  src/config   the settings table, hand.vibe, the scheme catalogue
#   hand_ssh     src/ssh      hosts: ~/.ssh/config, known_hosts, hosts.vibe, sftp
#   hand_app     src/app      the program: update + input, one file per domain
#                src/view     the widget tree (App::view), mayag widgets only
#   hand         src/main.cpp fonts, config, picks a mode
#                src/cli      the scripted modes: self-test, shots, bench, repros
#
#   core ◀─ config ◀─┐
#   core ◀─ ssh    ◀─┴─ app ◀─ hand          (and every layer may use mayag/toe)
#
# The headers share one include/ root, so the link graph alone cannot stop
# `#include <hand/app/...>` in core; tests/layering.cmake checks every
# include against this table and fails the build's test run on a violation.
function(hand_layer name)
    cmake_parse_arguments(L "" "" "DIRS;LINK" ${ARGN})
    set(srcs)
    foreach(d ${L_DIRS})
        file(GLOB s CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/${d}/*.cpp)
        list(APPEND srcs ${s})
    endforeach()
    add_library(${name} STATIC ${srcs})
    target_include_directories(${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(${name} PUBLIC ${L_LINK})
    target_compile_options(${name} PRIVATE ${HAND_WARNINGS})
endfunction()

hand_layer(hand_core   DIRS core   LINK mayag::mayag toe::toe)
hand_layer(hand_config DIRS config LINK hand_core vibe)
hand_layer(hand_ssh    DIRS ssh    LINK hand_core vibe)
hand_layer(hand_app    DIRS app view LINK hand_config hand_ssh)

file(GLOB HAND_CLI CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/cli/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/cli/checks/*.cpp)
add_executable(hand src/main.cpp ${HAND_CLI})   # main + its run modes (src/cli)
target_link_libraries(hand PRIVATE hand_app)
target_compile_options(hand PRIVATE ${HAND_WARNINGS})

# ── the OS seam ───────────────────────────────────────────────────────────────────────────
# The pty is the only part of hand that is not portable C++: forkpty lives in
# libutil on glibc, and on Windows it is ConPTY (kernel32) plus shlwapi for
# the host-glob fallback. It all belongs to hand_core, and travels up from it.
if(WIN32)
    target_link_libraries(hand_core PUBLIC shlwapi)
    # ConPTY needs Windows 10 1809 (the API that makes a real terminal
    # possible at all); say so rather than inheriting MinGW's older default.
    # NOGDI is deliberately NOT set: mayag's Win32 backend blits through GDI.
    target_compile_definitions(hand_core PUBLIC _WIN32_WINNT=0x0A00 WINVER=0x0A00
                                                WIN32_LEAN_AND_MEAN NOMINMAX)
    # (A fully static MinGW link was measured: +15 ms to first text, as the
    # bigger image pages in cold, while the shared runtime DLLs are already
    # resident. So the runtime stays dynamic.)
elseif(NOT APPLE)
    target_link_libraries(hand_core PUBLIC util)   # glibc forkpty
endif()
# Release ships no symbols: an 8.9 MB unstripped hand.exe took 11 ms longer
# to launch than the stripped 5.1 MB one (on-access scanning reads the whole
# image). RelWithDebInfo keeps them for profiling.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
    target_link_options(hand PRIVATE $<$<CONFIG:Release,MinSizeRel>:-s>)
endif()

if(HAND_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
