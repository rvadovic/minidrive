#pragma once

#include <string>
#include <filesystem>
#include <vector>
#include <mutex>

struct DatabaseEntry {
    std::string username;
    std::string password_hash;
};

// Holds user data for authentification
class Database {
public:
    Database(const std::filesystem::path& path);
    bool user_exists(const std::string& username);
    bool validate_user(const std::string& username, const std::string& password); // Check password
    void add_user(const std::string& username, const std::string& password); // Add user to database and hash pasword using password.hpp
private:
    std::filesystem::path path_; // Path to file_based database
    std::mutex db_mutex_; // Mutex for thread safe acces
    std::vector<DatabaseEntry> entries_; // Short lifetime copy of data in file

    void load(); // load form file to entries_
    void save(); // save from entries_ to file
};