#include "database.hpp"
#include <string>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include "password.hpp"
#include "filesystem/utils.hpp"
#include <iostream>
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

void Database::add_user(const std::string& username, const std::string& password, const std::string& storage_class) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    load();
    std::string password_hash = password::hash_password(password);
    entries_.push_back(DatabaseEntry{username, password_hash, storage_class});
    save();
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
            save();
            return true;
        }
    }
    return false;
}

void Database::load() {
    if(!fs::exists(path_)) return;

    entries_.clear();

    std::ifstream f(path_);
    if(!f) throw std::runtime_error("Failed to open database file");

    // check if database is empty
    if(f.peek() == std::ifstream::traits_type::eof()) {
        entries_.clear();
        return;
    }

    json j;
    f >> j;
    
    for(const auto& user: j["users"]) {
        // storage_class was added after the first users.json files were written,
        // so an entry without it is valid and means "server default tier"
        entries_.push_back(DatabaseEntry{
            user["username"].get<std::string>(),
            user["password_hash"].get<std::string>(),
            user.contains("storage_class") ? user["storage_class"].get<std::string>() : std::string()
        });
    }
   
    f.close();
}

void Database::save() {
    fs::path tmp = path_;
    tmp += ".tmp";

    std::ofstream f(tmp);
    if(!f) throw std::runtime_error("Failed to open database file for writing");

    json j;
    j["users"] = json::array();

    for(const auto& entry : entries_) {
        j["users"].push_back({
            {"username", entry.username},
            {"password_hash", entry.password_hash},
            {"storage_class", entry.storage_class}
        });
    }
    f << j.dump(4);
    f.close();

    fs::rename(tmp, path_);
}