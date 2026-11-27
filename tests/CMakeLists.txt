# hand's tests
#
#   app_headless                 the self-test: a real shell in a headless window (hand --headless)
#   layering                     hand -> mayag (public) -> jaal, and hand's own layers (tests/layering.cmake)
#   purity                       update / input / view never reach the world: effects live in src/app/fx
#                                (tests/purity.cmake)
#   concurrency                  jaal's banlist over hand: no raw thread / mutex / atomic / detach /
#                                thread_local / future / const_cast outside tests/allow_raw.txt
#   concurrency_allowlist_tight  every grant in tests/allow_raw.txt is still used (the list cannot rot)
#   mayag_*                      mayag's own walls, run from here so a hand build checks every layer:
#                                its layers, its public use of jaal, and jaal's banlist over mayag
#   jaal_upward                  jaal names nothing above it
#   mayag_one_look               mayag's graphics are one source on every platform
#   one_hand                     hand is one source on every platform: platform conditionals only in the
#                                OS seam (src/core), the rest decides from data (tests/one_hand.cmake)
#   one_os                       the target (OS, compiler, CPU, ISA) is read in ONE file for hand, toe,
#                                mayag and jaal; the build's renderer / window choices in one more
#                                (jaal/meta/target.hpp, mayag/core/target.hpp)
#   one_os_cmake                 no CMakeLists branches on WIN32 / APPLE / UNIX / CMAKE_SYSTEM_NAME: the build asks
#                                the C++ headers (jaal_target_is) or reads MAYAG_SYSTEM
#   mayag_no_byte_hash           cache keys are values, never an object's padded bytes
#   echo_latency[_under_flood]   keystroke -> pty -> echo -> frame, through the real loop (hand --echo),
#                                alone and with `yes` flooding a second tab:
#                                fails if the median passes 8 ms (baseline ~1 ms release, ~2.3 ms dev)
#   hint_scan                    hint mode's hand-written URL / path / hash / IP scanners agree with the
#                                std::regex each one replaced, on 40k random strings
#   fold_budget                  the self-test run alone, failing if any update passed jaal's 8 ms fold
#                                budget (label perf; the parallel app_headless only reports them)

set(_JAAL_LINT ${PROJECT_SOURCE_DIR}/external/mayag/third_party/jaal/tests/lint)
set(_MAYAG     ${PROJECT_SOURCE_DIR}/external/mayag)

add_test(NAME app_headless COMMAND hand --headless --last-resort)
add_executable(hint_scan_test EXCLUDE_FROM_ALL hint_scan_test.cpp)
target_include_directories(hint_scan_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(hint_scan_test PRIVATE cxx_std_23)
# the build presets make only `hand`: ctest builds the test it runs
add_test(NAME hint_scan_build
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target hint_scan_test)
set_tests_properties(hint_scan_build PROPERTIES FIXTURES_SETUP hint_scan_bin)
add_test(NAME hint_scan COMMAND hint_scan_test)
set_tests_properties(hint_scan PROPERTIES FIXTURES_REQUIRED hint_scan_bin)
# The echo program is hand's own `cat` (hand --cat; --flood's is hand --yes):
# the same bytes on every OS, so these gates run on Windows too (they were
# skipped there for want of coreutils, and the ConPTY path went unmeasured).
# Keys come at a typing cadence (60 ms apart, ~200 wpm): timed from each
# key, so hand's whole share is in the number; but back to back on Windows
# every key landed in conhost's 16 ms paint rest and measured conhost, not
# hand (p50 15.6 ms back to back, 1.0 ms at a cadence).
add_test(NAME echo_latency COMMAND hand --echo --last-resort)
set_tests_properties(echo_latency PROPERTIES RUN_SERIAL TRUE TIMEOUT 60 LABELS "perf"
                     ENVIRONMENT "HAND_ECHO_GAP_MS=60;HAND_ECHO_N=100")
# ...and with `yes` flooding a second tab: a key still echoes within a
# frame (measured p50 ~0.3 ms, max ~5 ms; the gate is the same 8 ms p50)
add_test(NAME echo_latency_under_flood COMMAND hand --echo --last-resort)
set_tests_properties(echo_latency_under_flood PROPERTIES RUN_SERIAL TRUE TIMEOUT 60 LABELS "perf"
                     ENVIRONMENT "HAND_ECHO_LOAD=yes;HAND_ECHO_GAP_MS=60;HAND_ECHO_N=100")
# the self-test again, alone on the machine, failing on any update over
# jaal's 8 ms fold budget (it names the message: "slow update: hand::msg::X")
add_test(NAME fold_budget COMMAND hand --headless --last-resort)
set_tests_properties(fold_budget PROPERTIES RUN_SERIAL TRUE TIMEOUT 120 LABELS "perf"
                     ENVIRONMENT "HAND_FOLD_BUDGET=strict")
# every animation alone, from rest to rest: each one stops, and its frames
# change pixels (a toast held 193 frames that changed nothing; a bell ran
# past its flash). Machine-independent: frames and tiles, not milliseconds.
add_test(NAME motion_economy COMMAND hand --motion --last-resort)
set_tests_properties(motion_economy PROPERTIES TIMEOUT 60 LABELS "perf")
# The system shell prints 400 numbered lines on a real pty, the window is
# dragged through 160 sizes the way a person drags it (the whole app: frames,
# fit, the drain, the pty): every line must still be there, none twice.
add_test(NAME resize_drag COMMAND hand --drag --last-resort)
# Frame pacing can't regress: mayag's scheduling rules through thousands of
# jittered refreshes (a refresh without its frame, an uneven step: fail).
add_executable(mayag_pacing ${PROJECT_SOURCE_DIR}/external/mayag/tests/test_pacing.cpp)
target_link_libraries(mayag_pacing PRIVATE mayag)
add_test(NAME mayag_pacing COMMAND mayag_pacing)
set_tests_properties(mayag_pacing PROPERTIES LABELS "perf")
# The world can't stall the UI: a slow world effect (mayag::run) runs on the
# host's world lane while the loop keeps folding; order and exit-drain kept.
add_executable(mayag_world_lane ${PROJECT_SOURCE_DIR}/external/mayag/tests/test_world_lane.cpp)
target_link_libraries(mayag_world_lane PRIVATE mayag)
add_test(NAME mayag_world_lane COMMAND mayag_world_lane)
set_tests_properties(mayag_world_lane PROPERTIES LABELS "perf" TIMEOUT 30)
add_test(NAME mayag_one_pacing COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}/external/mayag/include/mayag
         -P ${PROJECT_SOURCE_DIR}/external/mayag/tests/lint/one_pacing.cmake)
set_tests_properties(mayag_one_pacing PROPERTIES LABELS "static")
add_test(NAME mayag_one_diag COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}/external/mayag/include/mayag
         -P ${PROJECT_SOURCE_DIR}/external/mayag/tests/lint/one_diag.cmake)
set_tests_properties(mayag_one_diag PROPERTIES LABELS "static")
set_tests_properties(resize_drag PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
if(MAYAG_SYSTEM STREQUAL "windows")
    # The fallback rung: the system conhost, started by hand with --resizeQuirk.
    add_test(NAME resize_drag_conhost COMMAND hand --drag --last-resort)
    set_tests_properties(resize_drag_conhost PROPERTIES RUN_SERIAL TRUE TIMEOUT 120 ENVIRONMENT HAND_CONPTY=system)
endif()
add_test(NAME layering COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/layering.cmake)
# Every source compiles for Windows, where a mingw-w64 cross compiler is
# installed (nothing else here builds the Windows code; see the script).
find_program(HAND_MINGW_CXX NAMES x86_64-w64-mingw32-g++ x86_64-w64-mingw32-g++-posix)
if(HAND_MINGW_CXX AND NOT MAYAG_SYSTEM STREQUAL "windows")
    add_test(NAME windows_compile COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DCXX=${HAND_MINGW_CXX}
             -P ${CMAKE_CURRENT_SOURCE_DIR}/windows_compile.cmake)
    set_tests_properties(windows_compile PROPERTIES TIMEOUT 900 LABELS "static;slow")
endif()
add_test(NAME purity COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/purity.cmake)

# jaal's banlist takes one root; hand's sources and headers are two
foreach(part src include/hand)
    string(MAKE_C_IDENTIFIER ${part} id)
    add_test(NAME concurrency_${id}
             COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}/${part}
                     -DALLOW=${CMAKE_CURRENT_SOURCE_DIR}/allow_raw_${id}.txt -P ${_JAAL_LINT}/banlist.cmake)
    add_test(NAME concurrency_allowlist_tight_${id}
             COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}/${part}
                     -DALLOW=${CMAKE_CURRENT_SOURCE_DIR}/allow_raw_${id}.txt -P ${_JAAL_LINT}/prune_allowlist.cmake)
endforeach()

add_test(NAME mayag_layering
         COMMAND ${CMAKE_COMMAND} -DROOT=${_MAYAG} -P ${_MAYAG}/tests/lint/layering.cmake)
add_test(NAME mayag_uses_jaal_public_api
         COMMAND sh -c "! find '${_MAYAG}/include/mayag' -name '*.hpp' -exec sed 's|//.*||' {} + | grep -nE 'jaal::detail|pool_access|cap_access|thread_root|sink_access|loop_key'")
add_test(NAME mayag_concurrency
         COMMAND ${CMAKE_COMMAND} -DROOT=${_MAYAG}/include/mayag
                 -DALLOW=${_MAYAG}/tests/lint/allow_include.txt -P ${_JAAL_LINT}/banlist.cmake)
add_test(NAME jaal_upward
         COMMAND ${CMAKE_COMMAND} -DROOT=${_MAYAG}/third_party/jaal -P ${_JAAL_LINT}/layering.cmake)
add_test(NAME mayag_one_look
         COMMAND ${CMAKE_COMMAND} -DROOT=${_MAYAG}/include/mayag -DALLOW=${_MAYAG}/tests/lint/allow_one_look.txt
                 -P ${_MAYAG}/tests/lint/one_look.cmake)
add_test(NAME mayag_no_byte_hash
         COMMAND ${CMAKE_COMMAND} -DROOT=${_MAYAG}/include/mayag -P ${_MAYAG}/tests/lint/no_byte_hash.cmake)
add_test(NAME mayag_type_scale
         COMMAND ${CMAKE_COMMAND} -DROOT=${_MAYAG} -P ${_MAYAG}/tests/lint/type_scale.cmake)
add_test(NAME one_hand
         COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_SOURCE_DIR} -DALLOW=${CMAKE_SOURCE_DIR}/tests/allow_platform.txt
                 -P ${CMAKE_SOURCE_DIR}/tests/one_hand.cmake)
add_test(NAME one_os
         COMMAND ${CMAKE_COMMAND}
                 "-DROOTS=${CMAKE_SOURCE_DIR}/src;${CMAKE_SOURCE_DIR}/include;${CMAKE_SOURCE_DIR}/tests;${CMAKE_SOURCE_DIR}/external/toe/src;${CMAKE_SOURCE_DIR}/external/toe/include;${CMAKE_SOURCE_DIR}/external/toe/tests;${_MAYAG}/include;${_MAYAG}/tests;${_MAYAG}/examples;${_MAYAG}/third_party/jaal/include;${_MAYAG}/third_party/jaal/src;${_MAYAG}/third_party/jaal/tests"
                 "-DSOURCE=${_MAYAG}/third_party/jaal/include/jaal/meta/target.hpp;${CMAKE_SOURCE_DIR}/external/toe/include/toe/core/target.hpp"
                 -DBUILD_SOURCE=${_MAYAG}/include/mayag/core/target.hpp
                 -P ${_MAYAG}/third_party/jaal/tests/lint/one_os.cmake)
add_test(NAME one_os_cmake
         COMMAND ${CMAKE_COMMAND}
                 "-DFILES=${CMAKE_SOURCE_DIR}/CMakeLists.txt;${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt;${CMAKE_SOURCE_DIR}/external/toe/CMakeLists.txt;${CMAKE_SOURCE_DIR}/external/toe/src/CMakeLists.txt;${_MAYAG}/CMakeLists.txt;${_MAYAG}/third_party/jaal/CMakeLists.txt;${_MAYAG}/third_party/jaal/tests/CMakeLists.txt"
                 -P ${_MAYAG}/third_party/jaal/tests/lint/one_os_cmake.cmake)
set_tests_properties(layering purity concurrency_src concurrency_include_hand concurrency_allowlist_tight_src
                     concurrency_allowlist_tight_include_hand mayag_layering mayag_uses_jaal_public_api
                     mayag_concurrency jaal_upward mayag_one_look mayag_no_byte_hash mayag_type_scale one_hand one_os one_os_cmake PROPERTIES LABELS "static")
