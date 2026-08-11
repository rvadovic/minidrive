# CPack configuration for downloadable release archives.
#
# Deliberately packages only the built server/client binaries (via the install() rules in
# server/CMakeLists.txt and client/CMakeLists.txt) - no source, no test suite, no dev-container or
# planning files. Two generators:
#   TGZ - a self-contained "download and run" archive for any Linux distro.
#   DEB - installs the binaries to /usr/bin via dpkg/apt on Debian-family systems.
# Both ship the same statically-runtime-linked binaries built above (MINIDRIVE_STATIC_RUNTIME);
# there is no separate "static" vs "dynamic" package variant.

set(CPACK_PACKAGE_NAME "minidrive")
set(CPACK_PACKAGE_VENDOR "MiniDrive")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "MiniDrive file-sync server and CLI client")
set(CPACK_PACKAGE_VERSION_MAJOR "${MiniDrive_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${MiniDrive_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${MiniDrive_VERSION_PATCH}")
set(CPACK_PACKAGE_FILE_NAME "minidrive-${MiniDrive_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

set(CPACK_GENERATOR "TGZ;DEB")

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "MiniDrive project")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/rvadovic/minidrive")
# Statically-linked libstdc++/libgcc plus header-only dependencies (Asio, nlohmann/json, spdlog)
# and a from-source libsodium build mean the resulting binaries carry no shared-library
# dependency beyond glibc/libc, which every supported Debian-family release already satisfies -
# so no explicit Depends: list is needed here.

include(CPack)