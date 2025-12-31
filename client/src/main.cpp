#include <iostream>
#include <string>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <asio.hpp>
#include <sodium.h>

#include "minidrive/version.hpp"
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
    // Echo full command line once for diagnostics
    std::cout << "[cmd]";
    for (int i = 0; i < argc; ++i) {
        std::cout << " \"" << argv[i] << '"';
    }
    std::cout << std::endl;

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <host>:<port>" << std::endl;
        return 1;
    }

    UserHostPort hp;
    if (!parse_host_port(argv[1], hp)) {
        std::cerr << "Invalid endpoint format: " << argv[1] << std::endl;
        return 1;
    }

    std::cout << "MiniDrive client (version " << minidrive::version() << ")" << std::endl;
    std::cout << "Connecting to " << hp.host << ':' << hp.port << std::endl;

    // libsodium init
    if (sodium_init() < 0) {
        std::cerr << "libsodium failed to initialize\n";
        return 1;
    }

    asio::io_context io_context;
    
    Client client(hp.username, io_context);
    client.connect(hp.host, hp.port);

    // keep io_context alive
    asio::executor_work_guard<asio::io_context::executor_type> guard(io_context.get_executor());

    // handle SIGINT
    asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait([&](const std::error_code& ec, int) {
        std::cout << std::endl << "SIGINT received, shutting down..." << std::endl;
        client.exit();
        guard.reset(); // allow io-context.run() to return
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
