#include "sync_manifest.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace {

// One manifest per distinct local<->remote pairing, so the remote directory string has to become a
// usable file name.
std::string escape_remote_dir(const std::string& remote_dir) {
    std::string escaped;
    escaped.reserve(remote_dir.size());
    for(char c : remote_dir) {
        if(c == '/' || c == '\\' || c == ':' || c == '.' || c == ' ') {
            escaped += '_';
        } else {
            escaped += c;
        }
    }
    while(!escaped.empty() && escaped.front() == '_') escaped.erase(escaped.begin());
    while(!escaped.empty() && escaped.back() == '_') escaped.pop_back();
    if(escaped.empty()) escaped = "root";
    return escaped;
}

} // namespace

SyncManifest::SyncManifest(const std::filesystem::path& local_dir, const std::string& remote_dir)
    : remote_dir_(remote_dir) {
    std::error_code ec;
    std::filesystem::path dir = local_dir / ".minidrive-sync";
    std::filesystem::create_directories(dir, ec);
    manifest_file_ = dir / (escape_remote_dir(remote_dir) + ".json");
    load();
}

void SyncManifest::load() {
    std::ifstream f(manifest_file_);
    if(!f) return; // No baseline yet - a full diff, not an error

    json j;
    try {
        f >> j;
    } catch(const json::exception&) {
        entries_.clear(); // Corrupted baseline: fail open, re-diff everything
        return;
    }

    try {
        // A baseline recorded against a different remote directory says nothing about this pairing.
        if(j.value("remote_dir", std::string()) != remote_dir_) return;

        for(const auto& item : j.at("entries")) {
            SyncManifestEntry entry;
            entry.hash = item.value("hash", std::string());
            entry.mtime = item.value("mtime", uint64_t{0});
            entry.size = item.value("size", uint32_t{0});
            entry.is_directory = item.value("is_directory", false);
            entries_[item.at("relative_path").get<std::string>()] = entry;
        }
    } catch(const json::exception&) {
        entries_.clear();
    }
}

const std::map<std::string, SyncManifestEntry>& SyncManifest::get_entries() const {
    return entries_;
}

const SyncManifestEntry* SyncManifest::get(const std::string& relative_path) const {
    auto it = entries_.find(relative_path);
    if(it == entries_.end()) return nullptr;
    return &it->second;
}

void SyncManifest::put(const std::string& relative_path, const SyncManifestEntry& entry) {
    entries_[relative_path] = entry;
}

void SyncManifest::remove(const std::string& relative_path) {
    entries_.erase(relative_path);
}

const std::string& SyncManifest::remote_dir() const {
    return remote_dir_;
}

bool SyncManifest::save() {
    json j;
    j["remote_dir"] = remote_dir_;
    j["entries"] = json::array();

    for(const auto& [relative_path, entry] : entries_) {
        j["entries"].push_back({
            {"relative_path", relative_path},
            {"hash", entry.hash},
            {"mtime", entry.mtime},
            {"size", entry.size},
            {"is_directory", entry.is_directory}
        });
    }

    std::filesystem::path tmp = manifest_file_;
    tmp += ".tmp";

    {
        std::ofstream f(tmp);
        if(!f) return false;
        f << j.dump(4);
        if(!f) return false;
    }

    std::error_code ec;
    std::filesystem::rename(tmp, manifest_file_, ec);
    return !ec;
}
