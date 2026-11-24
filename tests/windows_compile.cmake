# tests/windows_compile.cmake — every hand source compiles for Windows.
#
# hand ships on Windows (ConPTY, Win32 window) but nothing here built it: the
# Windows Pty fell a whole interface behind POSIX (fd_/spawn, after the move
# to Pty(const PtySpec&)) and nobody saw, because only a Windows machine or a
# CI runner would have. A mingw-w64 cross compiler (brew install mingw-w64,
# apt install g++-mingw-w64) checks it here: syntax and types for _WIN32,
# every #if MAYAG_OS_WINDOWS branch taken. Not a link, not a run -- that stays
# CI's -- but it is where a missed seam shows first. Files compile in
# parallel (xargs -P; this test only runs on a POSIX host).
#
#   cmake -DROOT=<repo> -DCXX=<x86_64-w64-mingw32-g++> [-DJOBS=N] -P windows_compile.cmake

if(NOT ROOT OR NOT CXX)
    message(FATAL_ERROR "windows_compile: needs -DROOT and -DCXX")
endif()
if(NOT JOBS)
    cmake_host_system_information(RESULT JOBS QUERY NUMBER_OF_LOGICAL_CORES)
endif()
file(GLOB_RECURSE srcs RELATIVE ${ROOT} ${ROOT}/src/*.cpp)
list(SORT srcs)
string(JOIN "\n" listing ${srcs})
set(list_file ${CMAKE_CURRENT_BINARY_DIR}/windows_compile_files.txt)
file(WRITE ${list_file} "${listing}\n")
set(flags "-std=c++26 -freflection -fsyntax-only -w -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Iinclude -Isrc -Iexternal/mayag/include -Iexternal/mayag/third_party/jaal/include -Iexternal/toe/include -Iexternal/vibe")
# each failing file prints "FAIL <file>" and its first errors; xargs keeps going
set(one "out=$(\"$0\" ${flags} \"$1\" 2>&1) || { echo \"FAIL $1\"; printf '%s\\n' \"$out\" | grep -m4 error; }")
execute_process(COMMAND sh -c "xargs -P ${JOBS} -n 1 sh -c '${one}' '${CXX}' < '${list_file}'"
                WORKING_DIRECTORY ${ROOT} OUTPUT_VARIABLE out ERROR_VARIABLE err)
list(LENGTH srcs n)
if(out MATCHES "FAIL ")
    message(SEND_ERROR "windows_compile: sources that do not compile for Windows:\n${out}")
else()
    message(STATUS "windows_compile: ${n} sources compile for Windows")
endif()
