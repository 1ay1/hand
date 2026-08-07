# SPDX-License-Identifier: LGPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)
project(hand VERSION 0.1 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Pull in libgvte from the sibling directory.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../gvte ${CMAKE_BINARY_DIR}/gvte)

find_package(PkgConfig REQUIRED)
pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)

add_executable(hand main.cpp vendor/vibe_impl.c)
target_include_directories(hand PRIVATE vendor)
target_link_libraries(hand PRIVATE gvte::gvte PkgConfig::EPOXY)
target_compile_options(hand PRIVATE -Wall -Wextra -Wpedantic)
