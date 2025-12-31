#pragma once

#include <string>
#include <filesystem>
#include <vector>

struct DatabaseEntry {
    std::string username;
    std::string password_hash;
};

class Database {
public:
    Database(const std::filesystem::path& path);
    bool user_exists(const std::string& username);
    bool validate_user(const std::string& username, const std::string& password);
    void add_user(const std::string& username, const std::string& password);
private:
    std::filesystem::path path_;
    std::vector<DatabaseEntry> entries_;

    void load();
    void save();
};