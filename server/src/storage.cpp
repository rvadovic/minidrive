#include <map>
#include <spdlog/spdlog.h>
#include "filesystem/utils.hpp"
#include "filesystem/partmeta.hpp"
#include "storage.hpp"

namespace {

// Builds a "relative path -> content fingerprint" snapshot of a user's files directory.
// Directories are recorded too so an empty directory that failed to copy is still noticed.
// Returns false when the directory could not be scanned.
bool snapshot_files(const std::filesystem::path& files_dir,
                    std::map<std::string, std::string>& out,
                    uint64_t& bytes) {
    out.clear();
    bytes = 0;

    if(!fsutils::is_directory(files_dir)) return true; // Nothing stored yet, an empty snapshot is correct

    std::vector<fsutils::FileMetadata> files = fsutils::scan_directory(files_dir, true);
    if(fsutils::is_scan_dir_error(files)) return false;

    for(const auto& file : files) {
        std::string relative = fsutils::relative(files_dir, file.absolute_path).generic_string();
        if(fsutils::is_directory(file.absolute_path)) {
            out.emplace(relative, std::string("<dir>"));
        } else {
            out.emplace(relative, fsutils::hash_to_hex(file.hash));
            bytes += file.size;
        }
    }
    return true;
}

} // namespace

Storage::Storage(StorageConfig config)
    : root_(fsutils::absolute(config.root)),
      tiers_(std::move(config.tiers)),
      default_tier_(std::move(config.default_tier)) {
    for(auto& tier : tiers_) {
        tier.path = fsutils::absolute(tier.path);
    }
}

void Storage::setup() {
    if(!fsutils::exists(root_)) {
        fsutils::mkdir(std::filesystem::path(root_));
    }
    if(!fsutils::is_file(std::filesystem::path(root_ / "users.json"))) {
        fsutils::create_empty_file(std::filesystem::path(root_ / "users.json"));
        spdlog::info("Created users.json file in root directory.");
    }
    if(!fsutils::is_directory(std::filesystem::path(root_ / "public"))) {
        fsutils::mkdir(std::filesystem::path(root_ / "public"));
        spdlog::info("Created public directory in root directory.");
    }
    // Private user data lives on the storage media, one private/ per configured tier.
    // Without --tier flags there is a single implicit tier pointing at the root, which
    // makes this create exactly the root/private the server has always created.
    for(const auto& tier : tiers_) {
        if(!fsutils::is_directory(std::filesystem::path(tier.path / "private"))) {
            fsutils::mkdir(std::filesystem::path(tier.path / "private"));
            spdlog::info("Created private directory on tier '{}'.", tier.name);
        }
    }
    db_ = std::make_shared<Database>(std::filesystem::path(root_ / "users.json"));
    spdlog::info("Root directory is set up ({}).", root_.string());
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
        auto [inserted_it, inserted] = user_partmeta_.emplace(user, std::make_shared<PartialMetadata>(partmeta_file));
        return inserted_it->second;
    } else {
        if(!db_->user_exists(user)) {
            throw std::runtime_error("User does not exist: " + user);
        }
        std::filesystem::path user_root = get_user_root(user);
        if(user_root.empty()) {
            throw std::runtime_error("Storage tier is not configured for user: " + user);
        }
        auto partmeta_file = fsutils::absolute(std::filesystem::path(user_root / "private" / user / ".partial/partmeta.json"));
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

const std::vector<StorageTier>& Storage::get_tiers() const {
    return tiers_;
}

const StorageTier* Storage::find_tier(const std::string& name) const {
    for(const auto& tier : tiers_) {
        if(tier.name == name) return &tier;
    }
    return nullptr;
}

const std::string& Storage::get_default_tier() const {
    return default_tier_;
}

std::string Storage::get_user_tier(const std::string& user) {
    if(user == "public") return std::string(); // Public storage is not tiered, it stays on the control root

    std::string recorded = db_->get_storage_class(user);
    if(recorded.empty()) recorded = default_tier_; // Registered before tiering existed

    const StorageTier* tier = find_tier(recorded);
    if(tier == nullptr) {
        // The admin dropped the --tier flag this user was placed on. Fail closed rather than
        // silently falling back to the default, which would show the user an empty directory.
        return std::string();
    }

    if(fsutils::is_directory(std::filesystem::path(tier->path / "private" / user))) {
        return recorded;
    }

    // The record says one tier but there is no directory there. If the data sits on exactly one
    // other medium, a migration was interrupted between moving the files and updating users.json
    // - adopt what is actually on disk and repair the record.
    std::string found;
    size_t matches = 0;
    for(const auto& candidate : tiers_) {
        if(candidate.name == recorded) continue;
        if(fsutils::is_directory(std::filesystem::path(candidate.path / "private" / user))) {
            found = candidate.name;
            matches++;
        }
    }
    if(matches == 1) {
        spdlog::warn("User '{}' is recorded on tier '{}' but their data is on '{}'; correcting the record.",
                     user, recorded, found);
        db_->set_storage_class(user, found);
        return found;
    }

    return recorded; // Freshly registered user, or ambiguous - leave the record alone
}

std::filesystem::path Storage::get_user_root(const std::string& user) {
    if(user == "public") return root_;

    const StorageTier* tier = find_tier(get_user_tier(user));
    if(tier == nullptr) return std::filesystem::path();
    return tier->path;
}

void Storage::invalidate_partmeta(const std::string& user) {
    std::lock_guard<std::mutex> lock(user_partmeta_guard_);
    user_partmeta_.erase(user);
}

MigrationResult Storage::migrate_user(const std::string& user, const StorageTier& from, const StorageTier& to) {
    std::filesystem::path src = from.path / "private" / user;
    std::filesystem::path dst = to.path / "private" / user;

    spdlog::info("[{}] Migrating from tier '{}' to tier '{}'.", user, from.name, to.name);

    // Registered but never stored anything - there is nothing to copy
    if(!fsutils::is_directory(src)) {
        if(!fsutils::is_directory(dst) && !fsutils::mkdir(dst)) {
            return MigrationResult{false, "Failed to create your directory on tier '" + to.name + "'.", 0, 0};
        }
        invalidate_partmeta(user);
        return MigrationResult{true, "", 0, 0};
    }

    // A leftover from a migration that failed earlier needs an administrator, not a silent overwrite
    if(fsutils::exists(dst)) {
        return MigrationResult{false, "Data already exists on tier '" + to.name +
            "'; a previous migration may have failed. Contact the administrator.", 0, 0};
    }

    // Snapshot the source first so the copy can be verified before anything is deleted
    std::map<std::string, std::string> before;
    uint64_t bytes = 0;
    if(!snapshot_files(src / "files", before, bytes)) {
        return MigrationResult{false, "Failed to read your files on tier '" + from.name + "'.", 0, 0};
    }

    // Copy the whole user directory, files/ and .partial/ together, so the tree stays consistent
    if(!fsutils::copy_path(src, dst, false)) {
        fsutils::rmdir(dst);
        return MigrationResult{false, "Failed to copy your data to tier '" + to.name +
            "'; nothing was removed.", 0, 0};
    }

    // Verify before deleting anything - the source is still intact at this point
    std::map<std::string, std::string> after;
    uint64_t copied_bytes = 0;
    if(!snapshot_files(dst / "files", after, copied_bytes)) {
        fsutils::rmdir(dst);
        return MigrationResult{false, "Failed to verify the copy on tier '" + to.name +
            "'; nothing was removed.", 0, 0};
    }

    if(before != after) {
        fsutils::rmdir(dst);
        return MigrationResult{false, "The copy on tier '" + to.name +
            "' did not match the source; nothing was removed.", 0, 0};
    }

    // The destination is verified complete, so the move counts as done even if cleaning up
    // the old copy fails. Leaving the record pointing at the old tier would be worse.
    if(!fsutils::rmdir(src)) {
        spdlog::warn("[{}] Migration to tier '{}' succeeded but the old copy on '{}' could not be removed: {}",
                     user, to.name, from.name, src.string());
    }

    spdlog::info("[{}] Migration to tier '{}' succeeded: {} files, {} bytes.", user, to.name, before.size(), bytes);
    invalidate_partmeta(user);
    return MigrationResult{true, "", before.size(), bytes};
}
