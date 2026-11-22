# tests/purity.cmake — hand's update, input and view are pure.
#
# THE CONTRACT
#
# hand is one Elm program (jaal, through mayag). Everything in src/app and
# src/view is a function of (Model, Msg) or (Model): it reads and writes the
# model and RETURNS what it wants done. The world -- a pty, a file, a
# process, the clock, the environment -- is reached only by an effect body in
# src/app/fx/, which update names in a Cmd and the runtime performs after the
# step. So:
#
#   need                           say it with
#   ─────────────────────────────  ──────────────────────────────────────────
#   bytes to a shell               se.input += bytes      (drained to fx::pty_write)
#   a new shell / ssh              se.launch = Launch{...} (drained to fx::spawn)
#   a resize                       term->resize(...)       (drained to fx::pty_resize)
#   save / load a file, run a      Cmd::run(fx::..., args) / Cmd::task(fx::..., args)
#   program, ask the OS
#   the time                       m.now / m.wall_ms       (stamped by App's root update)
#   $HOME, $EDITOR, the host name  m.env                   (read once by main: Flags)
#   what runs in a tab             se.proc / se.remote     (the Probe effect reports it)
#   "remember this"                a model field, never a static
#
# Replay, the headless self-test and the scripted shots depend on it: the
# same messages give the same model, and a test feeds messages instead of
# stubbing the OS.
#
# HOW IT IS ENFORCED
#
# Every line of src/app/*.{cpp,hpp} and src/view/*.{cpp,hpp} (NOT src/app/fx/)
# is checked against the bans below, comments stripped. The model headers
# (include/hand/app/) may not hold `mutable` state, except a memo marked
# `// memo:` on its line (a cache that only ever equals recomputing it).
# There is no allowlist: a new need gets an effect in src/app/fx/.

set(ban_clock      "(steady|system|high_resolution)_clock::now|std::time[ \t]*\\(|time\\(nullptr\\)|clock_gettime|gettimeofday|localtime")
set(ban_env        "getenv|setenv|putenv|_wgetenv|environ\\b")
set(ban_fileio     "fstream|fopen|std::filesystem|(^|[^A-Za-z_:])fs::")
set(ban_process    "(^|[^A-Za-z_])proc::|spawn_detached|popen|::system[ \t]*\\(|(^|[^A-Za-z_])sftp::|(^|[^A-Za-z_])plat::")
set(ban_pty        "pty->(write|resize|fd|reap)|PtyRef::make|Pty::resolve_shell|pty_foreground|proc_name|proc_argv|pty_reap|shell_integration[ \t]*\\(|ssh_argv[ \t]*\\(")
set(ban_persist    "config_(save|load)[ \t]*\\(|config_path[ \t]*\\(|hosts_(load|save|remove|record_use|set_pinned)[ \t]*\\(|folder_(save|remove)[ \t]*\\(|import_sources[ \t]*\\(|(^|[^A-Za-z_])resolve[ \t]*\\(")
set(ban_random     "random_device|(^|[^A-Za-z_])rand[ \t]*\\(")
set(ban_static     "^[ \t]+static[ \t]+(const[ \t]+)?[A-Za-z_:<>]+[ \t]+[A-Za-z_]+[ \t]*(=|\\{|;)")
set(ban_names clock env fileio process pty persist random static)

file(GLOB pure_files ${ROOT}/src/app/*.cpp ${ROOT}/src/app/*.hpp ${ROOT}/src/view/*.cpp ${ROOT}/src/view/*.hpp)
set(bad 0)
foreach(f ${pure_files})
    file(RELATIVE_PATH rel ${ROOT} ${f})
    file(STRINGS ${f} lines)
    set(n 0)
    foreach(l IN LISTS lines)
        math(EXPR n "${n}+1")
        string(REGEX REPLACE "//.*" "" code "${l}")
        string(REGEX REPLACE "\"([^\"\\\\]|\\\\.)*\"" "\"\"" code "${code}")   # string literals are not calls
        foreach(b ${ban_names})
            if(code MATCHES "${ban_${b}}")
                if(b STREQUAL "static" AND code MATCHES "static[ \t]+constexpr")
                    continue()
                endif()
                message(SEND_ERROR "purity: ${rel}:${n}: ${b} — update/view reach the world (make it an effect in src/app/fx/): ${l}")
                math(EXPR bad "${bad}+1")
            endif()
        endforeach()
    endforeach()
endforeach()

file(GLOB_RECURSE model_files ${ROOT}/include/hand/app/*.hpp)
foreach(f ${model_files})
    file(RELATIVE_PATH rel ${ROOT} ${f})
    file(STRINGS ${f} lines)
    set(n 0)
    foreach(l IN LISTS lines)
        math(EXPR n "${n}+1")
        if(l MATCHES "// *memo:")
            continue()
        endif()
        string(REGEX REPLACE "//.*" "" code "${l}")
        if(code MATCHES "(^|[^A-Za-z_])mutable[ \t]")
            message(SEND_ERROR "purity: ${rel}:${n}: mutable model state (the view must not write the model; a pure cache says `// memo:`): ${l}")
            math(EXPR bad "${bad}+1")
        endif()
    endforeach()
endforeach()

if(bad GREATER 0)
    message(FATAL_ERROR "purity: ${bad} violation(s)")
endif()
list(LENGTH pure_files np)
message(STATUS "purity: ${np} files in src/app + src/view reach the world only through src/app/fx")
