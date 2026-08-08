# SPDX-License-Identifier: LGPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)
project(hand VERSION 0.1 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# --- vendored dependencies ---------------------------------------------------
# hand pulls both of its dependencies the same way: a sibling checkout when one
# exists (local development), otherwise fetched from GitHub. Override a source
# with -D<DEP>_DIR=/path or a ref with -D<DEP>_TAG=<git-ref>.

# libgvte — the terminal library hand is a thin frontend over.
set(GVTE_TAG "main" CACHE STRING "gvte git ref to fetch when no sibling exists")
if(NOT DEFINED GVTE_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../gvte/CMakeLists.txt")
  set(GVTE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../gvte")
endif()
if(DEFINED GVTE_DIR)
  message(STATUS "hand: using libgvte from ${GVTE_DIR}")
  set(GVTE_BUILD_PLATFORM ON  CACHE BOOL "" FORCE)  # hand uses the Linux backends
  set(GVTE_BUILD_DEMO     OFF CACHE BOOL "" FORCE)
  set(GVTE_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
  add_subdirectory("${GVTE_DIR}" "${CMAKE_BINARY_DIR}/gvte")
else()
  message(STATUS "hand: fetching libgvte (${GVTE_TAG}) from github.com/1ay1/gvte")
  FetchContent_Declare(gvte
    GIT_REPOSITORY https://github.com/1ay1/gvte.git
    GIT_TAG        ${GVTE_TAG})
  FetchContent_MakeAvailable(gvte)
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

find_package(PkgConfig REQUIRED)
pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)

add_executable(hand main.cpp)
target_link_libraries(hand PRIVATE gvte::core gvte::platform vibe PkgConfig::EPOXY)
target_compile_options(hand PRIVATE -Wall -Wextra -Wpedantic)
