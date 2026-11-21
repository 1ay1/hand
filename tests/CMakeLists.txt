# hand's tests
#   app_headless   the self-test: a real shell in a headless window (hand --headless)
#   layering       every #include goes down the layer graph (tests/layering.cmake)
add_test(NAME app_headless COMMAND hand --headless --last-resort)
add_test(NAME layering COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/layering.cmake)
