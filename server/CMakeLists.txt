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

minidrive_static_link(minidrive_server)

install(TARGETS minidrive_server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT server)
install(FILES
        ${PROJECT_SOURCE_DIR}/README.md
        ${PROJECT_SOURCE_DIR}/docs/tls.md
        ${PROJECT_SOURCE_DIR}/docs/vault.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/minidrive-server
    COMPONENT server)
# The operator needs a CA and a server certificate before --rung 3.5 can start at all.
install(PROGRAMS ${PROJECT_SOURCE_DIR}/lab/gen-certs.sh
    DESTINATION ${CMAKE_INSTALL_DATADIR}/minidrive
    COMPONENT server)

