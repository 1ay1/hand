# SPDX-License-Identifier: LGPL-2.0-or-later
#
# plat — hand's type-theoretic platform abstraction (graphics + input + window +
# OS services) behind capability concepts. Lives inside the hand repo.
#
# The contract layer (concepts + the GPU resource algebra) is header-only, so
# `plat` is an INTERFACE target; concrete backends compile into it later.

add_library(plat INTERFACE)
add_library(plat::plat ALIAS plat)
target_include_directories(plat INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(plat INTERFACE cxx_std_23)

# ---------------------------------------------------------------------------
# plat-gfx — the concrete sokol_gfx-backed Gpu (SokolGpu). Linux/GLCORE for now
# (the sokol IMPL itself is compiled inside toe; this TU only uses the header).
# ---------------------------------------------------------------------------
if(NOT APPLE AND NOT WIN32)
  add_library(plat-gfx STATIC src/sokol_gpu.cpp)
  add_library(plat::gfx ALIAS plat-gfx)
  target_link_libraries(plat-gfx PUBLIC plat::plat toe::toe)
  target_include_directories(plat-gfx PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "$<TARGET_PROPERTY:toe::toe,INTERFACE_INCLUDE_DIRECTORIES>")
  # SokolGpu's TU includes sokol_gfx.h (declarations); match toe's backend
  # define + point at the vendored header. The IMPL lives in toe.
  find_package(OpenGL REQUIRED)
  target_compile_definitions(plat-gfx PRIVATE SOKOL_GLCORE)
  target_include_directories(plat-gfx PRIVATE
    "${TOE_DIR}/include/sokol")
  target_link_libraries(plat-gfx PUBLIC OpenGL::GL)
endif()

# ---------------------------------------------------------------------------
# Tests. This subdirectory is only add_subdirectory()'d by hand inside its
# HAND_BUILD_TESTS/enable_testing() block, so tests register unconditionally.
# ---------------------------------------------------------------------------
# Positive: the legal frame/draw/present protocol + RAII/affine semantics.
add_executable(gpu_algebra_test tests/gpu_algebra_test.cpp)
target_link_libraries(gpu_algebra_test PRIVATE plat::plat)
add_test(NAME plat_gpu_algebra COMMAND gpu_algebra_test)

# Negative: prove the type theory forbids the illegal states. Each case is a
# target whose COMPILE must fail; WILL_FAIL flips a failed build into a PASS.
# Case 0 is the control (must compile + run).
add_executable(plat_cf_control tests/gpu_compile_fail.cpp)
target_link_libraries(plat_cf_control PRIVATE plat::plat)
add_test(NAME plat_cf_control COMMAND plat_cf_control)

foreach(case RANGE 1 5)
  set(tgt "plat_cf_${case}")
  add_executable(${tgt} EXCLUDE_FROM_ALL tests/gpu_compile_fail.cpp)
  target_link_libraries(${tgt} PRIVATE plat::plat)
  target_compile_definitions(${tgt} PRIVATE PLAT_CF_CASE=${case})
  add_test(NAME ${tgt}
           COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                   --target ${tgt} --config $<CONFIG>)
  set_tests_properties(${tgt} PROPERTIES WILL_FAIL TRUE)
endforeach()

# Platform capability concepts: a complete mock models Platform/Clipboard/Sys
# (static_asserts); the negative case (a backend missing window_action) must
# NOT compile.
add_executable(platform_concept_test tests/platform_concept_test.cpp)
target_link_libraries(platform_concept_test PRIVATE plat::plat)
add_test(NAME plat_platform_concept COMMAND platform_concept_test)

add_executable(plat_pc_1 EXCLUDE_FROM_ALL tests/platform_concept_test.cpp)
target_link_libraries(plat_pc_1 PRIVATE plat::plat)
target_compile_definitions(plat_pc_1 PRIVATE PLAT_PC_CASE=1)
add_test(NAME plat_pc_1
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                 --target plat_pc_1 --config $<CONFIG>)
set_tests_properties(plat_pc_1 PROPERTIES WILL_FAIL TRUE)

# The type-erasure seam: wrap a concrete mock, drive it through the erased API,
# verify the linear Frame lifecycle survives + the Clipboard refinement is a
# runtime query. Negative case: wrapping a non-Platform must not compile.
add_executable(any_platform_test tests/any_platform_test.cpp)
target_link_libraries(any_platform_test PRIVATE plat::plat)
add_test(NAME plat_any_platform COMMAND any_platform_test)

add_executable(plat_ap_1 EXCLUDE_FROM_ALL tests/any_platform_test.cpp)
target_link_libraries(plat_ap_1 PRIVATE plat::plat)
target_compile_definitions(plat_ap_1 PRIVATE PLAT_AP_CASE=1)
add_test(NAME plat_ap_1
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                 --target plat_ap_1 --config $<CONFIG>)
set_tests_properties(plat_ap_1 PROPERTIES WILL_FAIL TRUE)

# Real-hardware proof: SokolGpu renders a quad into an offscreen FBO through the
# linear Frame API; pixels are read back and asserted. Linux/GL only (needs the
# offscreen EGL context from hand::platform + the sokol IMPL from toe).
if(NOT APPLE AND NOT WIN32)
  add_executable(sokol_gpu_render_test tests/sokol_gpu_render_test.cpp)
  target_include_directories(sokol_gpu_render_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/include"
    "${TOE_DIR}/include/sokol")
  target_compile_definitions(sokol_gpu_render_test PRIVATE SOKOL_GLCORE)
  target_link_libraries(sokol_gpu_render_test PRIVATE plat::gfx hand::platform OpenGL::GL)
  add_test(NAME plat_sokol_gpu_render COMMAND sokol_gpu_render_test)
endif()
