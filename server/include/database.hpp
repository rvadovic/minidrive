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
    bool add_user(const std::string& username, const std::string& password, const std::string& storage_class); // Add user to database and hash pasword using password.hpp. False if it could not be stored
    std::string get_storage_class(const std::string& username); // Empty if user is unknown or has no tier recorded
    bool set_storage_class(const std::string& username, const std::string& storage_class); // False if user is unknown or it could not be stored
    // False while users.json cannot be parsed. Re-reads the file, so restoring it recovers
    // without a restart.
    bool is_healthy();
private:
    std::filesystem::path path_; // Path to file_based database
    std::mutex db_mutex_; // Mutex for thread safe acces
    std::vector<DatabaseEntry> entries_; // Short lifetime copy of data in file

    // Fail CLOSED, deliberately. A users.json that cannot be read must never look like an empty
    // user list: add_user() is load-push-save, so the next registration would rewrite the file
    // with one account and permanently destroy every other one. While healthy_ is false, save()
    // refuses to write and authentication is refused until an operator restores the file.
    bool healthy_ = true;

    void load(); // load form file to entries_, clears healthy_ on a file that cannot be read
    bool save(); // save from entries_ to file, refuses while !healthy_
};