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
