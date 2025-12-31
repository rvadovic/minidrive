#include "filesystem/utils.hpp"

#include <sodium.h>
#include <chrono>
#include <fstream>
#include <iostream>

namespace fsutils {

bool FileMetadata::operator==(const FileMetadata& other) const {
    return relative_path == other.relative_path &&
           size == other.size &&
           last_modified == other.last_modified &&
           hash == other.hash;
}

bool FileMetadata::operator!=(const FileMetadata& other) const {
    return !(*this == other);
}

fs::path relative(const fs::path& path, const fs::path& base) {
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

bool is_subpath(const fs::path& sub, const fs::path& base) {
    auto norm_base = normalize(base);
    auto norm_sub = normalize(sub);

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
    return fs::file_size(path);
}

uint64_t get_last_write_time(const fs::path& path) {
    fs::file_time_type ftime = fs::last_write_time(path);
    return std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();
}

bool mkdir(const fs::path& path) {
    return fs::create_directories(path);
}

bool rmdir(const fs::path& path) {
    return fs::remove_all(path) > 0;
}

bool create_empty_file(const fs::path& path) {
    if (fs::exists(path)) {
        return false;
    }
    std::ofstream file(path);
    return file.good();
}

bool remove_file(const fs::path& path) {
    return fs::remove(path);
}

bool copy_file(const fs::path& src, const fs::path& dest, bool overwrite) {
    fs::copy_options options = overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::none;
    std::error_code ec;
    fs::copy_file(src, dest, options, ec);
    if(ec) {
        std::cerr << ec.message() << std::endl;
        return false;
    }
    return true;
}

bool move_file(const fs::path& src, const fs::path& dest, bool overwrite) {
    std::error_code ec;

    if (overwrite && fs::exists(dest)) {
        fs::remove(dest, ec);
        if(ec) return false;
    }

    fs::rename(src, dest, ec);
    
    if(ec) {
        std::ifstream in(src, std::ios::binary);
        std::ofstream out(dest, std::ios::binary);
        if (!in || !out) return false;

        out << in.rdbuf();
        in.close();
        out.close();

        fs::remove(src, ec);
        if (ec) return false;
    }

    return true;
}

std::vector<uint8_t> hash_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);

    if (!file) {
        throw std::runtime_error("Failed to open file for hashing: " + path.string());
    }
    std::vector<uint8_t> buffer(8192);
    crypto_generichash_state state;
    crypto_generichash_init(&state, nullptr, 0, crypto_generichash_BYTES);

    while (file) {
        file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        size_t bytes_read = file.gcount();
        if (bytes_read > 0) {
            crypto_generichash_update(&state, buffer.data(), bytes_read);
        }
    }
    std::vector<uint8_t> hash(crypto_generichash_BYTES);
    crypto_generichash_final(&state, hash.data(), crypto_generichash_BYTES);
    return hash;
}

std::vector<uint8_t> hash_chunk(const fs::path& path, uint64_t offset, uint64_t size) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open file for chunk hashing: " + path.string());
    }

    file.seekg(offset);
    if(!file) {
        throw std::runtime_error("Failed to seek to offset for chunk hashing: " + path.string());
    }

    std::vector<uint8_t> buffer(8192);
    crypto_generichash_state state;
    crypto_generichash_init(&state, nullptr, 0, crypto_generichash_BYTES);

    uint64_t bytes_remaining = size;
    while (bytes_remaining > 0 && file) {
        uint64_t to_read = std::min<uint64_t>(buffer.size(), bytes_remaining);
        file.read(reinterpret_cast<char*>(buffer.data()), to_read);

        std::streamsize read = file.gcount();
        if (read <= 0) {
            break;
        }

        crypto_generichash_update(&state, buffer.data(), read);
        bytes_remaining -= read;
    }

    if(bytes_remaining != 0) {
        throw std::runtime_error("File too small for requested chunk");
    }

    std::vector<uint8_t> hash(crypto_generichash_BYTES);
    crypto_generichash_final(&state, hash.data(), hash.size());
    return hash;
}

std::string hash_to_hex(const std::vector<uint8_t>& hash) {
    std::string hex(hash.size() * 2 + 1, '\0');
    
    sodium_bin2hex(hex.data(), hex.size(), hash.data(), hash.size());
    hex.pop_back(); 
    return hex;
}

std::vector<uint8_t> hex_to_hash(const std::string& hex) {
    if (hex.length() % 2 != 0) {
        throw std::invalid_argument("Hex string has invalid length");
    }

    std::vector<uint8_t> hash(hex.length() / 2);
    size_t hash_len = 0;

    if (sodium_hex2bin(hash.data(), hash.size(), hex.data(), hex.length(), nullptr, &hash_len, nullptr) != 0) {
        throw std::invalid_argument("Invalid hex string");
    }

    if(hash_len != hash.size()) {
        throw std::invalid_argument("Hex decoding resulted in incorrect hash size");
    }

    return hash;
}

std::vector<FileMetadata> scan_directory(const fs::path& dir, bool recursive) {
    std::vector<FileMetadata> files;
    fs::directory_options options = fs::directory_options::skip_permission_denied;

    auto process_entry = [&](const fs::directory_entry& entry) {
        if(is_file(entry.path())) {
            FileMetadata metadata{
                fsutils::relative(entry.path(), dir),
                get_file_size(entry.path()),
                get_last_write_time(entry.path()),
                hash_file(entry.path())
            };
            files.push_back(metadata);
        }
    };

    if(recursive) {
        for (const auto& entry : fs::recursive_directory_iterator(dir, options)) {
            process_entry(entry);
        }
    } else {
        for (const auto& entry : fs::directory_iterator(dir, options)) {
            process_entry(entry);
        }
    }
    return files;
}

}

