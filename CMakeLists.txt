# CMake is here for Windows and for people whose build already speaks CMake.
# The Makefile is the one the maintainers use and the amalgamation is the one
# users are meant to use, so this file's job is to be correct and boring rather
# than to be clever.
#
# There is no configure step anywhere in this project. CMake is doing target
# and toolchain plumbing here, not feature detection, and if you find a
# check_symbol_exists in this file somebody has made a mistake.

cmake_minimum_required(VERSION 3.16)

file(READ include/burrow/version.h _version_header)
string(REGEX MATCH "BURROW_VERSION_STRING \"([0-9]+\\.[0-9]+\\.[0-9]+)\"" _ "${_version_header}")
set(BURROW_VERSION "${CMAKE_MATCH_1}")

project(burrow VERSION ${BURROW_VERSION} LANGUAGES C
        DESCRIPTION "The Go standard library, in C"
        HOMEPAGE_URL "https://github.com/tamnd/burrow")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(BURROW_BUILD_TESTS "Build burrow's tests" ${PROJECT_IS_TOP_LEVEL})
option(BURROW_WERROR "Turn warnings into errors" ${PROJECT_IS_TOP_LEVEL})

# The table of function names a traceback prints instead of bare addresses.
# Turning it off gives back the addresses and lets the linker drop objects
# nothing calls, which is what an embedded build wants. See burrow/symtab.h.
option(BURROW_SYMTAB "Build the table of names a traceback prints" ON)

# The gc backend, off by default. This is the project's only optional
# dependency and it is still not a feature check: turning it on says you have
# the Boehm collector and want it linked, and leaving it off means gc_allocator
# answers NULL. Nothing here looks for the library, because a build that asks
# for a collector it does not have should fail at the link line saying so
# rather than quietly produce a program without one.
option(BURROW_ENABLE_BOEHM "Link the Boehm collector and enable the gc backend" OFF)

# Forces the portable context switch on a machine that has assembly for it. This
# is not something you would ship, it is how you find out whether a bug is in
# the assembly or above it. It has to be a PUBLIC definition further down rather
# than a private one, because it changes the shape of burrow__MContext and every
# file that reads the header has to agree about that.
option(BURROW_PORTABLE_CONTEXT "Use ucontext or Fibers instead of the context switch assembly" OFF)

execute_process(
    COMMAND git describe --always --dirty --match v[0-9]*
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE BURROW_SOURCE_ID
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT BURROW_SOURCE_ID)
    set(BURROW_SOURCE_ID "unknown")
endif()

file(GLOB_RECURSE BURROW_SOURCES CONFIGURE_DEPENDS src/*.c)

# Two library targets where one would do, because of the symbol table further
# down. It is read out of the compiled objects with nm and then compiled in
# beside them, and no target can be an input to its own source list, so the
# objects are a target of their own and the archive is built from those plus the
# one generated file. Nothing outside this file sees the split: burrow is still
# the only target anybody links and the only one that gets installed.
add_library(burrow_objects OBJECT ${BURROW_SOURCES})

# Everything the library is compiled with, applied to both targets above.
# Anything that also has to reach whoever links burrow is set on burrow as
# PUBLIC further down instead, which is the only difference between the two
# lists and the reason this one is a function rather than an interface target:
# an interface target would have to be exported alongside burrow for install to
# work, and there is nothing in it a consumer should be able to see.
function(burrow_compile_settings target)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(${target} PRIVATE BURROW_SOURCE_ID="${BURROW_SOURCE_ID}")
    if(BURROW_PORTABLE_CONTEXT)
        target_compile_definitions(${target} PRIVATE BURROW_PORTABLE_CONTEXT=1)
    endif()
    if(BURROW_ENABLE_BOEHM)
        target_compile_definitions(${target} PRIVATE BURROW_ENABLE_BOEHM=1)
    endif()
endfunction()

# Where the generated table goes, and how it gets there.
#
# The Makefile runs the same script over the same objects. What is different
# here is the fallback: the script needs a shell and nm, MSVC builds have
# neither, and a Windows build that failed at this point would be a Windows
# build that fails. So a build with no way to read the objects writes the empty
# table instead and gets addresses in its tracebacks, which is exactly what
# BURROW_SYMTAB=OFF asks for on purpose.
set(BURROW_SYMTAB_SRC ${CMAKE_CURRENT_BINARY_DIR}/gen/symtab_gen.c)
find_program(BURROW_SYMTAB_SH sh)
find_program(BURROW_SYMTAB_NM nm)

if(BURROW_SYMTAB AND BURROW_SYMTAB_SH AND BURROW_SYMTAB_NM AND NOT CMAKE_CROSSCOMPILING)
    add_custom_command(
        OUTPUT ${BURROW_SYMTAB_SRC}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
        COMMAND ${BURROW_SYMTAB_SH} ${CMAKE_CURRENT_SOURCE_DIR}/tools/burrow-symtab
                -o ${BURROW_SYMTAB_SRC} $<TARGET_OBJECTS:burrow_objects>
        DEPENDS burrow_objects ${CMAKE_CURRENT_SOURCE_DIR}/tools/burrow-symtab
        COMMAND_EXPAND_LISTS
        VERBATIM
        COMMENT "Reading the traceback name table out of the objects")
else()
    # Written at configure time rather than built, because there is nothing to
    # read and nothing to read it with. It is the same file the script emits for
    # --empty, and burrow/symtab.h says what a table of length zero means.
    file(WRITE ${BURROW_SYMTAB_SRC}
"/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* Written by CMakeLists.txt. Do not edit, and do not commit.

 * An empty table, because this build was asked for one or because it has no
 * way to read the names out of its own objects. A traceback prints addresses.
 * A table of length zero still has to be an array of something, so it is one
 * null pointer that the count says is not there. */
extern void (*const burrow__symbol_addrs[])(void);
extern const char *const burrow__symbol_names[];
extern const int burrow__symbol_count;

void (*const burrow__symbol_addrs[])(void) = {0};
const char *const burrow__symbol_names[] = {0};
const int burrow__symbol_count = 0;
")
endif()

add_library(burrow STATIC $<TARGET_OBJECTS:burrow_objects> ${BURROW_SYMTAB_SRC})
add_library(burrow::burrow ALIAS burrow)

burrow_compile_settings(burrow_objects)
burrow_compile_settings(burrow)

target_include_directories(burrow PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

# PUBLIC, and see the option above for why: this one is part of the layout of a
# type, so a program that links burrow has to be compiled with the same answer
# the library was.
if(BURROW_PORTABLE_CONTEXT)
    target_compile_definitions(burrow PUBLIC BURROW_PORTABLE_CONTEXT=1)
endif()

# Threads. This is the one find_package in the file and it is not a feature
# check either: the runtime needs OS threads on every platform and the only
# question is what the local toolchain calls them, which is -pthread here and
# nothing at all on Windows. PUBLIC because a program linking burrow links the
# same threads, and THREADS_PREFER_PTHREAD_FLAG because -pthread sets defines
# that -lpthread does not.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(burrow PUBLIC Threads::Threads)

# An object library links nothing, so this one is here for the compile flag
# alone. -pthread is not only a linker argument: it defines _REENTRANT, and a
# library compiled without it beside one compiled with it is two different
# libraries.
target_link_libraries(burrow_objects PRIVATE Threads::Threads)

if(BURROW_ENABLE_BOEHM)
    target_link_libraries(burrow PUBLIC gc)
endif()

# Winsock. The completion port itself is kernel32, which comes in for free, so
# nothing in the library needs this yet. What needs it is every caller that has
# a socket to submit an operation on, which is the tests today and net later,
# and PUBLIC is what puts it on their link line without each of them asking.
if(WIN32)
    target_link_libraries(burrow PUBLIC ws2_32)
endif()

# The warning set mirrors the Makefile's. MSVC gets its own because /W4 is not
# -Wall and the numbered warnings have no gcc equivalent worth mapping. Both
# library targets get it, and so does the generated symbol table, which is a
# file with four hundred declarations in it and worth compiling as carefully as
# anything that was written by hand.
foreach(target burrow_objects burrow)
if(MSVC)
    # /wd4100 is MSVC's unreferenced formal parameter, and it is off for the
    # same reason -Wno-unused-parameter is off everywhere else. A vtable slot
    # has a fixed signature and most implementations of it ignore something.
    #
    # /wd4324 is "structure was padded due to alignment specifier", which MSVC
    # says every time it sees _Alignas on a member. It is a warning about
    # something being done on purpose. The P structure puts its run queue head
    # and tail on separate cache lines specifically so that a thief and an owner
    # do not fight over one, and the padding that comes with that is the point
    # rather than an accident. gcc and clang do not warn about this at all.
    #
    # /wd4200 is "zero-sized array in struct/union", which MSVC calls a
    # nonstandard extension. It was one in C89 and it has been in the standard
    # since C99, which is two revisions before the one this builds as. MSVC
    # compiles it correctly and warns anyway. sync.Pool's rings and per P arrays
    # are one allocation each because of it.
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /wd4100 /wd4200 /wd4324)
    if(BURROW_WERROR)
        target_compile_options(${target} PRIVATE /WX)
    endif()
    # MSVC's idea of a deprecated function includes most of the C standard
    # library. burrow does not call the functions it means, so the warning is
    # only ever noise here.
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    # All of it is behind COMPILE_LANGUAGE:C, because most of this list is about
    # C, and a project that adds a language to this build should not have C's
    # warnings handed to its compiler. With -Werror on, a flag the compiler does
    # not know is enough to fail the build.
    target_compile_options(${target} PRIVATE
        "$<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wpedantic;-Wshadow;-Wcast-qual;-Wcast-align;-Wconversion;-Wsign-conversion;-Wwrite-strings;-Wpointer-arith;-Wstrict-prototypes;-Wmissing-prototypes;-Wmissing-declarations;-Wredundant-decls;-Wundef;-Wswitch-enum;-Wswitch-default;-Wdouble-promotion;-Wformat=2;-Wformat-security;-Wnull-dereference;-Wstack-protector;-Wvla;-Wno-unused-parameter;-fstack-protector-strong;-fno-common>")
    if(BURROW_WERROR)
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:C>:-Werror>")
    endif()

    # PUBLIC, and separate from the warning list, because this one is not a
    # warning and it is not private either. The stack walker follows the frame
    # pointer chain, so a build without frame pointers gets a panic message and
    # no trace, which is the difference between a bug report somebody can act on
    # and one they cannot. Anything linking burrow wants its own frames walkable
    # too, which is why it crosses the target boundary.
    #
    # It costs a register on amd64 and nothing on arm64, macOS has kept frame
    # pointers all along, and the Linux distributions have been turning them
    # back on because the profilers and the debuggers want them. The Makefile
    # has had this from the start. Release builds here did not, which is why a
    # -O2 build on x86-64 Linux failed trace_test and a -O0 one did not.
    target_compile_options(${target} PUBLIC "$<$<COMPILE_LANGUAGE:C>:-fno-omit-frame-pointer>")
endif()
endforeach()

if(BURROW_BUILD_TESTS)
    enable_testing()
    file(GLOB BURROW_TEST_SOURCES CONFIGURE_DEPENDS tests/*_test.c)

    # Type descriptors that tools/burrow-gen produced from the annotated structs
    # in tests/gen. They are checked in rather than generated here, because
    # generating them needs libclang and building burrow must never need
    # libclang, and every test binary gets them for the same reason the Makefile
    # gives every test binary them: one test links them and working out which
    # one is not worth the line it would take.
    file(GLOB BURROW_TEST_GEN CONFIGURE_DEPENDS tests/gen/*.c)

    foreach(test_source ${BURROW_TEST_SOURCES})
        get_filename_component(test_name ${test_source} NAME_WE)
        # MSVC keeps C11 atomics behind /experimental:c11atomics, so including
        # <stdatomic.h> without it is a hard error. The C11 backend is never the
        # one MSVC picks and this test exists only to force it, so there is
        # nothing here worth an experimental compiler switch. Every other
        # platform builds and runs it.
        if(MSVC AND test_name STREQUAL "atomic_c11_test")
            continue()
        endif()
        add_executable(${test_name} ${test_source} ${BURROW_TEST_GEN})
        target_link_libraries(${test_name} PRIVATE burrow)
        target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        # /utf-8 for the same reason the library has it. Tests ported from Go
        # are full of string literals like "　", and without it MSVC turns
        # them into the system code page, or into a question mark when the code
        # page has no such character, instead of the UTF-8 that Go means.
        if(MSVC)
            target_compile_options(${test_name} PRIVATE /utf-8)
            target_compile_definitions(${test_name} PRIVATE _CRT_SECURE_NO_WARNINGS)
        endif()
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(BURROW_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/burrow)

install(TARGETS burrow EXPORT burrowTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/burrow DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT burrowTargets NAMESPACE burrow:: DESTINATION ${BURROW_CMAKE_DIR})

# SameMajorVersion is the usual rule and it is the wrong one before 1.0, where
# the major number is zero and never moves. SameMinorVersion says 0.4.1 will do
# when 0.4.0 was asked for and 0.5.0 will not, which is what this project's
# version numbers actually promise: a minor bump is a finished milestone and may
# have moved things.
configure_package_config_file(
    cmake/burrow-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/burrowConfig.cmake
    INSTALL_DESTINATION ${BURROW_CMAKE_DIR})
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/burrowConfigVersion.cmake
    COMPATIBILITY SameMinorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/burrowConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/burrowConfigVersion.cmake
    DESTINATION ${BURROW_CMAKE_DIR})
