include(FetchContent)

# Asio (header-only)
FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-36-0
)
FetchContent_MakeAvailable(asio)
add_library(asio INTERFACE)
target_include_directories(asio SYSTEM INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(asio INTERFACE ASIO_STANDALONE)
add_library(asio::asio ALIAS asio)

# nlohmann::json (header-only)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
)
set(JSON_SystemInclude ON CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# spdlog (optional)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.16.0
)
FetchContent_MakeAvailable(spdlog)

set(SODIUM_DISABLE_TESTS ON)

# libsodium
FetchContent_Declare(
    libsodium
    GIT_REPOSITORY https://github.com/robinlinden/libsodium-cmake.git
    GIT_TAG 260622e5b69bce9b955603a98e46354125a932a4 # libsodium version 1.0.20-RELEASE
)
FetchContent_MakeAvailable(libsodium)
if(NOT TARGET libsodium::libsodium)
    add_library(libsodium::libsodium ALIAS sodium)
    # Mark sodium includes as system to suppress warnings
    get_target_property(sodium_include_dirs sodium INTERFACE_INCLUDE_DIRECTORIES)
    if(sodium_include_dirs)
        set_target_properties(sodium PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${sodium_include_dirs}")
    endif()
endif()

# OpenSSL - asio::ssl wraps it directly, so it is the whole crypto stack for the transport rungs
# (TLS 1.3, chain validation, pinning, and the hybrid X25519MLKEM768 group). Deliberately NOT
# REQUIRED: rung 0 runs on PlainStream and must keep building on a machine or CI image without
# libssl-dev. Rung 5's hybrid post-quantum group needs OpenSSL 3.5+ (which ships ML-KEM natively -
# no liboqs); anything older still builds and gives TLS 1.3 without that group.
option(MINIDRIVE_ENABLE_TLS "Build the TLS transport (requires OpenSSL)" ON)

if(MINIDRIVE_FULLY_STATIC AND NOT MINIDRIVE_ENABLE_TLS)
    message(FATAL_ERROR "MINIDRIVE_FULLY_STATIC embeds OpenSSL; it cannot be combined with "
                        "MINIDRIVE_ENABLE_TLS=OFF")
endif()

if(MINIDRIVE_ENABLE_TLS)
    find_package(OpenSSL)
    if(MINIDRIVE_FULLY_STATIC)
        # A self-contained client that quietly came out rung-0-only, or without the hybrid group,
        # would be published as if it were the real thing. Refuse instead of degrading: every
        # client download is meant to negotiate X25519MLKEM768 and to support ENROLL_DEVICE.
        if(NOT OpenSSL_FOUND)
            message(FATAL_ERROR "MINIDRIVE_FULLY_STATIC: no static OpenSSL found (set OPENSSL_ROOT_DIR "
                                "to a no-shared OpenSSL 3.5+ install)")
        elseif(OPENSSL_VERSION VERSION_LESS 3.5.0)
            message(FATAL_ERROR "MINIDRIVE_FULLY_STATIC: found OpenSSL ${OPENSSL_VERSION}, need 3.5+ "
                                "(ML-KEM for the hybrid group and for vault device enrollment)")
        endif()
        message(STATUS "OpenSSL ${OPENSSL_VERSION} (static: ${OPENSSL_SSL_LIBRARY}) - embedded")
    elseif(NOT OpenSSL_FOUND)
        message(STATUS "OpenSSL not found - building without the TLS transport (rung 0 only)")
        set(MINIDRIVE_ENABLE_TLS OFF)
    elseif(OPENSSL_VERSION VERSION_LESS 3.5.0)
        message(STATUS "OpenSSL ${OPENSSL_VERSION} found - TLS transport enabled, but the hybrid "
                       "post-quantum group X25519MLKEM768 needs OpenSSL 3.5+")
    else()
        message(STATUS "OpenSSL ${OPENSSL_VERSION} found - TLS transport enabled")
    endif()
endif()

# Shared by both executables' CMakeLists: link a fully static client (Linux) or keep only the C++
# runtime static (MINIDRIVE_STATIC_RUNTIME, the server's setting). Apple is excluded from both -
# there is no static libc++ or libSystem to link, and passing these makes the link fail.
function(minidrive_static_link target)
    if(MSVC OR APPLE)
        return()
    endif()
    if(MINIDRIVE_FULLY_STATIC)
        # Only portable against musl: a static glibc still dlopen()s NSS modules for getaddrinfo at
        # runtime, so the binary would break on a host with a different glibc. The release builds
        # this on Alpine for exactly that reason; a glibc -static build links (with warnings) and
        # is fine for testing on the machine that built it.
        target_link_options(${target} PRIVATE -static)
    elseif(MINIDRIVE_STATIC_RUNTIME)
        target_link_options(${target} PRIVATE -static-libgcc -static-libstdc++)
    endif()
endfunction()

# Helper interface library for shared warning flags
add_library(minidrive_warnings INTERFACE)
if(MSVC)
    target_compile_options(minidrive_warnings INTERFACE
        /W4 /permissive- /Zc:__cplusplus /EHsc
    )
else()
    target_compile_options(minidrive_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
endif()
