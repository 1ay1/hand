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
#   one_os                       the OS is read in ONE file for hand, mayag and jaal
#                                (jaal/meta/target.hpp); everything tests JAAL_OS_* / MAYAG_OS_*
#   mayag_no_byte_hash           cache keys are values, never an object's padded bytes
#   echo_latency                 keystroke -> pty -> echo -> frame, through the real loop (hand --echo):
#                                fails if the median passes 8 ms (baseline ~1 ms release, ~2.3 ms dev)

set(_JAAL_LINT ${PROJECT_SOURCE_DIR}/external/mayag/third_party/jaal/tests/lint)
set(_MAYAG     ${PROJECT_SOURCE_DIR}/external/mayag)

add_test(NAME app_headless COMMAND hand --headless --last-resort)
if(NOT WIN32)   # `cat` is the echo program
    add_test(NAME echo_latency COMMAND hand --echo --last-resort)
    set_tests_properties(echo_latency PROPERTIES RUN_SERIAL TRUE TIMEOUT 60 LABELS "perf")
endif()
add_test(NAME layering COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/layering.cmake)
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
                 "-DROOTS=${CMAKE_SOURCE_DIR}/src;${CMAKE_SOURCE_DIR}/include;${_MAYAG}/include;${_MAYAG}/third_party/jaal/include;${_MAYAG}/third_party/jaal/src"
                 -DSOURCE=${_MAYAG}/third_party/jaal/include/jaal/meta/target.hpp
                 -P ${_MAYAG}/third_party/jaal/tests/lint/one_os.cmake)
set_tests_properties(layering purity concurrency_src concurrency_include_hand concurrency_allowlist_tight_src
                     concurrency_allowlist_tight_include_hand mayag_layering mayag_uses_jaal_public_api
                     mayag_concurrency jaal_upward mayag_one_look mayag_no_byte_hash mayag_type_scale one_hand one_os PROPERTIES LABELS "static")
