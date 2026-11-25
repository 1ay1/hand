# hand — a terminal, like foot
#
#   app → mayag (UI) → jaal (runtime)        toe (VT engine)
#
# hand names mayag only. jaal is mayag's implementation detail and is never
# included or linked by anything in this tree; `tests/layering` enforces it.
cmake_minimum_required(VERSION 3.24)
project(hand VERSION 0.1.0 LANGUAGES C CXX
        DESCRIPTION "A terminal you set up by looking at it")

# Build types: Dev (the default) is for the edit-build-test loop. hand's own
# files compile at -O0 (a header edit rebuilds ~45 of them: optimising each
# was ~80% of the time), while everything hot stays at -O2: toe, and mayag's
# code -- compiled ONCE into the PCH's object (see HAND_PCH), which every hand
# file links to. So the app still runs at speed. Release / RelWithDebInfo
# are for shipping and the benchmarks (--bench, --flood): build-rel.
# (CMake caches an EMPTY flags entry for a build type it doesn't know, so an
# empty value is filled in rather than respected.)
foreach(lang C CXX)
    if(NOT CMAKE_${lang}_FLAGS_DEV)
        set(CMAKE_${lang}_FLAGS_DEV "-O0 -g1 -DNDEBUG" CACHE STRING "${lang} flags for Dev builds" FORCE)
    endif()
endforeach()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Dev CACHE STRING "" FORCE)
endif()
# Never recompile what was compiled before, and link fast, when the tools exist.
find_program(HAND_CCACHE ccache)
if(HAND_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    set(CMAKE_C_COMPILER_LAUNCHER ${HAND_CCACHE})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${HAND_CCACHE})
    # No size cap: ccache's default 5 GB fills with ONE Dev + Release pair of
    # trees (mayag's PCH objects are large), and then evicts what the next
    # build needs -- a 10% hit rate was measured on a full cache. Unlimited,
    # once, at configure (HAND_CCACHE_SIZE=20G to cap it instead).
    set(HAND_CCACHE_SIZE "0" CACHE STRING "ccache max_size for this machine (0 = unlimited)")
    execute_process(COMMAND ${HAND_CCACHE} --max-size=${HAND_CCACHE_SIZE} OUTPUT_QUIET ERROR_QUIET)
endif()
find_program(HAND_MOLD mold)
find_program(HAND_LLD NAMES ld.lld lld)
if(HAND_MOLD AND NOT CMAKE_LINKER_TYPE)
    set(CMAKE_LINKER_TYPE MOLD)
elseif(HAND_LLD AND NOT CMAKE_LINKER_TYPE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    # No mold (Windows, most macOS): lld. GNU ld spent 25 minutes and 5 GB
    # linking the clang Dev build's PCH code on Windows; lld takes seconds.
    set(CMAKE_LINKER_TYPE LLD)
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
if(CMAKE_BUILD_TYPE STREQUAL "Dev")
    target_compile_options(toe PRIVATE -O2)   # the VT parser is the hot path; it is rebuilt rarely
endif()

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
#   hand_app     src/app      the program: update + input, one file per domain; src/app/fx its effects
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
hand_layer(hand_app    DIRS app app/fx view LINK hand_config hand_ssh)

file(GLOB HAND_CLI CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/cli/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/cli/checks/*.cpp)
add_executable(hand src/main.cpp ${HAND_CLI})   # main + its run modes (src/cli)
target_link_libraries(hand PRIVATE hand_app)
target_compile_options(hand PRIVATE ${HAND_WARNINGS})

# ── build speed: one precompiled header ──────────────────────────────────────────────────────────
# mayag is header-only and big: without this, every app/view/cli file parses
# and instantiates all of it again (60-120 s each). The PCH holds ONLY what
# hand doesn't edit -- mayag, toe, the standard library -- so changing a hand
# header never invalidates it; it rebuilds only when mayag or toe change.
option(HAND_PCH "Precompile mayag/toe/std for hand's app and cli" ON)
set(HAND_PCH_HEADERS
    mayag/mayag.hpp mayag/core/event.hpp
    mayag/widget/file_pane.hpp mayag/widget/marks.hpp mayag/widget/resize_hud.hpp
    mayag/widget/scrollback_bar.hpp mayag/widget/settings_panel.hpp mayag/widget/ssh_forward.hpp
    mayag/widget/ssh.hpp mayag/widget/ssh_manager.hpp mayag/widget/tab_strip.hpp
    mayag/widget/term_dialog.hpp mayag/widget/term_search.hpp mayag/widget/toast.hpp
    mayag/widget/terminal_view.hpp mayag/widget/terminal/cursor_fx.hpp mayag/widget/terminal/history.hpp
    mayag/widget/terminal/overlays.hpp mayag/widget/terminal/remote.hpp mayag/widget/terminal/window.hpp
    toe/input/keymap.hpp toe/term/update.hpp
    algorithm array chrono cmath cstdint cstdio cstdlib cstring filesystem
    map memory optional span string string_view thread unordered_map utility variant vector)
if(HAND_PCH AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # clang: the PCH also carries mayag's CODE (-fpch-codegen), compiled and
    # optimised ONCE into hand_pch_code.o; every hand file links to it instead
    # of re-optimising its own copy (that was ~80% of a 60-120 s file).
    set(_hdr ${CMAKE_CURRENT_BINARY_DIR}/hand_pch/hand_pch.hxx)
    set(_body "")
    foreach(h ${HAND_PCH_HEADERS})
        string(APPEND _body "#include <${h}>\n")
    endforeach()
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/hand_pch)
    file(CONFIGURE OUTPUT ${_hdr} CONTENT "${_body}")
    add_library(hand_pch OBJECT ${_hdr})
    set_source_files_properties(${_hdr} PROPERTIES LANGUAGE CXX)
    target_link_libraries(hand_pch PRIVATE mayag::mayag toe::toe)
    target_compile_options(hand_pch PRIVATE -x c++-header -fpch-instantiate-templates -fpch-codegen
                                            "SHELL:-Xclang -fno-pch-timestamp")
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _cfg)
    separate_arguments(_opt UNIX_COMMAND "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_cfg}}")
    if(_cfg STREQUAL "DEV")
        list(APPEND _opt -O2)   # mayag's code, compiled once: always optimised (the last -O wins)
    endif()
    set(_pch ${CMAKE_CURRENT_BINARY_DIR}/hand_pch/hand.pch)
    set(_code ${CMAKE_CURRENT_BINARY_DIR}/hand_pch/hand_pch_code.o)
    add_custom_command(
        OUTPUT ${_code}
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_OBJECTS:hand_pch> ${_pch}
        COMMAND ${CMAKE_CXX_COMPILER_LAUNCHER} ${CMAKE_CXX_COMPILER} ${_opt} -c ${_pch} -o ${_code}
        DEPENDS hand_pch $<TARGET_OBJECTS:hand_pch>
        COMMENT "hand: compiling the PCH's shared code once"
        VERBATIM)
    add_custom_target(hand_pch_code DEPENDS ${_code})
    set_source_files_properties(${_code} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    foreach(t hand_app hand)
        add_dependencies(${t} hand_pch_code)
        target_compile_options(${t} PRIVATE "SHELL:-include-pch $<TARGET_OBJECTS:hand_pch>")
    endforeach()
    target_sources(hand PRIVATE ${_code})
    # -fpch-codegen emits EVERY inline in the PCH's headers, used or not, so
    # the object links what those inlines import: on Windows, libstdc++'s
    # std::print terminal hooks (stdc++exp). GNU ld let unused references
    # slide, lld does not. (commctrl's DPA_/DSA_ inlines are kept out at the
    # source: mayag defines NO_COMMCTRL_DA.)
    if(MAYAG_SYSTEM STREQUAL "windows" AND NOT MSVC)
        target_link_libraries(hand PRIVATE stdc++exp)
    endif()
    # ccache caches PCH users only with these
    if(CMAKE_CXX_COMPILER_LAUNCHER MATCHES "ccache")
        set_property(TARGET hand_pch hand_app hand PROPERTY CXX_COMPILER_LAUNCHER
                     ${CMAKE_COMMAND} -E env "CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime" ccache)
    endif()
elseif(HAND_PCH)
    list(TRANSFORM HAND_PCH_HEADERS PREPEND "<")
    list(TRANSFORM HAND_PCH_HEADERS APPEND ">")
    target_precompile_headers(hand_app PRIVATE ${HAND_PCH_HEADERS})
    target_precompile_headers(hand REUSE_FROM hand_app)
endif()

# ── the OS seam ───────────────────────────────────────────────────────────────────────────
# The pty is the only part of hand that is not portable C++: forkpty lives in
# libutil on glibc, and on Windows it is ConPTY (kernel32) plus shlwapi for
# the host-glob fallback. It all belongs to hand_core, and travels up from it.
# Which OS: mayag's one answer (MAYAG_SYSTEM: apple | windows | freedesktop |
# other), never WIN32 / APPLE again here.
if(MAYAG_SYSTEM STREQUAL "windows")
    target_link_libraries(hand_core PUBLIC shlwapi)
    # The manifest + version block (res/windows): no stray console from
    # Explorer, per-monitor DPI before the first window, and the UTF-8 code
    # page -- every std::string in hand is UTF-8, and with this so are the
    # narrow Win32 / CRT calls (getenv, *A APIs) a non-ASCII user name reaches.
    enable_language(RC)
    target_sources(hand PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/res/windows/hand.rc)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/res/windows/hand.rc PROPERTIES
                                OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/res/windows/hand.manifest)
    if(MSVC)
        target_link_options(hand PRIVATE /MANIFEST:NO)   # ours is in the .rc; no second, generated one
    endif()
    # ConPTY needs Windows 10 1809 (the API that makes a real terminal
    # possible at all); say so rather than inheriting MinGW's older default.
    # NOGDI is deliberately NOT set: mayag's Win32 backend blits through GDI.
    target_compile_definitions(hand_core PUBLIC _WIN32_WINNT=0x0A00 WINVER=0x0A00
                                                WIN32_LEAN_AND_MEAN NOMINMAX)
    # Microsoft's current pseudoconsole host, embedded: OpenConsole.exe from the
    # Microsoft.Windows.Console.ConPTY package (MIT; what Windows Terminal and
    # WezTerm ship). The inbox conhost repaints the whole viewport on every
    # resize, reflowed by its own rules -- a window drag doubled lines in the
    # scrollback and cut the head off a line wrapped across the top. hand stays
    # one file: the host rides in hand.exe as a resource and src/core/pty_win.cpp
    # launches it itself (no conpty.dll). Fetched once at configure, pinned by
    # hash; without it hand falls back to the system conhost.
    set(HAND_CONPTY_VERSION "1.25.261109001" CACHE STRING "Microsoft.Windows.Console.ConPTY embedded in hand.exe ('' = none)")
    set(HAND_CONPTY_SHA256 "3D77F535DF848E44225C2A8912AC844FECB23F3057732B639F3034B587496F09" CACHE STRING "SHA-256 of that .nupkg")
    if(HAND_CONPTY_VERSION)
        set(_cp ${CMAKE_BINARY_DIR}/_deps/conpty-${HAND_CONPTY_VERSION})
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
            set(_arch arm64)
        else()
            set(_arch x64)
        endif()
        set(_host ${_cp}/build/native/runtimes/${_arch}/OpenConsole.exe)
        if(NOT EXISTS ${_host})
            # TLS_VERIFY OFF for intercepting proxies; the hash is the check.
            file(DOWNLOAD https://www.nuget.org/api/v2/package/Microsoft.Windows.Console.ConPTY/${HAND_CONPTY_VERSION}
                 ${_cp}.zip STATUS _st TLS_VERIFY OFF EXPECTED_HASH SHA256=${HAND_CONPTY_SHA256})
            list(GET _st 0 _code)
            if(_code EQUAL 0)
                file(ARCHIVE_EXTRACT INPUT ${_cp}.zip DESTINATION ${_cp})
            else()
                message(WARNING "hand: could not fetch ConPTY ${HAND_CONPTY_VERSION} (${_st}); the system conhost will be used")
            endif()
        endif()
        if(EXISTS ${_host})
            file(WRITE ${CMAKE_BINARY_DIR}/conpty.rc.in "OPENCONSOLE RCDATA \"${_host}\"\n")
            configure_file(${CMAKE_BINARY_DIR}/conpty.rc.in ${CMAKE_BINARY_DIR}/conpty.rc COPYONLY)
            target_sources(hand PRIVATE ${CMAKE_BINARY_DIR}/conpty.rc)
            set_source_files_properties(${CMAKE_BINARY_DIR}/conpty.rc PROPERTIES OBJECT_DEPENDS ${_host})
            set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/core/pty_win.cpp PROPERTIES
                                        COMPILE_DEFINITIONS "HAND_CONPTY_VERSION=L\"${HAND_CONPTY_VERSION}\"")
        endif()
    endif()
    # (A fully static MinGW link was measured: +15 ms to first text, as the
    # bigger image pages in cold, while the shared runtime DLLs are already
    # resident. So the runtime stays dynamic.)
elseif(NOT MAYAG_SYSTEM STREQUAL "apple")
    target_link_libraries(hand_core PUBLIC util)   # glibc forkpty
endif()
# Release ships no symbols: an 8.9 MB unstripped hand.exe took 11 ms longer
# to launch than the stripped 5.1 MB one (on-access scanning reads the whole
# image). RelWithDebInfo keeps them for profiling.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MAYAG_SYSTEM STREQUAL "apple")
    target_link_options(hand PRIVATE $<$<CONFIG:Release,MinSizeRel>:-s>)
endif()

if(HAND_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
