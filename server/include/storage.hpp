#pragma once

#include<mutex>
#include<unordered_map>
#include "filesystem/utils.hpp"
#include "database.hpp"
#include "filesystem/partmeta.hpp"
#include "tier_config.hpp"
#include "vault.hpp"
#include "auth_limiter.hpp"

// Stores data of currently active file transfer
struct ActiveTransfer{
    uint32_t transfer_id = UINT32_MAX; // ID used in partmeta_ database
    fsutils::FileMetadata fmeta{}; // File metadata
    std::filesystem::path partial_path{}; // Path of .part file (user/.partial/id.part)
    std::vector<protocol::ChunkInfo> chunks{}; // Sizes, indexes and hashes of chunks
    std::vector<bool> chunk_state{}; // Represents received/sent chunks
    // Vault bookkeeping for an upload into an end-to-end encrypted account. Empty otherwise, and
    // opaque either way: the server records these and hands them back, it cannot use them.
    protocol::WrappedBlob wrapped_dek{};
    std::string plaintext_hash{};
    uint32_t plaintext_size = 0;
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

    bool setup(); // Prepares server root dircetory, false if the server must not start
    std::shared_ptr<Database> get_database(); // Gets database of user data
    std::shared_ptr<PartialMetadata> get_partmeta(const std::string& user); // Gets database of partial file metadata for user, lazy initialization
    // End-to-end encryption state, same lazy-per-user caching as get_partmeta(). Public mode has
    // no database row and therefore no vault, so both return nullptr for it.
    std::shared_ptr<VaultStore> get_vault(const std::string& user);
    std::shared_ptr<DekManifest> get_dek_manifest(const std::string& user);
    // Password-failure lockout. Lives here rather than on Session because it is per-user state
    // shared by every session of that user - the same reason the busy lock does - and a counter
    // that reset on reconnect would stop nothing.
    AuthLimiter& get_auth_limiter();
    std::filesystem::path get_root(); // Gets server control root (users.json, public/)
    // The lock is per user but a user can have several live sessions, so it records which session
    // holds it. owner is any non-zero id unique among live sessions; releasing with a different one
    // is ignored, otherwise a second session frees a transfer it knows nothing about.
    bool try_acquire_user_lock(const std::string& user, uint64_t owner); // Returns if user can use lock, lazy initialization
    void release_user_lock(const std::string& user, uint64_t owner); // Releases only if owner currently holds it

    // Storage tiering
    const std::vector<StorageTier>& get_tiers() const; // All media configured with --tier
    const StorageTier* find_tier(const std::string& name) const; // nullptr when the name is not configured
    const std::string& get_default_tier() const; // Tier assigned to newly registered users
    std::string get_user_tier(const std::string& user); // Effective tier name for a user, empty if it is no longer configured
    std::filesystem::path get_user_root(const std::string& user); // Media root holding this user's data, empty if their tier is gone
    void invalidate_partmeta(const std::string& user); // Drop every cached per-user database so they are rebuilt against a new root
    MigrationResult migrate_user(const std::string& user, const StorageTier& from, const StorageTier& to); // Copy, verify, then remove

private:
    std::filesystem::path root_; // Server control root directory (users.json, public/)
    std::vector<StorageTier> tiers_; // Configured storage media, immutable after construction
    std::string default_tier_; // Name of the tier new users are placed on
    std::mutex user_partmeta_guard_; // Mutex for user_partmeta_ map
    std::mutex user_lock_guard_; // Mutex for user_transfer_map
    std::unordered_map<std::string, std::shared_ptr<PartialMetadata>> user_partmeta_; // Map of users and their partial file metadata database
    std::unordered_map<std::string, std::shared_ptr<VaultStore>> user_vault_; // Map of users and their vault database
    std::unordered_map<std::string, std::shared_ptr<DekManifest>> user_dek_; // Map of users and their per-file key manifest
    std::unordered_map<std::string, uint64_t> user_lock_; // Map of users and the session id holding their lock, 0 when free
    std::shared_ptr<Database> db_; // Database of user data
    AuthLimiter auth_limiter_; // Shared by every session, keyed by username
};