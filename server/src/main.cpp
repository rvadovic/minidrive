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
                  << " [--default-tier <name>] [--log-file <path>] [--log-level <level>]\n";
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

    asio::io_context io_context;
    Server server(io_context, port, StorageConfig{root, tiers, default_tier});

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
