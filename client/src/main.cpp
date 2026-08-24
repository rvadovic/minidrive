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
#include "transport/tls.hpp"

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
        std::cerr << "Usage: " << argv[0] << " [username@]<host>:<port> [--log <log_file>] [--log-level <level>]"
                  << " [--rung <0|3.5>] [--tls-verify <none|ca|pinned>] [--ca-file <pem>]"
                  << " [--pin <sha256:hex>] [--tls-servername <name>] [--tls-min-version <1.2|1.3>]"
                  << " [--tls-ciphers <list>] [--tls-groups <list>] [--tls-require-pq]" << std::endl;
        return 1;
    }

    UserHostPort hp;
    if (!parse_host_port(argv[1], hp)) {
        std::cerr << "Invalid endpoint format: " << argv[1] << std::endl;
        return 1;
    }

    std::string log_file;
    std::string log_level_str = "info";
    transport::Rung rung = transport::Rung::Plain;
    transport::TlsClientConfig tls;
    bool verify_given = false;

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
        } else if (arg == "--rung") {
            if (i + 1 >= argc) {
                std::cerr << "--rung requires a value (0 or 3.5)\n";
                return 1;
            }
            std::string error;
            if (!transport::parse_rung(argv[++i], rung, error)) {
                std::cerr << error << std::endl;
                return 1;
            }
        } else if (arg == "--tls-verify") {
            if (i + 1 >= argc) {
                std::cerr << "--tls-verify requires a value (none|ca|pinned)\n";
                return 1;
            }
            std::string value = argv[++i];
            if (value == "none") tls.verify = transport::PeerVerify::None;
            else if (value == "ca") tls.verify = transport::PeerVerify::Ca;
            else if (value == "pinned") tls.verify = transport::PeerVerify::Pinned;
            else {
                std::cerr << "--tls-verify expects none, ca or pinned, got: " << value << std::endl;
                return 1;
            }
            verify_given = true;
        } else if (arg == "--ca-file") {
            if (i + 1 >= argc) {
                std::cerr << "--ca-file requires a path to a PEM certificate authority\n";
                return 1;
            }
            tls.ca_file = argv[++i];
        } else if (arg == "--pin") {
            if (i + 1 >= argc) {
                std::cerr << "--pin requires a SHA-256 public key pin (sha256:<hex>)\n";
                return 1;
            }
            tls.pin = argv[++i];
        } else if (arg == "--tls-servername") {
            if (i + 1 >= argc) {
                std::cerr << "--tls-servername requires a name\n";
                return 1;
            }
            tls.server_name = argv[++i];
        } else if (arg == "--tls-min-version") {
            if (i + 1 >= argc) {
                std::cerr << "--tls-min-version requires a value (1.2 or 1.3)\n";
                return 1;
            }
            tls.min_version = argv[++i];
        } else if (arg == "--tls-ciphers") {
            if (i + 1 >= argc) {
                std::cerr << "--tls-ciphers requires a cipher list\n";
                return 1;
            }
            tls.ciphers = argv[++i];
        } else if (arg == "--tls-groups") {
            if (i + 1 >= argc) {
                std::cerr << "--tls-groups requires a group list, e.g. X25519MLKEM768:X25519\n";
                return 1;
            }
            tls.groups = argv[++i];
        } else if (arg == "--tls-require-pq") {
            tls.require_pq = true;
        } else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            return 1;
        }
    }

    // A pin is only ever given in order to be enforced, so supplying one selects the pinned
    // posture unless --tls-verify explicitly said otherwise.
    if (!tls.pin.empty() && !verify_given) {
        tls.verify = transport::PeerVerify::Pinned;
    }
    // The certificate is checked against whatever name the user typed, unless --tls-servername
    // overrides it (connecting to an IP whose certificate only carries a DNS name).
    if (tls.server_name.empty()) {
        tls.server_name = hp.host;
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

    std::shared_ptr<transport::StreamFactory> streams;
    if (rung == transport::Rung::Plain) {
        // Silently ignoring these would let someone believe a connection is verified when it is
        // not even encrypted, which is the exact misunderstanding the rungs exist to prevent.
        if (!tls.ca_file.empty() || !tls.pin.empty() || verify_given) {
            std::cerr << "warning: TLS options were given but --rung is 0, so the connection is "
                         "plaintext and none of them apply. Pass --rung 3.5 to use them."
                      << std::endl;
        }
        streams = transport::make_plain_factory();
    } else {
        std::string error;
        std::vector<std::string> warnings;
        streams = transport::make_tls_client_factory(tls, error, warnings);
        // Warnings go to stderr as well as the log: a client running with certificate checking
        // switched off should say so where the person running it can see it.
        for (const auto& warning : warnings) {
            spdlog::warn("{}", warning);
            std::cerr << "warning: " << warning << std::endl;
        }
        if (!streams) {
            spdlog::critical("TLS setup failed: {}", error);
            std::cerr << "TLS setup failed: " << error << std::endl;
            return 1;
        }
    }
    spdlog::info("Transport: {}", streams->describe());

    asio::io_context io_context;
    auto work_guard = std::make_shared<asio::executor_work_guard<asio::io_context::executor_type>>(asio::make_work_guard(io_context));

    Client client(hp.username, io_context, work_guard, streams);
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
