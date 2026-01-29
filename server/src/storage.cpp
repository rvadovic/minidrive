#include <iostream>
#include "filesystem/utils.hpp"
#include "filesystem/partmeta.hpp"
#include "storage.hpp"

Storage::Storage(std::filesystem::path root)
    : root_(std::move(root)) {
    }

void Storage::setup() {
    if(!fsutils::exists(root_)) {
        fsutils::mkdir(std::filesystem::path(root_));
    }
    if(!fsutils::is_file(std::filesystem::path(root_ / "users.json"))) {
        fsutils::create_empty_file(std::filesystem::path(root_ / "users.json"));
        std::cout << "Created users.json file in root directory." << std::endl;
    }
    if(!fsutils::is_directory(std::filesystem::path(root_ / "public"))) {
        fsutils::mkdir(std::filesystem::path(root_ / "public"));
        std::cout << "Created public directory in root directory." <<  std::endl;
    }
    if(!fsutils::is_directory(std::filesystem::path(root_ / "private"))) {
        fsutils::mkdir(std::filesystem::path(root_ / "private"));
        std::cout << "Created private directory in root directory." << std::endl;
    }
    db_ = std::make_shared<Database>(std::filesystem::path(root_ / "users.json"));
    std::cout << "Root directory is set up" << std::endl;
}

std::shared_ptr<Database> Storage::get_database() {
    return db_;
}

std::shared_ptr<PartialMetadata> Storage::get_partmeta(const std::string& user) {
    std::lock_guard<std::mutex> lock(user_partmeta_guard_);
    auto it = user_partmeta_.find(user);
    if(it != user_partmeta_.end()) {
        return it->second;
    }
    if(user == "public") {
        auto partmeta_file = fsutils::absolute(std::filesystem::path(root_ / "public/.partial/partmeta.json"));
        user_partmeta_.emplace("public", std::make_shared<PartialMetadata>(partmeta_file));
        auto [inserted_it, inserted] = user_partmeta_.emplace(user, std::make_shared<PartialMetadata>(partmeta_file));
        return inserted_it->second;
    } else {
        if(!db_->user_exists(user)) {
            throw std::runtime_error("User does not exist: " + user);
        }
        auto partmeta_file = fsutils::absolute(std::filesystem::path(root_ / "private" / user / ".partial/partmeta.json"));
        auto [inserted_it, inserted] = user_partmeta_.emplace(user, std::make_shared<PartialMetadata>(partmeta_file));
        return inserted_it->second;
    }
}

std::filesystem::path Storage::get_root() {
    return root_;
}

bool Storage::try_acquire_user_lock(const std::string& user) {
    std::lock_guard<std::mutex> lock(user_lock_guard_);
    auto it = user_lock_.find(user);
    if(it != user_lock_.end()) {
        if(it->second) {
            return false;
        }
        it->second = true;
        return true;
    }
    if(user == "public") {
        user_lock_.emplace("public", true);
        return true;
    } else {
        if(!db_->user_exists(user)) {
            return false;
        }
        auto [inserted_it, inserted] = user_lock_.emplace(user, true);
        return true;
    }
}

void Storage::release_user_lock(const std::string& user) {
    std::lock_guard<std::mutex> lock(user_lock_guard_);
    auto it = user_lock_.find(user);
    if(it == user_lock_.end()) return;
    it->second = false;
}

