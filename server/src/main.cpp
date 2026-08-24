#include <iostream>
#include <string>
#include <cstdint>
#include <asio.hpp>
#include <spdlog/spdlog.h>

#include "minidrive/version.hpp"
#include "minidrive/logging.hpp"
#include "server.hpp"
#include "tier_config.hpp"
#include "filesystem/utils.hpp"
#include "transport/tls.hpp"
#include <filesystem>
#include <sodium.h>
#include <sys/stat.h>

namespace {

// Splits a "name=value" command line argument. False when there is no '=' or either side is empty.
bool split_pair(const std::string& arg, std::string& name, std::string& value) {
    auto eq = arg.find('=');
    if(eq == std::string::npos || eq == 0 || eq + 1 >= arg.size()) return false;
    name = arg.substr(0, eq);
    value = arg.substr(eq + 1);
    return true;
}

// Tier names travel to clients and end up in users.json, so keep them short and printable
bool valid_tier_name(const std::string& name) {
    if(name.empty() || name.size() > 32) return false;
    for(char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if(!ok) return false;
    }
    return true;
}

// Two tiers on the same filesystem defeat the point of tiering, but the integration tests
// legitimately do it, so this only warns. Server is Linux only, so stat() is fine here.
void warn_on_shared_devices(const std::vector<StorageTier>& tiers) {
    for(size_t i = 0; i < tiers.size(); ++i) {
        for(size_t k = i + 1; k < tiers.size(); ++k) {
            struct stat a{};
            struct stat b{};
            if(::stat(tiers[i].path.c_str(), &a) != 0) continue;
            if(::stat(tiers[k].path.c_str(), &b) != 0) continue;
            if(a.st_dev == b.st_dev) {
                spdlog::warn("Tiers '{}' and '{}' are on the same filesystem; they are not separate media.",
                             tiers[i].name, tiers[k].name);
            }
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    std::uint16_t port = 9000;
    std::filesystem::path root;
    bool root_provided =  false;
    std::vector<StorageTier> tiers;
    std::vector<std::pair<std::string, std::string>> tier_descriptions;
    std::string default_tier;
    std::string log_file;
    std::string log_level_str = "info";
    // Rung 0 is the default: the whole integration suite runs on it, and turning on TLS is an
    // explicit act with certificates behind it.
    transport::Rung rung = transport::Rung::Plain;
    transport::TlsServerConfig tls;

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
        else if (arg == "--tier") {
            if (i + 1 >= argc) {
                std::cerr << "--tier requires <name>=<path>\n";
                return 1;
            }
            std::string name;
            std::string path;
            if (!split_pair(argv[++i], name, path)) {
                std::cerr << "--tier expects <name>=<path>, got: " << argv[i] << std::endl;
                return 1;
            }
            if (!valid_tier_name(name)) {
                std::cerr << "Invalid tier name '" << name
                          << "': use 1-32 characters from A-Z a-z 0-9 _ -" << std::endl;
                return 1;
            }
            for (const auto& existing : tiers) {
                if (existing.name == name) {
                    std::cerr << "Duplicate tier name: " << name << std::endl;
                    return 1;
                }
            }
            if (!fsutils::is_directory(std::filesystem::path(path))) {
                std::cerr << "Tier path is not an existing directory: " << path << std::endl;
                return 1;
            }
            tiers.push_back(StorageTier{name, std::filesystem::path(path), std::string()});
        }
        else if (arg == "--tier-desc") {
            if (i + 1 >= argc) {
                std::cerr << "--tier-desc requires <name>=<description>\n";
                return 1;
            }
            std::string name;
            std::string description;
            if (!split_pair(argv[++i], name, description)) {
                std::cerr << "--tier-desc expects <name>=<description>, got: " << argv[i] << std::endl;
                return 1;
            }
            tier_descriptions.emplace_back(name, description);
        }
        else if (arg == "--default-tier") {
            if (i + 1 >= argc) {
                std::cerr << "--default-tier requires a tier name\n";
                return 1;
            }
            default_tier = argv[++i];
        }
        else if (arg == "--rung") {
            if (i + 1 >= argc) {
                std::cerr << "--rung requires a value (0 or 3.5)\n";
                return 1;
            }
            std::string error;
            if (!transport::parse_rung(argv[++i], rung, error)) {
                std::cerr << error << std::endl;
                return 1;
            }
        }
        else if (arg == "--tls-cert") {
            if (i + 1 >= argc) {
                std::cerr << "--tls-cert requires a path to a PEM certificate chain\n";
                return 1;
            }
            tls.cert_file = argv[++i];
        }
        else if (arg == "--tls-key") {
            if (i + 1 >= argc) {
                std::cerr << "--tls-key requires a path to a PEM private key\n";
                return 1;
            }
            tls.key_file = argv[++i];
        }
        else if (arg == "--tls-min-version") {
            if (i + 1 >= argc) {
                std::cerr << "--tls-min-version requires a value (1.2 or 1.3)\n";
                return 1;
            }
            tls.min_version = argv[++i];
        }
        else if (arg == "--tls-ciphers") {
            if (i + 1 >= argc) {
                std::cerr << "--tls-ciphers requires a cipher list\n";
                return 1;
            }
            tls.ciphers = argv[++i];
        }
        else if (arg == "--tls-groups") {
            if (i + 1 >= argc) {
                std::cerr << "--tls-groups requires a group list, e.g. X25519MLKEM768:X25519\n";
                return 1;
            }
            tls.groups = argv[++i];
        }
        else if (arg == "--tls-require-pq") {
            tls.require_pq = true;
        }
        else if (arg == "--log-file") {
            if (i + 1 >= argc) {
                std::cerr << "--log-file requires a path\n";
                return 1;
            }
            log_file = argv[++i];
        }
        else if (arg == "--log-level") {
            if (i + 1 >= argc) {
                std::cerr << "--log-level requires a value (trace|debug|info|warn|error|critical|off)\n";
                return 1;
            }
            log_level_str = argv[++i];
        }
        else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            return 1;
        }
    }

    if (!root_provided) {
        std::cerr << "Usage: " << argv[0] << " [--port <port>] --root <root_path>"
                  << " [--tier <name>=<path>]... [--tier-desc <name>=<text>]..."
                  << " [--default-tier <name>] [--log-file <path>] [--log-level <level>]"
                  << " [--rung <0|3.5>] [--tls-cert <pem>] [--tls-key <pem>]"
                  << " [--tls-min-version <1.2|1.3>] [--tls-ciphers <list>] [--tls-groups <list>]"
                  << " [--tls-require-pq]\n";
        return 1;
    }

    // No --tier flags means the classic single root layout: one implicit tier pointing at it
    if (tiers.empty()) {
        tiers.push_back(StorageTier{"hot", root, "default storage"});
    }

    for (const auto& [name, description] : tier_descriptions) {
        bool applied = false;
        for (auto& tier : tiers) {
            if (tier.name == name) {
                tier.description = description;
                applied = true;
            }
        }
        if (!applied) {
            std::cerr << "--tier-desc names an undeclared tier: " << name << std::endl;
            return 1;
        }
    }

    if (default_tier.empty()) default_tier = "hot";

    {
        bool known = false;
        for (const auto& tier : tiers) {
            if (tier.name == default_tier) known = true;
        }
        if (!known) {
            std::cerr << "Default tier '" << default_tier << "' was not declared with --tier."
                      << " Pass --default-tier <name> naming one of the configured tiers." << std::endl;
            return 1;
        }
    }

    // Nested tiers would make a migration copy a tree into itself
    for (size_t i = 0; i < tiers.size(); ++i) {
        for (size_t k = i + 1; k < tiers.size(); ++k) {
            if (fsutils::is_subpath(tiers[i].path, tiers[k].path) ||
                fsutils::is_subpath(tiers[k].path, tiers[i].path)) {
                std::cerr << "Tier '" << tiers[i].name << "' and tier '" << tiers[k].name
                          << "' overlap on disk; tier paths must be separate directories." << std::endl;
                return 1;
            }
        }
    }

    if (sodium_init() < 0) {
        std::cerr << "libsodium failed to initialize\n";
        return 1;
    }

    minidrive::log::init("server", log_file, minidrive::log::level_from_string(log_level_str), /*also_console=*/true);

    {
        std::string cmdline;
        for (int i = 0; i < argc; ++i) {
            if (i) cmdline += ' ';
            cmdline += '"';
            cmdline += argv[i];
            cmdline += '"';
        }
        spdlog::debug("[cmd] {}", cmdline);
    }

    warn_on_shared_devices(tiers);

    for (const auto& tier : tiers) {
        spdlog::info("Storage tier '{}' -> {}{}", tier.name, tier.path.string(),
                     tier.name == default_tier ? " (default)" : "");
    }

    // The transport is built before the port is bound, so an unreadable certificate or a key that
    // does not match it is a startup failure with a specific message, not a stream of failed
    // handshakes after the server is nominally "up".
    std::shared_ptr<transport::StreamFactory> streams;
    if (rung == transport::Rung::Plain) {
        if (!tls.cert_file.empty() || !tls.key_file.empty()) {
            spdlog::warn("--tls-cert/--tls-key were given but --rung is 0: the connection is plaintext. "
                         "Pass --rung 3.5 to actually use them.");
        }
        streams = transport::make_plain_factory();
    } else {
        std::string error;
        std::vector<std::string> warnings;
        streams = transport::make_tls_server_factory(tls, error, warnings);
        for (const auto& warning : warnings) spdlog::warn("{}", warning);
        if (!streams) {
            spdlog::critical("TLS setup failed: {}", error);
            std::cerr << "TLS setup failed: " << error << std::endl;
            return 1;
        }

        // Printed so a client can be pinned against this exact key without anyone having to run
        // the openssl pipeline by hand.
        std::string pin;
        std::string pin_error;
        if (transport::certificate_pin(tls.cert_file, pin, pin_error)) {
            spdlog::info("Certificate public key pin: sha256:{}", pin);
        } else {
            spdlog::warn("Could not compute the certificate pin: {}", pin_error);
        }
    }
    spdlog::info("Transport: {}", streams->describe());

    asio::io_context io_context;
    Server server(io_context, port, StorageConfig{root, tiers, default_tier}, streams);

    asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait([&](const std::error_code& ec, int) {
        spdlog::info("Signal received, shutting down...");
        server.exit_all_sessions();
    });

    spdlog::info("Starting async server (version {}) on port {}", minidrive::resolved_version(), port);
    server.start();

    const unsigned int thread_count =  std::max(1u, std::thread::hardware_concurrency());

    std::vector<std::thread> pool;
    pool.reserve(thread_count);

    for(unsigned int i = 0; i < thread_count; i++) {
        pool.emplace_back([&io_context] {
            io_context.run();
        });
    }

    //io_context.stop();
    
    for (auto& t : pool) {
        t.join();
    }

    spdlog::info("Server exited.");
    return 0;
}
