# tests/layering.cmake — the hierarchy, enforced:  hand → mayag → jaal.
#
#   cmake -DROOT=<repo> -P tests/layering.cmake
#
# 1. hand's own layers. A layer includes itself and the layers below:
#
#      layer    may include hand/...
#      core     core
#      config   core config
#      ssh      core ssh
#      app      core config ssh app            (src/app, src/view, src/cli, main.cpp)
#
# 2. hand → mayag, through mayag's PUBLIC surface only:
#      <mayag/mayag.hpp>           the framework (program model, dsl, theme, runtime)
#      <mayag/widget/...>          the widgets
#      <mayag/core/event.hpp>      key/mouse vocabulary, for core/ (term.hpp): core
#                                  must not pull in the runtime that mayag.hpp brings
#    Never mayag's internals (app/ host/ backend/ platform/ render/ scene/ ...),
#    and never mayag::detail.
#
# 3. hand never names jaal -- not an include, not jaal::, not a jaal macro.
#    mayag re-exports what an app needs (Cmd, Sub, Sink, CoOwned, Shared,
#    Guarded, Sendable, Sync, MAYAG_SYNC ...). A missing piece is added to
#    mayag, never reached around.
#
# 4. The app names every change: no anonymous model edit (mayag::Edit) in
#    src/app or src/view -- those are for the self-test and scripted shots.
#
# Raw threads, locks and atomics are checked separately, by jaal's own
# banlist (ctest `concurrency_*`, allowlists tests/allow_raw_*.txt).

set(allowed_core   "core")
set(allowed_config "core;config")
set(allowed_ssh    "core;ssh")
set(allowed_app    "core;config;ssh;app")
# rule 2: the mayag headers hand may name, beyond <mayag/widget/...>
set(mayag_public "mayag/mayag.hpp;mayag/core/event.hpp")

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
macro(violation msg)
    message(SEND_ERROR "layering: ${msg}")
    math(EXPR bad "${bad}+1")
endmacro()

foreach(f ${files})
    file(RELATIVE_PATH rel ${ROOT} ${f})
    layer_of(${f} layer)
    file(STRINGS ${f} lines)
    foreach(l ${lines})
        string(REGEX REPLACE "//.*" "" code "${l}")   # prose about jaal is not a use of it
        # rule 3
        if(code MATCHES "jaal/|jaal::|JAAL_")
            violation("${rel}: names jaal (mayag's implementation; use mayag's name for it): ${l}")
        endif()
        if(NOT code MATCHES "^[ \t]*#[ \t]*include")
            if(code MATCHES "mayag::detail")
                violation("${rel}: reaches into mayag::detail: ${l}")
            endif()
            continue()
        endif()
        # rule 1
        if(code MATCHES "<hand/([a-z_]+)[/.]")
            set(target ${CMAKE_MATCH_1})   # hand/app.hpp and hand/app/... are both the app layer
            list(FIND allowed_${layer} ${target} ok)
            if(ok EQUAL -1)
                violation("${rel} (${layer}) includes ${target}: ${l}")
            endif()
        endif()
        if(NOT layer STREQUAL "app" AND code MATCHES "\"\\.\\./(app|view|cli)/")
            violation("${rel} (${layer}) reaches into ${CMAKE_MATCH_1}/: ${l}")
        endif()
        # rule 2
        if(code MATCHES "<(mayag/[^>]+)>")
            set(h ${CMAKE_MATCH_1})
            list(FIND mayag_public ${h} pub)
            if(NOT h MATCHES "^mayag/widget/" AND pub EQUAL -1)
                violation("${rel}: includes mayag's internals (${h}); hand uses <mayag/mayag.hpp> and <mayag/widget/...>: ${l}")
            endif()
            if(h STREQUAL "mayag/mayag.hpp" AND layer STREQUAL "core")
                violation("${rel}: core includes the mayag runtime (mayag.hpp); core is VT + OS only: ${l}")
            endif()
        endif()
    endforeach()
endforeach()

# rule 4
file(GLOB_RECURSE app_files ${ROOT}/src/app/* ${ROOT}/src/view/*)
foreach(f ${app_files})
    file(RELATIVE_PATH rel ${ROOT} ${f})
    file(STRINGS ${f} edits REGEX "Edit<|(^|[^A-Za-z0-9_])E\\{\\[")
    foreach(l ${edits})
        violation("${rel}: an anonymous model edit (name it as a message): ${l}")
    endforeach()
endforeach()

if(bad GREATER 0)
    message(FATAL_ERROR "layering: ${bad} violation(s)")
endif()
list(LENGTH files n)
message(STATUS "layering: ${n} files; hand -> mayag (public) -> jaal holds")
