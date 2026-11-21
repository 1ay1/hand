# tests/layering.cmake — every #include respects hand's layer graph.
#
#   cmake -DROOT=<repo> -P tests/layering.cmake
#
# A layer may include itself, the layers below it, and the libraries beneath
# hand (mayag, toe, vibe, the standard library). It may never include a layer
# above it -- and nothing in hand ever names jaal (mayag's implementation).
# The app layer also names every change: no mayag::Edit in src/app, src/view.
#
#   layer    may include hand/...
#   core     core
#   config   core config
#   ssh      core ssh
#   app      core config ssh app            (src/app, src/view, main.cpp)

set(allowed_core   "core")
set(allowed_config "core;config")
set(allowed_ssh    "core;ssh")
set(allowed_app    "core;config;ssh;app")

# which layer a file belongs to
function(layer_of file out)
    set(l app)
    if(file MATCHES "/(src|include/hand)/core/")
        set(l core)
    elseif(file MATCHES "/(src|include/hand)/config/")
        set(l config)
    elseif(file MATCHES "/(src|include/hand)/ssh/")
        set(l ssh)
    endif()
    set(${out} ${l} PARENT_SCOPE)
endfunction()

file(GLOB_RECURSE files ${ROOT}/src/*.cpp ${ROOT}/src/*.hpp ${ROOT}/include/hand/*.hpp)
set(bad 0)
foreach(f ${files})
    layer_of(${f} layer)
    file(STRINGS ${f} lines REGEX "^[ \t]*#[ \t]*include")
    foreach(l ${lines})
        if(l MATCHES "jaal/")
            message(SEND_ERROR "layering: ${f}: names jaal (mayag's implementation): ${l}")
            math(EXPR bad "${bad}+1")
        endif()
        if(l MATCHES "<hand/([a-z_]+)[/.]")
            set(target ${CMAKE_MATCH_1})   # hand/app.hpp and hand/app/... are both the app layer
            list(FIND allowed_${layer} ${target} ok)
            if(ok EQUAL -1)
                message(SEND_ERROR "layering: ${f} (${layer}) includes ${target}: ${l}")
                math(EXPR bad "${bad}+1")
            endif()
        endif()
        # a lower layer reaching into src/app or src/view by relative path
        if(NOT layer STREQUAL "app" AND l MATCHES "\"\\.\\./(app|view)/")
            message(SEND_ERROR "layering: ${f} (${layer}) reaches into ${CMAKE_MATCH_1}/: ${l}")
            math(EXPR bad "${bad}+1")
        endif()
    endforeach()
endforeach()

# the app names every change it makes: no anonymous model edits (mayag::Edit)
# in src/app or src/view -- those are for the self-test and scripted shots
file(GLOB_RECURSE app_files ${ROOT}/src/app/* ${ROOT}/src/view/*)
foreach(f ${app_files})
    file(STRINGS ${f} edits REGEX "Edit<|(^|[^A-Za-z0-9_])E\\{\\[")
    foreach(l ${edits})
        message(SEND_ERROR "layering: ${f}: an anonymous model edit (name it as a message): ${l}")
        math(EXPR bad "${bad}+1")
    endforeach()
endforeach()
if(bad GREATER 0)
    message(FATAL_ERROR "layering: ${bad} violation(s)")
endif()
list(LENGTH files n)
message(STATUS "layering: ${n} files, every include goes down")
