#pragma once

#include <filesystem>
#include <cstdint>
#include <vector>

namespace fsutils {
    namespace fs = std::filesystem;

    struct TransferConfig {
        std::size_t CHUNK_SIZE = 256 * 1024; // 256 KB
    };

    struct TransferMetadata {};

    struct FileMetadata {
        fs::path relative_path;
        uint64_t size;
        uint64_t last_modified;
        std::vector<uint8_t> hash;

        bool operator==(const FileMetadata& other) const;
        bool operator!=(const FileMetadata& other) const;
    };

    struct ChunkMetadata {
        uint64_t index;
        uint64_t offset;
        uint32_t size;
        std::vector<uint8_t> hash;
    };

    fs::path relative(const fs::path& base, const fs::path& path);
    fs::path normalize(const fs::path& path);

    bool exists(const fs::path& path);
    bool is_file(const fs::path& path);
    bool is_directory(const fs::path& path);
    bool paths_equal(const fs::path& p1, const fs::path& p2);
    bool is_subpath(const fs::path& base, const fs::path& sub);

    bool mkdir(const fs::path& path);
    bool rmdir(const fs::path& path);

    bool create_empty_file(const fs::path& path);
    bool remove_file(const fs::path& path);

    bool copy_file(const fs::path& src, const fs::path& dest, bool overwrite = false);
    bool move_file(const fs::path& src, const fs::path& dest, bool overwrite = false);

    bool write_atomic(const fs::path& path, const std::vector<uint8_t>& data);

    uint64_t get_file_size(const fs::path& path);
    uint64_t get_last_write_time(const fs::path& path);

    std::vector<uint8_t> hash_file(const fs::path& path);
    std::vector<uint8_t> hash_chunk(const fs::path& path, uint64_t offset, uint8_t size);
    
    std::vector<FileMetadata> scan_directory(const fs::path& dir, bool recursive = false);
}