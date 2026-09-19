#include "database.hpp"
#include <string>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include "password.hpp"
#include "filesystem/utils.hpp"
#include <spdlog/spdlog.h>
#include <mutex>

using nlohmann::json;

namespace fs = std::filesystem;

Database::Database(const std::filesystem::path& path) : path_(path) {
    if(fsutils::get_file_size(path) == 0) save();
}

bool Database::user_exists(const std::string& username) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    load();
    for(const auto& entry : entries_) {
        if(entry.username == username) {
            return true;    
        }
    }
    return false;
}

bool Database::validate_user(const std::string& username, const std::string& password) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    load();
    for(const auto& entry : entries_) {
        if(entry.username == username && password::verify_password(password, entry.password_hash)) {
            return true;
        }
    }
    return false;
}

bool Database::add_user(const std::string& username, const std::string& password, const std::string& storage_class) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    load();
    if(!healthy_) return false; // See healthy_: a push onto an unreadable list would erase every other account
    std::string password_hash = password::hash_password(password);
    entries_.push_back(DatabaseEntry{username, password_hash, storage_class});
    return save();
}

std::string Database::get_storage_class(const std::string& username) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    load();
    for(const auto& entry : entries_) {
        if(entry.username == username) {
            return entry.storage_class;
        }
    }
    return std::string();
}

bool Database::set_storage_class(const std::string& username, const std::string& storage_class) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    load();
    for(auto& entry : entries_) {
        if(entry.username == username) {
            entry.storage_class = storage_class;
            return save();
        }
    }
    return false;
}

bool Database::is_healthy() {
    std::lock_guard<std::mutex> lock(db_mutex_);
    load();
    return healthy_;
}

void Database::load() {
    entries_.clear();

    // A missing file means no accounts yet (the constructor and Storage::setup() create it)
    if(!fsutils::exists(path_)) {
        healthy_ = true;
        return;
    }

    std::ifstream f(path_);
    if(!f) {
        healthy_ = false;
        spdlog::critical("Account database {} cannot be opened; authentication is disabled.", path_.string());
        return;
    }

    // check if database is empty
    if(f.peek() == std::ifstream::traits_type::eof()) {
        healthy_ = true;
        return;
    }

    try {
        json j;
        f >> j;

        // at(), not operator[]: an object without "users" is a damaged file, not an empty one
        const json& users = j.at("users");
        if(!users.is_array()) throw std::runtime_error("\"users\" is not an array");

        std::vector<DatabaseEntry> loaded;
        for(const auto& user: users) {
            // storage_class was added after the first users.json files were written,
            // so an entry without it is valid and means "server default tier"
            loaded.push_back(DatabaseEntry{
                user.at("username").get<std::string>(),
                user.at("password_hash").get<std::string>(),
                user.contains("storage_class") ? user.at("storage_class").get<std::string>() : std::string()
            });
        }
        entries_ = std::move(loaded);
        healthy_ = true;
    } catch(const std::exception& e) {
        // Fail closed - see healthy_ in database.hpp before "simplifying" this into an empty list
        entries_.clear();
        healthy_ = false;
        spdlog::critical("Account database {} is corrupted ({}); authentication is disabled and the "
                         "file will not be written until it is restored.", path_.string(), e.what());
    }
}

bool Database::save() {
    if(!healthy_) {
        spdlog::critical("Refusing to write account database {}: the file on disk could not be read, "
                         "and overwriting it would destroy every account in it.", path_.string());
        return false;
    }

    json j;
    j["users"] = json::array();

    for(const auto& entry : entries_) {
        j["users"].push_back({
            {"username", entry.username},
            {"password_hash", entry.password_hash},
            {"storage_class", entry.storage_class}
        });
    }

    if(!fsutils::atomic_write_file(path_, j.dump(4))) {
        spdlog::error("Failed to write account database {}", path_.string());
        return false;
    }
    return true;
}
