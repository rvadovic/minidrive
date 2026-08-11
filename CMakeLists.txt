cmake_minimum_required(VERSION 3.22)

project(MiniDrive VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MINIDRIVE_BUILD_TESTS "Build MiniDrive tests" ON)
option(MINIDRIVE_STATIC_RUNTIME
    "Statically link libstdc++/libgcc into server/client so a release binary has no matching runtime-version requirement (Linux, non-MSVC only)"
    ON)

include(GNUInstallDirs)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)

# `minidrive::resolved_version()` prefers `git describe` over the plain project version, so
# releases built straight from a tag (or a dev build N commits past one) self-report exactly what
# they were built from. Falls back to an empty string (see version.hpp.in) when the source tree
# isn't a git checkout at all, e.g. a downloaded "Source code" tarball.
find_package(Git QUIET)
set(MINIDRIVE_GIT_DESCRIBE "")
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE MINIDRIVE_GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE MINIDRIVE_GIT_DESCRIBE_RESULT
    )
    if(NOT MINIDRIVE_GIT_DESCRIBE_RESULT EQUAL 0)
        set(MINIDRIVE_GIT_DESCRIBE "")
    endif()
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/shared/include/minidrive/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/minidrive/version.hpp
    @ONLY
)

add_subdirectory(shared)
add_subdirectory(server)
add_subdirectory(client)

if(MINIDRIVE_BUILD_TESTS)
    # Must be called at the top level: CTest only generates a root build/CTestTestfile.cmake
    # (what `ctest --test-dir build` reads) when enable_testing() runs here, not just inside
    # the tests/ subdirectory - otherwise `ctest` silently reports "No tests were found!!!"
    # even though tests/CTestTestfile.cmake was generated correctly one level down.
    enable_testing()
    add_subdirectory(tests)
endif()

include(Packaging)

