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

set(_JAAL_LINT ${PROJECT_SOURCE_DIR}/external/mayag/third_party/jaal/tests/lint)
set(_MAYAG     ${PROJECT_SOURCE_DIR}/external/mayag)

add_test(NAME app_headless COMMAND hand --headless --last-resort)
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
set_tests_properties(layering purity concurrency_src concurrency_include_hand concurrency_allowlist_tight_src
                     concurrency_allowlist_tight_include_hand mayag_layering mayag_uses_jaal_public_api
                     mayag_concurrency jaal_upward mayag_one_look PROPERTIES LABELS "static")
