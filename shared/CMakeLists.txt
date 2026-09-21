add_library(minidrive_shared STATIC
    src/version.cpp
    src/minidrive/logging.cpp
    src/protocol/message.cpp
    src/crypto/base64.cpp
    src/filesystem/utils.cpp
    src/filesystem/partmeta.cpp
    src/transport/stream.cpp
    src/transport/tls.cpp
)

target_include_directories(minidrive_shared
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(minidrive_shared
    PUBLIC
        asio::asio
        nlohmann_json::nlohmann_json
        libsodium::libsodium
        spdlog::spdlog_header_only
    PUBLIC
        minidrive_warnings
)

# TlsStream compiles only where OpenSSL was found. PUBLIC so the define reaches server/client too:
# transport/stream.hpp guards the class declaration on it, and a mismatch between the library's
# view of the header and an executable's would be an ODR violation, not a missing symbol.
if(MINIDRIVE_ENABLE_TLS)
    # CMAKE_DL_LIBS: transport/stream.cpp resolves SSL_get0_group_name with dlsym, so that a binary
    # built against an older OpenSSL can still name a group negotiated by a newer libssl at runtime
    # (empty on platforms where dlsym lives in libc).
    target_link_libraries(minidrive_shared PUBLIC OpenSSL::SSL OpenSSL::Crypto ${CMAKE_DL_LIBS})
    target_compile_definitions(minidrive_shared PUBLIC MINIDRIVE_ENABLE_TLS)
endif()

# Windows (the headless client only - the server is Linux-only). PUBLIC so every translation unit
# that includes a shared header sees the same <windows.h> configuration:
#   NOMINMAX            - otherwise min/max are macros and std::numeric_limits<T>::max() breaks
#   WIN32_LEAN_AND_MEAN - keeps winsock.h out, which would otherwise fight asio's winsock2.h
#   NOGDI               - wingdi.h defines ERROR, which collides with protocol::statuses/flags::ERROR
#   _WIN32_WINNT        - asio otherwise assumes Windows 7 and warns in every translation unit
# DELETE (winnt.h) cannot be switched off this way; protocol/win32_macros.hpp removes it.
# ws2_32/mswsock: asio's socket layer. MSVC auto-links them through #pragma comment, MinGW does not.
if(WIN32)
    target_compile_definitions(minidrive_shared PUBLIC
        NOMINMAX WIN32_LEAN_AND_MEAN NOGDI _WIN32_WINNT=0x0A00)
    target_link_libraries(minidrive_shared PUBLIC ws2_32 mswsock)
endif()

set_target_properties(minidrive_shared PROPERTIES EXPORT_NAME shared)
