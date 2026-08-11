#include <iostream>
#include <string>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <asio.hpp>
#include <sodium.h>
#include <spdlog/spdlog.h>

#include "minidrive/version.hpp"
#include "minidrive/logging.hpp"
#include "client.hpp"

struct UserHostPort {
    std::string username;
    std::string host;
    uint16_t port{};
};

// parse username host and port
static bool parse_host_port(const std::string& input, UserHostPort& out) {
    auto colon = input.rfind(':');
    auto at = input.rfind("@");

    if (colon == std::string::npos) return false;
    
    std::string username("");
    std::string host;

    if(!(at == std::string::npos)) {
        username = input.substr(0, at);
        host = input.substr(at + 1, colon - (at + 1));
    } else {
        host = input.substr(0, colon);
    }

    std::string port_str = input.substr(colon + 1);

    if (host.empty() || port_str.empty()) return false;

    char* end = nullptr;
    long p = std::strtol(port_str.c_str(), &end, 10);
    if (*end != '\0' || p < 0 || p > 65535) return false;
    out.username = std::move(username);
    out.host = std::move(host);
    out.port = static_cast<uint16_t>(p);
    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " [username@]<host>:<port> [--log <log_file>] [--log-level <level>]" << std::endl;
        return 1;
    }

    UserHostPort hp;
    if (!parse_host_port(argv[1], hp)) {
        std::cerr << "Invalid endpoint format: " << argv[1] << std::endl;
        return 1;
    }

    std::string log_file;
    std::string log_level_str = "info";
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--log") {
            if (i + 1 >= argc) {
                std::cerr << "--log requires a path\n";
                return 1;
            }
            log_file = argv[++i];
        } else if (arg == "--log-level") {
            if (i + 1 >= argc) {
                std::cerr << "--log-level requires a value (trace|debug|info|warn|error|critical|off)\n";
                return 1;
            }
            log_level_str = argv[++i];
        } else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            return 1;
        }
    }

    // libsodium init
    if (sodium_init() < 0) {
        std::cerr << "libsodium failed to initialize\n";
        return 1;
    }

    // File sink ONLY, never console: client stdout is the OK/ERROR protocol contract callers and
    // tests parse (see docs/protocol.md) - a stray log line there would corrupt it. With no --log
    // flag, log_file is empty and logging is a no-op (see minidrive::log::init).
    minidrive::log::init("client", log_file, minidrive::log::level_from_string(log_level_str), /*also_console=*/false);
    spdlog::debug("MiniDrive client {} connecting to {}:{}", minidrive::resolved_version(), hp.host, hp.port);

    asio::io_context io_context;
    auto work_guard = std::make_shared<asio::executor_work_guard<asio::io_context::executor_type>>(asio::make_work_guard(io_context));
    
    Client client(hp.username, io_context, work_guard);
    client.connect(hp.host, hp.port);

    // handle SIGINT, SIGTERM
    asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait([&](const std::error_code& ec, int) {
        std::cout << std::endl << "Signal received, shutting down..." << std::endl;
        client.exit();
    });

    // network thread
    std::thread net_thread([&](){
        try {
            io_context.run();
        } catch (const std::exception& e) {
            std::cerr << "IO thread exception: " << e.what() << std::endl;
        }
    });
    
    net_thread.join();

    std::cout << "Client closed" << std::endl;
}
