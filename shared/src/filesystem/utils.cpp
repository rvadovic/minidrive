#include "filesystem/utils.hpp"

#include <sodium.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <array>
#include <algorithm>

namespace fsutils {

bool FileMetadata::operator==(const FileMetadata& other) const {
    return absolute_path == other.absolute_path &&
           size == other.size &&
           last_modified == other.last_modified &&
           hash == other.hash;
}

bool FileMetadata::operator!=(const FileMetadata& other) const {
    return !(*this == other);
}

fs::path absolute(const fs::path& path) {
    return fs::absolute(path);
}
fs::path relative(const fs::path& base, const fs::path& path) {
    return fs::relative(path, base);
}

fs::path normalize(const fs::path& path) {
    try {
        return fs::weakly_canonical(path);
    } catch (...) {
        return path.lexically_normal();
    }
}

bool exists(const fs::path& path) {
    return fs::exists(path);
}

bool is_file(const fs::path& path) {
    return fs::is_regular_file(path);
}

bool is_directory(const fs::path& path) {
    return fs::is_directory(path);
}

bool paths_equal(const fs::path& p1, const fs::path& p2) {
    return normalize(p1) == normalize(p2);
}

bool is_subpath(const fs::path& base, const fs::path& sub) {
    auto norm_base = base.lexically_normal(); // does not need to exist for mkdir and create file operations
    auto norm_sub = sub.lexically_normal();

    std::cout << norm_base.string() << " " << norm_sub.string() << std::endl;

    auto base_it = norm_base.begin();
    auto sub_it = norm_sub.begin();

    for (; base_it != norm_base.end() && sub_it != norm_sub.end(); ++base_it, ++sub_it) {
        if (*base_it != *sub_it) {
            return false;
        }
    }
    return base_it == norm_base.end();
}

uint64_t get_file_size(const fs::path& path) {
    uint64_t size = fs::file_size(path);
    if(size > UINT32_MAX) return UINT64_MAX;
    return size;
}

uint64_t get_last_write_time(const fs::path& path) {
    fs::file_time_type ftime = fs::last_write_time(path);
    return std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count(); // Returns time in seconds
}

bool mkdir(const fs::path& path) {
    std::error_code ec;
    bool result = fs::create_directories(path, ec);
    if(ec) {
        result = false;
    }
    return result;
}

bool rmdir(const fs::path& path) {
    std::error_code ec;
    uint32_t result = fs::remove_all(path, ec);
    if(ec) {
        return false;
    }
    return result > 0;
}

bool create_empty_file(const fs::path& path) {
    if (fs::exists(path)) {
        return false;
    }
    std::ofstream file(path);
    return file.good();
}

bool remove_file(const fs::path& path) {
    std::error_code ec;
    bool result = fs::remove(path,ec);
    if(ec) {
        result = false;
    }
    return result;
}

bool copy_path(const fs::path& src, const fs::path& dest, bool overwrite) {
    std::error_code ec;

    if(!fs::exists(src)) return false;

    // Make sure parent dircectories exist
    if(!dest.parent_path().empty()) {
        fs::create_directories(dest.parent_path(), ec);
        if(ec) return false;
    }

    // Ovewrite case
    if (overwrite && fs::exists(dest)) {
        fs::remove_all(dest, ec);
        if(ec) return false;
    }

    // Directory case
    if(fs::is_directory(src)) {
        fs::create_directories(dest, ec);
        if(ec) return false;

        fs::copy(src, dest, fs::copy_options::recursive, ec);

        return !ec;
    }

    // File case
    fs::copy_file(src, dest, overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::none, ec);

    return !ec;
}

bool move_path(const fs::path& src, const fs::path& dest, bool overwrite) {
    std::error_code ec;

    if(!fs::exists(src)) return false;

    // Make sure parent directories exist
    if(!dest.parent_path().empty()) {
        fs::create_directories(dest.parent_path(), ec);
        if(ec) return false;
    }

    // If ovewrite remove dest
    if (overwrite && fs::exists(dest)) {
        fs::remove_all(dest, ec);
        if(ec) return false;
    }

    // Try rename
    fs::rename(src, dest, ec);
    if(!ec) return true;

    // Rename fails, copy, then delete

    // Directory
    if(fs::is_directory(src)) {
        fs::create_directories(dest, ec);
        if(ec) return false;

        fs::copy(src, dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if(ec) return false;

        fs::remove_all(src, ec);
        return !ec;
    }

    // File
    fs::copy_file(src, dest, overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::none, ec);
    if(ec) return false;

    fs::remove(src, ec);
    return !ec;
}

bool write_chunk(const fs::path& path, uint32_t offset, const std::vector<uint8_t>& data) {
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    
    // File does not exist, create file
    if (!f) {
        f.open(path, std::ios::binary | std::ios::out);
        if (!f) return false;
        f.close();

        f.open(path, std::ios::binary | std::ios::in | std::ios::out);
        if (!f) return false;
    }

    f.seekp(offset);
    f.write(reinterpret_cast<const char*>(data.data()), data.size());

    return f.good();
}

std::vector<uint8_t> read_chunk(const std::filesystem::path& path, uint32_t offset, uint32_t size) {
    std::vector<uint8_t> buffer(size);
    std::ifstream f(path, std::ios::binary);
    if(!f) {
        buffer.clear();
        return buffer;
    }

    f.seekg(offset);
    f.read(reinterpret_cast<char*>(buffer.data()), size);

    buffer.resize(f.gcount());

    f.close();
    return buffer;
}

bool is_hash_error(const std::array<uint8_t, crypto_generichash_BYTES>& h) {
    return std::all_of(h.begin(), h.end(), [](uint8_t b){ return b == 0; });
}

std::array<uint8_t, crypto_generichash_BYTES> hash_file(const fs::path& path) {
    std::cout << "hashing file" << std::endl;
    std::array<uint8_t, crypto_generichash_BYTES> hash;
    std::ifstream file(path, std::ios::binary);

    if (!file) return HASH_ERROR;

    std::array<uint8_t, 8192> buffer;
    crypto_generichash_state state;
    crypto_generichash_init(&state, nullptr, 0, crypto_generichash_BYTES);

    while (file) {
        file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        size_t bytes_read = file.gcount();
        if (bytes_read > 0) {
            crypto_generichash_update(&state, buffer.data(), bytes_read);
        }
    }

    crypto_generichash_final(&state, hash.data(), crypto_generichash_BYTES);
    std::cout << "file hashed" << std::endl;
    return hash;
}

std::array<uint8_t, crypto_generichash_BYTES> hash_chunk(const std::vector<uint8_t>& data) {
    std::array<uint8_t, crypto_generichash_BYTES> hash;

    crypto_generichash(hash.data(), hash.size(), data.data(), data.size(), nullptr, 0);

    return hash;
}

std::string hash_to_hex(const std::array<uint8_t, crypto_generichash_BYTES>& hash) {
    std::string hex(hash.size() * 2 + 1, '\0');
    
    sodium_bin2hex(hex.data(), hex.size(), hash.data(), hash.size());
    hex.pop_back(); 
    return hex;
}

std::array<uint8_t, crypto_generichash_BYTES> hex_to_hash(const std::string& hex) {
    std::array<uint8_t, crypto_generichash_BYTES> hash;
    if (hex.length() / 2 != crypto_generichash_BYTES) {
        hash.empty();
        return hash;
    }

    size_t hash_len = 0;

    if (sodium_hex2bin(hash.data(), hash.size(), hex.data(), hex.length(), nullptr, &hash_len, nullptr) != 0) {
        hash.empty();
        return hash;
    }

    if(hash_len != hash.size()) {
        hash.empty();
        return hash;
    }

    return hash;
}

bool is_scan_file_error(const FileMetadata& fmeta) {
    return fmeta.absolute_path.empty() && fmeta.size == 0;
}

bool is_scan_dir_error(const std::vector<FileMetadata>& list) {
    return list.size() == 1 && is_scan_file_error(list[0]);
}

FileMetadata scan_file(const fs::path& path) {
    if(get_file_size(path) == SIZE_ERROR) return FileMetadata{};
    std::array<uint8_t, crypto_generichash_BYTES> hash = hash_file(path);
    if(is_hash_error(hash)) return FileMetadata{};
    return FileMetadata{
        fsutils::absolute(path),
        static_cast<uint32_t>(get_file_size(path)),
        get_last_write_time(path),
        hash_file(path)
    };
}

std::vector<FileMetadata> scan_directory(const fs::path& dir, bool recursive) {
    std::vector<FileMetadata> files;
    fs::directory_options options = fs::directory_options::skip_permission_denied; // Skips hidden files

    bool error = false;

    auto process_entry = [&](const fs::directory_entry& entry) {
        if(error) return;

        FileMetadata metadata;
        metadata.absolute_path = fsutils::absolute(entry.path());
        metadata.last_modified = get_last_write_time(entry.path());

        if(is_file(entry.path())) {
            auto size = get_file_size(entry.path());
            if(size == SIZE_ERROR) { // File too large
                error = true;
                return;
            }

            metadata.size = static_cast<uint32_t>(size);
            metadata.hash = hash_file(entry.path());

            if(is_hash_error(metadata.hash)) { // Error in hash_file()
                error = true;
                return;
            }
        } else {
            metadata.size = 0; // Directory has no size
            metadata.hash = {}; // Directory has no hash
        }
        files.push_back(metadata);
    };

    std::error_code ec;

    if(recursive) {
        for (const auto& entry : fs::recursive_directory_iterator(dir, options, ec)) {
            process_entry(entry);
        }
    } else {
        for (const auto& entry : fs::directory_iterator(dir, options, ec)) {
            process_entry(entry);
        }
    }
    if(error) {
        return {FileMetadata{}};
    }
    return files;
}

bool is_compute_chunks_error(const std::vector<protocol::ChunkInfo>& chunks) {
    return chunks.size() == 1 && chunks[0].index == 0 && chunks[0].size == 0;
}

uint32_t chunk_count(const uint32_t& file_size) {
    if(file_size == 0) return 0;
    return (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
}

std::vector<protocol::ChunkInfo> compute_chunks(const FileMetadata& fmeta) {
    std::vector<protocol::ChunkInfo> chunks;

    uint32_t count = chunk_count(fmeta.size);
    chunks.reserve(count);

    for(uint32_t i = 0; i < count; ++i) {
        uint32_t offset = i * CHUNK_SIZE;
        uint32_t chunk_size = std::min(CHUNK_SIZE, fmeta.size - offset);

        std::vector<uint8_t> data = read_chunk(fmeta.absolute_path, offset, chunk_size);

        if(data.empty()) {
            chunks.clear();
            chunks.push_back(protocol::ChunkInfo{});
            return chunks;
        }

        std::array<uint8_t, crypto_generichash_BYTES> hash = hash_chunk(data);

        if(is_hash_error(hash)) {
            chunks.clear();
            chunks.push_back(protocol::ChunkInfo{});
            return chunks;
        }
        
        chunks.push_back({i, chunk_size, hash_to_hex(hash)});
    }

    return chunks;
}

}

