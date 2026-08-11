#pragma once

#include<mutex>
#include<unordered_map>
#include "filesystem/utils.hpp"
#include "database.hpp"
#include "filesystem/partmeta.hpp"
#include "tier_config.hpp"

// Stores data of currently active file transfer
struct ActiveTransfer{
    uint32_t transfer_id; // ID used in partmeta_ database
    fsutils::FileMetadata fmeta; // File metadata
    std::filesystem::path partial_path; // Path of .part file (user/.partial/id.part)
    std::vector<protocol::ChunkInfo> chunks; // Sizes, indexes and hashes of chunks
    std::vector<bool> chunk_state; // Represents received/sent chunks
};

// Outcome of physically relocating one user's tree between two storage tiers
struct MigrationResult {
    bool ok;
    std::string error; // Human readable reason when ok is false
    uint64_t files; // Number of files moved
    uint64_t bytes; // Total size moved
};

class Storage {
public:
    Storage(StorageConfig config);

    void setup(); // Prepares server root dircetory
    std::shared_ptr<Database> get_database(); // Gets database of user data
    std::shared_ptr<PartialMetadata> get_partmeta(const std::string& user); // Gets database of partial file metadata for user, lazy initialization
    std::filesystem::path get_root(); // Gets server control root (users.json, public/)
    bool try_acquire_user_lock(const std::string& user); // Returns if user can use lock, lazy initialization
    void release_user_lock(const std::string& user); // Set the value of user lock"

    // Storage tiering
    const std::vector<StorageTier>& get_tiers() const; // All media configured with --tier
    const StorageTier* find_tier(const std::string& name) const; // nullptr when the name is not configured
    const std::string& get_default_tier() const; // Tier assigned to newly registered users
    std::string get_user_tier(const std::string& user); // Effective tier name for a user, empty if it is no longer configured
    std::filesystem::path get_user_root(const std::string& user); // Media root holding this user's data, empty if their tier is gone
    void invalidate_partmeta(const std::string& user); // Drop cached PartialMetadata so it is rebuilt against a new root
    MigrationResult migrate_user(const std::string& user, const StorageTier& from, const StorageTier& to); // Copy, verify, then remove

private:
    std::filesystem::path root_; // Server control root directory (users.json, public/)
    std::vector<StorageTier> tiers_; // Configured storage media, immutable after construction
    std::string default_tier_; // Name of the tier new users are placed on
    std::mutex user_partmeta_guard_; // Mutex for user_partmeta_ map
    std::mutex user_lock_guard_; // Mutex for user_transfer_map
    std::unordered_map<std::string, std::shared_ptr<PartialMetadata>> user_partmeta_; // Map of users and their partial file metadata database
    std::unordered_map<std::string, bool> user_lock_; // Map of users and their operation lock value
    std::shared_ptr<Database> db_; // Database of user data
};