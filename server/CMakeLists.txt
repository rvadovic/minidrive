add_executable(minidrive_server
    src/main.cpp
    src/server.cpp
    src/session.cpp
    src/password.cpp
    src/database.cpp
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

