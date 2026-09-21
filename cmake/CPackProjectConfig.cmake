# Read by CPack once per generator (CPACK_PROJECT_CONFIG_FILE), so settings here can differ between
# generators, which Packaging.cmake alone cannot do.

# Archives unpack into one directory named after the download. A .deb must not: its paths are
# installed relative to /, so a top-level directory would land as /minidrive-.../usr/bin.
if(CPACK_GENERATOR MATCHES "^(TGZ|TXZ|ZIP)$")
    set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY ON)
else()
    set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY OFF)
endif()
