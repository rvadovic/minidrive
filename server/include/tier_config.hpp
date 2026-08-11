#pragma once

#include <string>
#include <filesystem>
#include <vector>

// One configured storage medium (a physical disk / mount point the admin declared with --tier).
// The name is what travels to clients; the path never leaves the server.
struct StorageTier {
    std::string name; // Logical name, e.g. "hot" - never a path
    std::filesystem::path path; // Absolute root of the medium
    std::string description; // Admin supplied text, shown to clients
};

// Everything the server was configured with on the command line.
// Bundled so Server/Storage keep a small constructor as more configuration is added later.
struct StorageConfig {
    std::filesystem::path root; // Control root: users.json and public/
    std::vector<StorageTier> tiers; // Declared media, always at least one
    std::string default_tier; // Tier assigned to newly registered users
};
