# tests/one_hand.cmake — hand behaves the same on every platform.
#
#   cmake -DROOT=<repo> -DALLOW=<repo>/tests/allow_platform.txt -P tests/one_hand.cmake
#
# hand is written once. What differs between macOS, Linux and Windows lives
# in ONE seam, src/core (+ include/hand/core): the pty, processes, the OS's
# answers (home, time, files) and its capabilities (can its ssh share a
# connection?). Everything above -- app, view, ssh, config -- decides from
# DATA: a key carries its keymap (mayag::Keymap: the Mac's or the PC's), a
# shell has its dialect (hand::Dialect), the OS seam states its capabilities.
# So both conventions run in the self-test on every machine.
#
# Outside the seam a platform conditional (_WIN32, __APPLE__, __linux__,
# __unix__, TARGET_OS_*) is an error unless tests/allow_platform.txt grants
# that file an exact COUNT, with the reason. A new conditional fails the
# build; so does a grant that is no longer used up.

file(STRINGS ${ALLOW} allow_lines)
set(granted "")
foreach(l IN LISTS allow_lines)
    string(REGEX REPLACE "#.*" "" l "${l}")
    string(STRIP "${l}" l)
    if(l STREQUAL "")
        continue()
    endif()
    string(REGEX MATCH "^([^:]+): *([0-9]+)$" m "${l}")
    if(NOT m)
        message(SEND_ERROR "one_hand: bad grant line: ${l}")
        continue()
    endif()
    set(grant_${CMAKE_MATCH_1} ${CMAKE_MATCH_2})
    list(APPEND granted ${CMAKE_MATCH_1})
endforeach()

file(GLOB_RECURSE files RELATIVE ${ROOT} ${ROOT}/src/*.cpp ${ROOT}/src/*.hpp ${ROOT}/include/hand/*.hpp)
set(bad 0)
foreach(rel ${files})
    if(rel MATCHES "^src/core/" OR rel MATCHES "^include/hand/core/")
        continue()   # the OS seam
    endif()
    file(STRINGS ${ROOT}/${rel} hits REGEX "^[ \t]*#[ \t]*(if|ifdef|ifndef|elif)[ \t].*(_WIN32|__APPLE__|__linux__|__unix__|TARGET_OS_|MAYAG_OS_|JAAL_OS_)")
    list(LENGTH hits n)
    set(allowed 0)
    if(DEFINED grant_${rel})
        set(allowed ${grant_${rel}})
    endif()
    if(NOT n EQUAL allowed)
        if(n GREATER allowed)
            message(SEND_ERROR "one_hand: ${rel} has ${n} platform conditional(s), ${allowed} granted. Decide from data "
                    "(Mods.keymap, shell_dialect, a plat:: capability) or move the OS call into src/core. Lines: ${hits}")
        else()
            message(SEND_ERROR "one_hand: ${rel} is granted ${allowed} but uses ${n}: tighten tests/allow_platform.txt")
        endif()
        math(EXPR bad "${bad}+1")
    endif()
endforeach()
foreach(g ${granted})
    if(NOT EXISTS ${ROOT}/${g})
        message(SEND_ERROR "one_hand: a grant for ${g}, which does not exist")
        math(EXPR bad "${bad}+1")
    endif()
endforeach()
if(bad GREATER 0)
    message(FATAL_ERROR "one_hand: ${bad} violation(s)")
endif()
message(STATUS "one_hand: hand is one source on every platform (the OS seam: src/core)")
