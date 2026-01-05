#pragma once

#include<mutex>
#include<unordered_map>
#include "filesystem/utils.hpp"
#include "database.hpp"
#include "filesystem/partmeta.hpp"

// Stores data of currently active file transfer
struct ActiveTransfer{
    uint32_t transfer_id; // ID used in partmeta_ database
    fsutils::FileMetadata fmeta; // File metadata
    std::filesystem::path partial_path; // Path of .part file (user/.partial/id.part)
    std::vector<protocol::ChunkInfo> chunks; // Sizes, indexes and hashes of chunks
    std::vector<bool> chunk_state; // Represents received/sent chunks
};

class Storage {
public:
    Storage(std::filesystem::path root);

    void setup(); // Prepares server root dircetory
    std::shared_ptr<Database> get_database(); // Gets database of user data
    std::shared_ptr<PartialMetadata> get_partmeta(const std::string& user); // Gets database of partial file metadata for user, lazy initialization
    std::filesystem::path get_root(); // Gets server root
    bool try_acquire_user_lock(const std::string& user); // Returns if user can use lock, lazy initialization
    void release_user_lock(const std::string& user); // Set the value of user lock"

private:
    std::filesystem::path root_; // Server filesystem root directory
    std::mutex user_partmeta_guard_; // Mutex for user_partmeta_ map
    std::mutex user_lock_guard_; // Mutex for user_transfer_map
    std::unordered_map<std::string, std::shared_ptr<PartialMetadata>> user_partmeta_; // Map of users and their partial file metadata database
    std::unordered_map<std::string, bool> user_lock_; // Map of users and their operation lock value
    std::shared_ptr<Database> db_; // Database of user data
};