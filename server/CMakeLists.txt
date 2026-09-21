add_executable(minidrive_server
    src/main.cpp
    src/server.cpp
    src/session.cpp
    src/password.cpp
    src/database.cpp
    src/storage.cpp
    src/vault.cpp
    src/user_lock.cpp
    src/path_guard.cpp
    src/auth_limiter.cpp
)

target_include_directories(minidrive_server
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(minidrive_server
    PRIVATE
        asio
        minidrive_shared
        minidrive_warnings
)

set_target_properties(minidrive_server PROPERTIES OUTPUT_NAME server)

# See the note in client/CMakeLists.txt: Apple's clang has no static libc++ to link against.
if(MINIDRIVE_STATIC_RUNTIME AND NOT MSVC AND NOT APPLE)
    target_link_options(minidrive_server PRIVATE -static-libgcc -static-libstdc++)
endif()

install(TARGETS minidrive_server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

