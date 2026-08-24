add_library(minidrive_shared STATIC
    src/version.cpp
    src/minidrive/logging.cpp
    src/protocol/message.cpp
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

set_target_properties(minidrive_shared PROPERTIES EXPORT_NAME shared)
