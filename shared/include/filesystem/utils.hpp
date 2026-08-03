#pragma once

#include <filesystem>
#include <cstdint>
#include <vector>
#include <sodium.h>
#include "protocol/message.hpp"

namespace fsutils {
    namespace fs = std::filesystem;

    // Default size of chunk
    inline constexpr uint32_t CHUNK_SIZE = 256 * 1024; // 256 KB

    inline constexpr std::array<uint8_t, crypto_generichash_BYTES> HASH_ERROR = {}; // For error in hashing functions 

    inline constexpr uint64_t SIZE_ERROR = UINT64_MAX; // For size > 4GB error
    
    inline constexpr uint64_t TIME_ERRROR = UINT64_MAX; // For last time error

    // Describes files
    struct FileMetadata {
        fs::path absolute_path; // Absolute path of file
        uint32_t size; // size fo file
        uint64_t last_modified; // last modified for SYNC
        std::array<uint8_t, crypto_generichash_BYTES> hash; // file hash - generic hash

        bool operator==(const FileMetadata& other) const; // for SYNC
        bool operator!=(const FileMetadata& other) const; // for SYNC
    };

    // Convert path type
    fs::path resolve_path(const fs::path& user_dir, const fs::path& current_dir, const fs::path& path);
    fs::path absolute(const fs:: path& path);
    fs::path relative(const fs::path& base, const fs::path& path);
    fs::path normalize(const fs::path& path);

    // Path tests
    bool exists(const fs::path& path);
    bool is_file(const fs::path& path);
    bool is_directory(const fs::path& path);
    bool paths_equal(const fs::path& p1, const fs::path& p2);
    bool is_subpath(const fs::path& base, const fs::path& sub); // sub path does not have to exist

    // Directory operations
    bool mkdir(const fs::path& path);
    bool rmdir(const fs::path& path);

    // File operations
    bool create_empty_file(const fs::path& path);
    bool remove_file(const fs::path& path);

    // Copy and move
    bool copy_path(const fs::path& src, const fs::path& dest, bool overwrite = false); // Create parent directories too
    bool move_path(const fs::path& src, const fs::path& dest, bool overwrite = false); // Create parent directories too

    // Read/write of file with chunks
    bool write_chunk(const fs::path& path, uint32_t offset, const std::vector<uint8_t>& data);
    std::vector<uint8_t> read_chunk(const fs::path& path, uint32_t offset, uint32_t size);

    // File info
    uint64_t get_file_size(const fs::path& path); // returns SIZE_ERROR if over 4GB
    uint64_t get_last_write_time(const fs::path& path); // in seconds from epoch

    // Hash files and chunks using libsodium generic hash
    bool is_hash_error(const std::array<uint8_t, crypto_generichash_BYTES>& h); // Chek if hash == HASH_ERROR
    std::array<uint8_t, crypto_generichash_BYTES> hash_file(const fs::path& path); // return HASH_ERROR if error
    std::array<uint8_t, crypto_generichash_BYTES> hash_chunk(const std::vector<uint8_t>& data); // return HASH_ERROR if error

    // Conversion to string in hex notation
    std::string hash_to_hex(const std::array<uint8_t, crypto_generichash_BYTES>& hash);
    std::array<uint8_t, crypto_generichash_BYTES> hex_to_hash(const std::string& hex);

    // Gather info on files and directories
    bool is_scan_file_error(const FileMetadata& fmeta); // Check if scan_file returned error value
    bool is_scan_dir_error(const std::vector<FileMetadata>& list); // Check if scan_dir returned error value
    FileMetadata scan_file(const fs::path& path);
    std::vector<FileMetadata> scan_directory(const fs::path& dir, bool recursive = false);

    // Chunk helper functions
    bool is_compute_chunks_error(const std::vector<protocol::ChunkInfo>& chunks); // Check if compute_chunks returned error value
    uint32_t chunk_count(const uint32_t& file_size); // Number of chunks needed for file
    std::vector<protocol::ChunkInfo> compute_chunks(const FileMetadata& fmeta); // Put all neded chunks, their sizes, hashes, indexes into vector 
}