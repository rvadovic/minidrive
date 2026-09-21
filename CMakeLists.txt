cmake_minimum_required(VERSION 3.22)

project(MiniDrive VERSION 0.7.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MINIDRIVE_BUILD_TESTS "Build MiniDrive tests" ON)
option(MINIDRIVE_STATIC_RUNTIME
    "Statically link libstdc++/libgcc into server/client so a release binary has no matching runtime-version requirement (Linux, non-MSVC only)"
    ON)

# The server's source release (minidrive-server-<ver>-src.tar.gz) ships without client/, so each half
# builds only when its directory is present. A full checkout builds both, exactly as before.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/client/CMakeLists.txt")
    set(_minidrive_has_client ON)
else()
    set(_minidrive_has_client OFF)
endif()
option(MINIDRIVE_BUILD_CLIENT "Build the client" ${_minidrive_has_client})
option(MINIDRIVE_BUILD_SERVER "Build the server" ON)

# A client that is one downloaded file and runs: no libc, libssl or C++ runtime dependency on
# Linux (built against musl - see "Release Artifacts" in the release workflow), static CRT on MSVC,
# and OpenSSL embedded everywhere. This deliberately reverses the dynamic-libssl choice the server
# keeps: a client user cannot be relied on to patch OpenSSL, and Windows/macOS have no system
# OpenSSL to link against in the first place. Meant for a client-only build tree - it also makes
# find_package(OpenSSL) prefer static archives for everything else configured in the same tree.
option(MINIDRIVE_FULLY_STATIC
    "Build a fully self-contained client: -static on Linux, /MT on MSVC, static OpenSSL (3.5+ required)"
    OFF)

if(MINIDRIVE_FULLY_STATIC)
    # Both have to be in place before Dependencies.cmake: the first steers find_package(OpenSSL),
    # the second has to reach libsodium's FetchContent build too, or the link mixes CRTs.
    set(OPENSSL_USE_STATIC_LIBS ON)
    set(OPENSSL_MSVC_STATIC_RT ON)
    set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

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
if(MINIDRIVE_BUILD_SERVER)
    add_subdirectory(server)
endif()
if(MINIDRIVE_BUILD_CLIENT)
    add_subdirectory(client)
endif()

if(MINIDRIVE_BUILD_TESTS)
    # Must be called at the top level: CTest only generates a root build/CTestTestfile.cmake
    # (what `ctest --test-dir build` reads) when enable_testing() runs here, not just inside
    # the tests/ subdirectory - otherwise `ctest` silently reports "No tests were found!!!"
    # even though tests/CTestTestfile.cmake was generated correctly one level down.
    enable_testing()
    add_subdirectory(tests)
endif()

include(Packaging)

