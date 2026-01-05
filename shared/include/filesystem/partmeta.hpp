#pragma once

#include <filesystem>
#include <mutex>
#include <chrono>
#include <unordered_map>
#include <sodium.h>
#include <array>
#include <queue>
#include "protocol/message.hpp"
#include "filesystem/utils.hpp"

enum class TransferType {
    UPLOAD,
    DOWNLOAD
};

constexpr auto TRANSFER_TIMEOUT = std::chrono::hours(1); // Transfer is deleted after 1 hour

// Partial metadata for file sufficient for resumed transfer
struct PartialMetadataEntry {
    uint32_t id; // Id of transfer
    std::filesystem::path absolute_path; // Absolute destination path
    uint32_t size; // Final size
    std::array<uint8_t, crypto_generichash_BYTES> file_hash; //Final file hash
    TransferType type; // Download/ Upload
    std::vector<protocol::ChunkInfo> chunks; // Chunks, their indexe, sizes, hashes
    std::vector<bool> chunk_state; // bitmap of transferred chunks
    std::chrono::system_clock::time_point last_activity; // last update of data
};

// File-based JSON database of partial file metadata for resuming unfinished transfers
class PartialMetadata {
public:
    PartialMetadata(std::filesystem::path metadata_file);

    std::filesystem::path get_partial_path(uint32_t id); // Returns partial path for entry (user/.partial/id.part)
    bool is_expired(uint32_t id);

    // Add new partial metadata, lazy initialization, returns ID of entry
    uint32_t add_partial_metadata(TransferType type, fsutils::FileMetadata fmeta, std::vector<protocol::ChunkInfo> chunks, uint32_t id);
    void delete_partial_metadata(uint32_t id); // Delete entry
    void mark_chunk_received(uint32_t id, uint32_t chunk_index); // Mark chunk at index was sucesfully transfered
    void save(); // save entries_ to file, triggered manually, mostly during exit

private:// Check if scan_file returned error value
    std::filesystem::path metadata_file_; // Path of database file
    std::mutex partmeta_mutex_; // muetex for entries_
    std::unordered_map<uint32_t ,PartialMetadataEntry> entries_; // One partial metadata per user
    uint32_t next_id_{1}; // Increments with each assigned ID
    std::queue<uint32_t> free_ids_; // Queue of freed IDs so the IDs wont icrement till failure

    void cleanup_expired(); // Deletes expired entries
    void load();// load from file to entries_, triggered by constructor
    uint32_t generate_id(); // generate ID reusing free IDs or incrementing next_id_
};