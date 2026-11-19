# hand — the terminal
#
#   app → mayag (UI) → jaal (runtime)            toe (VT engine) joins later
#
# hand names mayag only. jaal is mayag's implementation detail and is never
# included or linked by anything in this tree; `tests/layering` enforces it.
cmake_minimum_required(VERSION 3.24)
project(hand VERSION 0.1.0 LANGUAGES CXX
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

# ── hand::ui — every widget, header-only, on top of mayag only ───────────
add_library(hand_ui INTERFACE)
add_library(hand::ui ALIAS hand_ui)
target_include_directories(hand_ui INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hand_ui INTERFACE mayag::mayag)

# ── the app ───────────────────────────────────────────────────────────────
add_executable(hand src/main.cpp)
target_link_libraries(hand PRIVATE hand::ui)
target_compile_options(hand PRIVATE ${HAND_WARNINGS})

if(HAND_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
