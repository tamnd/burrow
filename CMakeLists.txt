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
# than a private one, because it changes the shape of burrow__Context and every
# file that reads the header has to agree about that.
option(BURROW_PORTABLE_CONTEXT "Use ucontext or Fibers instead of the context switch assembly" OFF)

# The assembler, which is the same compiler driver under another name. MSVC does
# not get it: its assembler reads a different syntax entirely, and Windows takes
# the Fibers path anyway, so there is nothing for it to assemble. Every .S in
# the tree is guarded on the same macros the header picks a backend with, so a
# file that is not wanted on this target assembles to an empty object rather
# than needing a rule here to leave it out.
if(NOT MSVC)
    enable_language(ASM)
endif()

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
if(NOT MSVC)
    file(GLOB_RECURSE BURROW_ASM_SOURCES CONFIGURE_DEPENDS src/*.S)
    list(APPEND BURROW_SOURCES ${BURROW_ASM_SOURCES})
endif()

add_library(burrow STATIC ${BURROW_SOURCES})
add_library(burrow::burrow ALIAS burrow)

target_include_directories(burrow PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_definitions(burrow PRIVATE BURROW_SOURCE_ID="${BURROW_SOURCE_ID}")

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

if(BURROW_ENABLE_BOEHM)
    target_compile_definitions(burrow PRIVATE BURROW_ENABLE_BOEHM=1)
    target_link_libraries(burrow PUBLIC gc)
endif()

# The warning set mirrors the Makefile's. MSVC gets its own because /W4 is not
# -Wall and the numbered warnings have no gcc equivalent worth mapping.
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
    target_compile_options(burrow PRIVATE /W4 /permissive- /utf-8 /wd4100 /wd4324)
    if(BURROW_WERROR)
        target_compile_options(burrow PRIVATE /WX)
    endif()
    # MSVC's idea of a deprecated function includes most of the C standard
    # library. burrow does not call the functions it means, so the warning is
    # only ever noise here.
    target_compile_definitions(burrow PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    # All of it is behind COMPILE_LANGUAGE:C, because most of this list is about
    # C and a compiler handed -Wstrict-prototypes for an assembler file is
    # entitled to say the argument did nothing. With -Werror on, saying so is
    # enough to fail the build.
    target_compile_options(burrow PRIVATE
        "$<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wpedantic;-Wshadow;-Wcast-qual;-Wcast-align;-Wconversion;-Wsign-conversion;-Wwrite-strings;-Wpointer-arith;-Wstrict-prototypes;-Wmissing-prototypes;-Wmissing-declarations;-Wredundant-decls;-Wundef;-Wswitch-enum;-Wswitch-default;-Wdouble-promotion;-Wformat=2;-Wformat-security;-Wnull-dereference;-Wstack-protector;-Wvla;-Wno-unused-parameter;-fstack-protector-strong;-fno-common>")
    if(BURROW_WERROR)
        target_compile_options(burrow PRIVATE "$<$<COMPILE_LANGUAGE:C>:-Werror>")
    endif()
endif()

if(BURROW_BUILD_TESTS)
    enable_testing()
    file(GLOB BURROW_TEST_SOURCES CONFIGURE_DEPENDS tests/*_test.c)
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
        add_executable(${test_name} ${test_source})
        target_link_libraries(${test_name} PRIVATE burrow)
        target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        if(MSVC)
            target_compile_definitions(${test_name} PRIVATE _CRT_SECURE_NO_WARNINGS)
        endif()
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()

include(GNUInstallDirs)
install(TARGETS burrow EXPORT burrowTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/burrow DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT burrowTargets NAMESPACE burrow:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/burrow)
