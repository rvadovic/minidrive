#include <iostream>
#include <string>
#include <cstdint>
#include <asio.hpp>

#include "minidrive/version.hpp"
#include "server.hpp"
#include "filesystem/utils.hpp"
#include <filesystem>
#include <sodium.h>

int main(int argc, char* argv[]) {
    // Echo full command line once for diagnostics
    std::cout << "[cmd]";
    for (int i = 0; i < argc; ++i) {
        std::cout << " \"" << argv[i] << '"';
    }
    std::cout << std::endl;
    std::uint16_t port = 9000;
    std::filesystem::path root;
    bool root_provided =  false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--port") {
            if (i + 1 >= argc) {
                std::cerr << "--port requires a value\n";
                return 1;
            }
            port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        }
        else if (arg == "--root") {
            if (i + 1 >= argc) {
                std::cerr << "--root requires a path\n";
                return 1;
            }
            root = std::filesystem::path(argv[++i]);
            root_provided = true;

            if (!fsutils::exists(root)) {
                std::cerr << "Root path does not exist: " << root << std::endl;
                return 1;
            }
        }
        else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            return 1;
        }
    }

    if (!root_provided) {
        std::cerr << "Usage: " << argv[0] << " [--port <port>] --root <root_path>\n";
        return 1;
    }

    if (!fsutils::exists(root)) {
                std::cerr << "Usage: " << argv[0] << " [--port <port>] [--root <root_path>]" << std::endl;
                return 1;
            }


    if (sodium_init() < 0) {
        std::cerr << "libsodium failed to initialize\n";
        return 1;
    }

    asio::io_context io_context;
    Server server(io_context, port, root);
    std::cout << "Starting async server (version " << minidrive::version() << ") on port " << port << std::endl;
    server.start();
    io_context.run();
    std::cout << "Server exited." << std::endl;
    return 0;
}
