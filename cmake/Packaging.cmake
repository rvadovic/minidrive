# CPack configuration for the release assets.
#
# The server and the client are packaged SEPARATELY (CPack components "server" and "client", from
# the install() rules in server/CMakeLists.txt and client/CMakeLists.txt), because they are deployed
# in opposite ways:
#   - the server runs on one known target, Debian 13, where apt keeps its dynamically linked libssl
#     patched: it ships as a .deb (built on debian:13, Depends: derived by dpkg-shlibdeps);
#   - the client runs on whatever the user has, and must be one downloaded file that runs: it ships
#     as a .tar.gz (Linux, macOS) or .zip (Windows) of a MINIDRIVE_FULLY_STATIC build.
# The server's other release asset, the source tarball, is made by `git archive` in release.yml,
# not by CPack.
#
# Asset names follow minidrive-<component>-<version>-<platform>.<ext>. Both variable parts can be set
# at configure time so the release workflow names assets after the tag being released:
#   -DMINIDRIVE_PACKAGE_VERSION=0.6.0-rc1       (default: the project version)
#   -DMINIDRIVE_PACKAGE_PLATFORM=debian13-amd64  (default: derived from the build host)
#
# A tree that builds only one half packages only that half: `cpack -G TGZ` in a client-only build
# tree produces exactly one archive.

set(MINIDRIVE_PACKAGE_VERSION "${MiniDrive_VERSION}" CACHE STRING
    "Version string used in release asset names (the release workflow passes the tag without its 'v')")

string(TOLOWER "${CMAKE_SYSTEM_NAME}" _md_os)
if(_md_os STREQUAL "darwin")
    set(_md_os "macos")
endif()
# Normalise the processor to one spelling per architecture: Windows reports AMD64, Linux x86_64,
# macOS arm64 vs. Linux aarch64. An explicit CMAKE_OSX_ARCHITECTURES (cross-building) wins.
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    string(REPLACE ";" "-" _md_arch "${CMAKE_OSX_ARCHITECTURES}")
else()
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _md_arch)
endif()
if(_md_arch STREQUAL "amd64")
    set(_md_arch "x86_64")
elseif(_md_arch STREQUAL "aarch64")
    set(_md_arch "arm64")
endif()
set(MINIDRIVE_PACKAGE_PLATFORM "${_md_os}-${_md_arch}" CACHE STRING
    "Platform string used in release asset names, e.g. linux-x86_64 or debian13-amd64")

set(CPACK_PACKAGE_NAME "minidrive")
set(CPACK_PACKAGE_VENDOR "MiniDrive")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "MiniDrive file-sync server and client")
set(CPACK_PACKAGE_VERSION_MAJOR "${MiniDrive_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${MiniDrive_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${MiniDrive_VERSION_PATCH}")
set(CPACK_PACKAGE_VERSION "${MINIDRIVE_PACKAGE_VERSION}")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
# Fallback name, only used if a generator is ever run without component packaging.
set(CPACK_PACKAGE_FILE_NAME "minidrive-${MINIDRIVE_PACKAGE_VERSION}-${MINIDRIVE_PACKAGE_PLATFORM}")

if(WIN32)
    set(CPACK_GENERATOR "ZIP")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CPACK_GENERATOR "TGZ;DEB")
else()
    set(CPACK_GENERATOR "TGZ")
endif()

# --- components -------------------------------------------------------------------------------

set(CPACK_COMPONENTS_ALL "")
if(MINIDRIVE_BUILD_SERVER)
    list(APPEND CPACK_COMPONENTS_ALL server)
endif()
if(MINIDRIVE_BUILD_CLIENT)
    list(APPEND CPACK_COMPONENTS_ALL client)
endif()
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
# Archives get a top-level directory, .debs must not - set per generator in this file.
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/CPackProjectConfig.cmake")

# That top-level directory is named after CPACK_PACKAGE_FILE_NAME, not the component. A release
# build tree packages exactly one component, so name it after that one: the client archive then
# unpacks to minidrive-client-<ver>-<platform>/, matching its own file name.
list(LENGTH CPACK_COMPONENTS_ALL _md_ncomp)
if(_md_ncomp EQUAL 1)
    set(CPACK_PACKAGE_FILE_NAME
        "minidrive-${CPACK_COMPONENTS_ALL}-${MINIDRIVE_PACKAGE_VERSION}-${MINIDRIVE_PACKAGE_PLATFORM}")
endif()
set(CPACK_DEB_COMPONENT_INSTALL ON)

set(CPACK_COMPONENT_SERVER_DISPLAY_NAME "MiniDrive server")
set(CPACK_COMPONENT_SERVER_DESCRIPTION
    "MiniDrive file-sync server: public and private modes, storage tiers, TLS 1.3 with the hybrid post-quantum group X25519MLKEM768.")
set(CPACK_COMPONENT_CLIENT_DISPLAY_NAME "MiniDrive client")
set(CPACK_COMPONENT_CLIENT_DESCRIPTION
    "MiniDrive client: transfers, two-way SYNC, resume, end-to-end encrypted vault, and a headless --ipc mode.")

foreach(_md_comp IN ITEMS server client)
    string(TOUPPER "${_md_comp}" _md_COMP)
    set(CPACK_ARCHIVE_${_md_COMP}_FILE_NAME
        "minidrive-${_md_comp}-${MINIDRIVE_PACKAGE_VERSION}-${MINIDRIVE_PACKAGE_PLATFORM}")
    set(CPACK_DEBIAN_${_md_COMP}_FILE_NAME
        "minidrive-${_md_comp}-${MINIDRIVE_PACKAGE_VERSION}-${MINIDRIVE_PACKAGE_PLATFORM}.deb")
    set(CPACK_DEBIAN_${_md_COMP}_PACKAGE_NAME "minidrive-${_md_comp}")
endforeach()

# --- Debian -----------------------------------------------------------------------------------

# A pre-release tag (0.6.0-rc1) must sort *before* the release it precedes; in Debian versions that
# is what '~' means, while '-' would be read as a Debian revision.
string(REPLACE "-" "~" CPACK_DEBIAN_PACKAGE_VERSION "${MINIDRIVE_PACKAGE_VERSION}")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "MiniDrive project")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/rvadovic/minidrive")
# The server links libssl/libcrypto dynamically on purpose: on its target (Debian 13) apt and
# unattended-upgrades keep them patched, which an embedded copy would forfeit. dpkg-shlibdeps works
# the dependency out from the built binary (libssl3t64 on trixie) rather than a version guessed
# here, and a static client produces no dependency at all - which it notices too.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

include(CPack)
