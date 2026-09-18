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

add_library(burrow STATIC ${BURROW_SOURCES})
add_library(burrow::burrow ALIAS burrow)

target_include_directories(burrow PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_definitions(burrow PRIVATE BURROW_SOURCE_ID="${BURROW_SOURCE_ID}")

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
    target_compile_options(burrow PRIVATE /W4 /permissive- /utf-8 /wd4100)
    if(BURROW_WERROR)
        target_compile_options(burrow PRIVATE /WX)
    endif()
    # MSVC's idea of a deprecated function includes most of the C standard
    # library. burrow does not call the functions it means, so the warning is
    # only ever noise here.
    target_compile_definitions(burrow PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(burrow PRIVATE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wcast-qual -Wcast-align -Wconversion -Wsign-conversion
        -Wwrite-strings -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes
        -Wmissing-declarations -Wredundant-decls -Wundef -Wswitch-enum
        -Wswitch-default -Wdouble-promotion -Wformat=2 -Wformat-security
        -Wnull-dereference -Wstack-protector -Wvla -Wno-unused-parameter
        -fstack-protector-strong -fno-common)
    if(BURROW_WERROR)
        target_compile_options(burrow PRIVATE -Werror)
    endif()
endif()

if(BURROW_BUILD_TESTS)
    enable_testing()
    file(GLOB BURROW_TEST_SOURCES CONFIGURE_DEPENDS tests/*_test.c)
    foreach(test_source ${BURROW_TEST_SOURCES})
        get_filename_component(test_name ${test_source} NAME_WE)
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
