# The interactive terminal UI is POSIX-only (termios, asio::posix::stream_descriptor, dup(stdin))
# and deliberately stays that way. Rather than abstract it per platform, a headless build simply
# does not compile it: the client is then driven over --ipc, which is what the desktop GUI does on
# every platform anyway. Windows gets a headless client and nothing is lost there.
option(MINIDRIVE_INTERACTIVE_CLI
    "Build the interactive terminal UI (POSIX only). Off means a headless, --ipc-only client."
    ON)

if(WIN32 AND MINIDRIVE_INTERACTIVE_CLI)
    message(STATUS "MiniDrive: interactive CLI is POSIX-only; building a headless (--ipc) client for Windows")
    set(MINIDRIVE_INTERACTIVE_CLI OFF)
endif()

set(MINIDRIVE_CLIENT_SOURCES
    src/main.cpp
    src/client.cpp
    src/ipc_io.cpp
    src/sync_manifest.cpp
    src/sync_diff.cpp
    src/vault.cpp
)

if(MINIDRIVE_INTERACTIVE_CLI)
    list(APPEND MINIDRIVE_CLIENT_SOURCES
        src/terminal_io.cpp
        src/terminalNoEcho.cpp
        src/terminalRaw.cpp
    )
endif()

add_executable(minidrive_client ${MINIDRIVE_CLIENT_SOURCES})

target_include_directories(minidrive_client
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(MINIDRIVE_INTERACTIVE_CLI)
    target_compile_definitions(minidrive_client PRIVATE MINIDRIVE_INTERACTIVE_CLI)
endif()

target_link_libraries(minidrive_client
    PRIVATE
        asio
        minidrive_shared
        minidrive_warnings
)

set_target_properties(minidrive_client PROPERTIES OUTPUT_NAME client)

# Not on Apple: clang there has no static libc++ to link against, and passing these makes the link
# fail rather than producing a more self-contained binary.
if(MINIDRIVE_STATIC_RUNTIME AND NOT MSVC AND NOT APPLE)
    target_link_options(minidrive_client PRIVATE -static-libgcc -static-libstdc++)
endif()

install(TARGETS minidrive_client RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
