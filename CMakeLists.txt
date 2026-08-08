# SPDX-License-Identifier: LGPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)
project(hand VERSION 0.1 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- libgvte -----------------------------------------------------------------
# hand is a thin frontend over libgvte. Use a sibling ../gvte checkout when one
# exists (local development), otherwise fetch it from GitHub. Override the
# source with -DGVTE_DIR=/path or the tag with -DGVTE_TAG=<ref>.
set(GVTE_TAG "main" CACHE STRING "gvte git ref to fetch when no sibling exists")
if(NOT DEFINED GVTE_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../gvte/CMakeLists.txt")
  set(GVTE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../gvte")
endif()

if(DEFINED GVTE_DIR)
  message(STATUS "hand: using libgvte from ${GVTE_DIR}")
  add_subdirectory("${GVTE_DIR}" "${CMAKE_BINARY_DIR}/gvte")
else()
  message(STATUS "hand: fetching libgvte (${GVTE_TAG}) from github.com/1ay1/gvte")
  include(FetchContent)
  FetchContent_Declare(
    gvte
    GIT_REPOSITORY https://github.com/1ay1/gvte.git
    GIT_TAG        ${GVTE_TAG}
  )
  FetchContent_MakeAvailable(gvte)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)

add_executable(hand main.cpp vendor/vibe_impl.c)
target_include_directories(hand PRIVATE vendor)
target_link_libraries(hand PRIVATE gvte::gvte PkgConfig::EPOXY)
target_compile_options(hand PRIVATE -Wall -Wextra -Wpedantic)
