#pragma once

#include <string>
#include <filesystem>
#include <vector>
#include <mutex>

struct DatabaseEntry {
    std::string username;
    std::string password_hash;
    std::string storage_class; // Name of the storage tier this user's data lives on, empty means server default
};

// Holds user data for authentification
class Database {
public:
    Database(const std::filesystem::path& path);
    bool user_exists(const std::string& username);
    bool validate_user(const std::string& username, const std::string& password); // Check password
    void add_user(const std::string& username, const std::string& password, const std::string& storage_class); // Add user to database and hash pasword using password.hpp
    std::string get_storage_class(const std::string& username); // Empty if user is unknown or has no tier recorded
    bool set_storage_class(const std::string& username, const std::string& storage_class); // False if user is unknown
private:
    std::filesystem::path path_; // Path to file_based database
    std::mutex db_mutex_; // Mutex for thread safe acces
    std::vector<DatabaseEntry> entries_; // Short lifetime copy of data in file

    void load(); // load form file to entries_
    void save(); // save from entries_ to file
};