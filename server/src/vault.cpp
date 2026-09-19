#include "vault.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include "filesystem/utils.hpp"

using nlohmann::json;

namespace {

// A crash mid-write leaves the previous complete file rather than a truncated one.
bool atomic_write(const std::filesystem::path& file, const json& j) {
    return fsutils::atomic_write_file(file, j.dump(4));
}

json read_json(const std::filesystem::path& file) {
    std::ifstream f(file);
    if(!f) return json::object();
    if(f.peek() == std::ifstream::traits_type::eof()) return json::object();

    json j;
    try {
        f >> j;
    } catch(const json::exception& e) {
        spdlog::error("Vault database {} is corrupted: {}", file.string(), e.what());
        return json::object();
    }
    return j;
}

// A relative path either is the prefix, or sits under it. Plain string comparison would also match
// "notes" against "notes-old", which would drop the wrong entries on RMDIR.
bool is_under(const std::string& path, const std::string& prefix) {
    if(path == prefix) return true;
    return path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 && path[prefix.size()] == '/';
}

} // namespace

VaultStore::VaultStore(std::filesystem::path file) : file_(std::move(file)) {
    std::lock_guard lock(mutex_);
    load();
}

void VaultStore::load() {
    initialized_ = false;
    info_ = protocol::VaultInfo{};
    devices_.clear();

    json j = read_json(file_);
    if(!j.contains("vault_salt")) return; // No vault for this account yet

    try {
        info_.enabled = true;
        info_.salt = j.at("vault_salt").get<std::string>();
        info_.opslimit = j.value("argon2_opslimit", uint64_t{0});
        info_.memlimit = j.value("argon2_memlimit", uint64_t{0});
        info_.algorithm = j.value("argon2_algorithm", 0);
        if(j.contains("wrapped_vk_password")) {
            protocol::from_json(j.at("wrapped_vk_password"), info_.wrapped_vk_password);
        }

        for(const auto& item : j.value("devices", json::array())) {
            VaultDevice device;
            device.device_id = item.value("device_id", std::string());
            device.device_name = item.value("device_name", std::string());
            device.algorithm = item.value("algorithm", std::string());
            device.x25519_pub = item.value("x25519_pub", std::string());
            device.mlkem_pub = item.value("mlkem_pub", std::string());
            if(item.contains("wrapped_vk_device")) {
                protocol::from_json(item.at("wrapped_vk_device"), device.wrapped_vk);
            }
            device.created_at = item.value("created_at", uint64_t{0});
            device.last_seen = item.value("last_seen", uint64_t{0});
            if(!device.device_id.empty()) devices_.push_back(std::move(device));
        }
        initialized_ = true;
    } catch(const json::exception& e) {
        // Fail closed: an unreadable vault must not look like "no vault", which would let the
        // account be re-initialized under a fresh VK and orphan every file it already holds.
        spdlog::error("Vault database {} could not be read: {}", file_.string(), e.what());
        initialized_ = true;
        info_ = protocol::VaultInfo{};
        info_.enabled = true;
    }
}

bool VaultStore::save() {
    json j;
    j["vault_salt"] = info_.salt;
    j["argon2_opslimit"] = info_.opslimit;
    j["argon2_memlimit"] = info_.memlimit;
    j["argon2_algorithm"] = info_.algorithm;
    j["wrapped_vk_password"] = info_.wrapped_vk_password;
    j["devices"] = json::array();

    for(const auto& device : devices_) {
        j["devices"].push_back({
            {"device_id", device.device_id},
            {"device_name", device.device_name},
            {"algorithm", device.algorithm},
            {"x25519_pub", device.x25519_pub},
            {"mlkem_pub", device.mlkem_pub},
            {"wrapped_vk_device", device.wrapped_vk},
            {"created_at", device.created_at},
            {"last_seen", device.last_seen}
        });
    }

    return atomic_write(file_, j);
}

bool VaultStore::is_initialized() {
    std::lock_guard lock(mutex_);
    return initialized_;
}

protocol::VaultInfo VaultStore::get_info() {
    std::lock_guard lock(mutex_);
    return info_;
}

bool VaultStore::initialize(const protocol::VaultInfo& info, std::string& error) {
    std::lock_guard lock(mutex_);

    if(initialized_) {
        error = "This account already has a vault.";
        return false;
    }
    if(info.salt.empty() || info.wrapped_vk_password.empty() || info.opslimit == 0 || info.memlimit == 0) {
        error = "Incomplete vault material.";
        return false;
    }

    info_ = info;
    info_.enabled = true;
    devices_.clear();

    if(!save()) {
        info_ = protocol::VaultInfo{};
        error = "Failed to store the vault.";
        return false;
    }
    initialized_ = true;
    return true;
}

std::vector<VaultDevice> VaultStore::get_devices() {
    std::lock_guard lock(mutex_);
    return devices_;
}

std::optional<VaultDevice> VaultStore::get_device(const std::string& device_id) {
    std::lock_guard lock(mutex_);
    for(const auto& device : devices_) {
        if(device.device_id == device_id) return device;
    }
    return std::nullopt;
}

bool VaultStore::add_device(const VaultDevice& device, std::string& error) {
    std::lock_guard lock(mutex_);

    if(!initialized_) {
        error = "This account has no vault.";
        return false;
    }
    for(const auto& existing : devices_) {
        if(existing.device_id == device.device_id) {
            error = "A device with that id is already enrolled.";
            return false;
        }
    }

    devices_.push_back(device);
    if(!save()) {
        devices_.pop_back();
        error = "Failed to record the device.";
        return false;
    }
    return true;
}

bool VaultStore::remove_device(const std::string& device_id) {
    std::lock_guard lock(mutex_);

    for(auto it = devices_.begin(); it != devices_.end(); ++it) {
        if(it->device_id != device_id) continue;
        VaultDevice removed = *it;
        devices_.erase(it);
        if(!save()) {
            devices_.push_back(removed);
            return false;
        }
        return true;
    }
    return false;
}

void VaultStore::touch_device(const std::string& device_id) {
    std::lock_guard lock(mutex_);

    for(auto& device : devices_) {
        if(device.device_id != device_id) continue;
        device.last_seen = static_cast<uint64_t>(std::time(nullptr));
        save();
        return;
    }
}

DekManifest::DekManifest(std::filesystem::path file) : file_(std::move(file)) {
    std::lock_guard lock(mutex_);
    load();
}

void DekManifest::load() {
    entries_.clear();

    json j = read_json(file_);
    if(!j.contains("entries")) return;

    try {
        // Same {"entries": {...}} shape the rest of the codebase settled on
        for(const auto& [relative_path, item] : j.at("entries").items()) {
            DekEntry entry;
            if(item.contains("wrapped_dek")) {
                protocol::from_json(item.at("wrapped_dek"), entry.wrapped_dek);
            }
            entry.plaintext_hash = item.value("plaintext_hash", std::string());
            entry.plaintext_size = item.value("plaintext_size", uint32_t{0});
            entries_[relative_path] = entry;
        }
    } catch(const json::exception& e) {
        spdlog::error("DEK manifest {} could not be read: {}", file_.string(), e.what());
        entries_.clear();
    }
}

bool DekManifest::save() {
    json j;
    j["entries"] = json::object();

    for(const auto& [relative_path, entry] : entries_) {
        j["entries"][relative_path] = {
            {"wrapped_dek", entry.wrapped_dek},
            {"plaintext_hash", entry.plaintext_hash},
            {"plaintext_size", entry.plaintext_size}
        };
    }

    return atomic_write(file_, j);
}

std::optional<DekEntry> DekManifest::get(const std::string& relative_path) {
    std::lock_guard lock(mutex_);
    auto it = entries_.find(relative_path);
    if(it == entries_.end()) return std::nullopt;
    return it->second;
}

std::map<std::string, DekEntry> DekManifest::get_entries() {
    std::lock_guard lock(mutex_);
    return entries_;
}

void DekManifest::put(const std::string& relative_path, const DekEntry& entry) {
    std::lock_guard lock(mutex_);
    entries_[relative_path] = entry;
    save();
}

void DekManifest::remove(const std::string& relative_path) {
    std::lock_guard lock(mutex_);
    if(entries_.erase(relative_path) == 0) return;
    save();
}

void DekManifest::remove_subtree(const std::string& relative_path) {
    std::lock_guard lock(mutex_);

    bool changed = false;
    for(auto it = entries_.begin(); it != entries_.end();) {
        if(is_under(it->first, relative_path)) {
            it = entries_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if(changed) save();
}

void DekManifest::rename(const std::string& from, const std::string& to) {
    std::lock_guard lock(mutex_);

    std::map<std::string, DekEntry> moved;
    bool changed = false;

    for(auto it = entries_.begin(); it != entries_.end();) {
        if(is_under(it->first, from)) {
            // MOVE renames whole directories too, so every key under the old prefix follows it
            std::string suffix = it->first.substr(from.size());
            moved[to + suffix] = it->second;
            it = entries_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }

    for(auto& [relative_path, entry] : moved) {
        entries_[relative_path] = std::move(entry);
    }
    if(changed) save();
}

void DekManifest::copy(const std::string& from, const std::string& to) {
    std::lock_guard lock(mutex_);

    std::map<std::string, DekEntry> copies;
    for(const auto& [relative_path, entry] : entries_) {
        if(!is_under(relative_path, from)) continue;
        copies[to + relative_path.substr(from.size())] = entry;
    }
    if(copies.empty()) return;

    for(auto& [relative_path, entry] : copies) {
        entries_[relative_path] = std::move(entry);
    }
    save();
}
