#include <mutex>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <thread>
#include <string>
#include <nlohmann/json.hpp>
#include "filesystem/partmeta.hpp"
#include "filesystem/utils.hpp"

using nlohmann::json;

PartialMetadata::PartialMetadata(std::filesystem::path metadata_file)
    :metadata_file_(std::move(metadata_file)) {
    if(fsutils::get_file_size(metadata_file_) == 0) save();
    load(); // load on initialization
    cleanup_expired(); // check expired on initialization
}

std::filesystem::path PartialMetadata::get_partial_path(uint32_t id) {
    std::lock_guard lock(partmeta_mutex_);
    auto it = entries_.find(id);
    if(it == entries_.end()) return std::filesystem::path("");

    std::string name = std::to_string(id) + ".part";
    return fsutils::absolute(std::filesystem::path(metadata_file_.parent_path() / name));
}

uint32_t PartialMetadata::generate_id() {
    if(!free_ids_.empty()) {
        uint32_t id = free_ids_.front();
        free_ids_.pop();
        return id;
    }
    return next_id_++;
}

uint32_t PartialMetadata::add_partial_metadata(TransferType type, fsutils::FileMetadata fmeta, std::vector<protocol::ChunkInfo> chunks, uint32_t id) {
    std::lock_guard lock(partmeta_mutex_);

    if(id == UINT32_MAX) {
        id = generate_id();
    }

    PartialMetadataEntry entry{
        id,
        fmeta.absolute_path,
        fmeta.size,
        fmeta.hash,
        type,
        chunks,
        std::vector<bool>(chunks.size(), false),
        std::chrono::system_clock::now()
    };

    entries_.emplace(id, std::move(entry));
    return id;
}

void PartialMetadata::delete_partial_metadata(uint32_t id) {
    {
        std::lock_guard lock(partmeta_mutex_);
        entries_.erase(id);
        free_ids_.push(id);
    }
    save(); // persist the removal immediately -- otherwise a completed/discarded transfer
            // reappears as "resumable" from the stale on-disk file after the next restart
}

void PartialMetadata::mark_chunk_received(uint32_t id, uint32_t chunk_index) {
    std::lock_guard lock(partmeta_mutex_);

    auto it = entries_.find(id);
    if(it == entries_.end()) return;

    PartialMetadataEntry& entry = it->second;
    entry.chunk_state[chunk_index] = true;
    entry.last_activity = std::chrono::system_clock::now();
}

bool PartialMetadata::is_expired(uint32_t id) {
    std::lock_guard lock(partmeta_mutex_);

    auto it = entries_.find(id);
    if(it == entries_.end()) return false;

    PartialMetadataEntry entry = it->second;
    
    return (std::chrono::system_clock::now() - entry.last_activity) > TRANSFER_TIMEOUT;
}

void PartialMetadata::save() {
    std::lock_guard lock(partmeta_mutex_);

    json j;
    j["entries"] = json::array();

    for (const auto& [id, entry] : entries_) {
        j["entries"].push_back({
            {"id", entry.id},
            {"absolute_path", entry.absolute_path.string()},
            {"size", entry.size},
            {"file_hash", fsutils::hash_to_hex(entry.file_hash)},
            {"type", static_cast<int>(entry.type)},
            {"chunks", entry.chunks},
            {"chunk_state", entry.chunk_state},
            {"last_activity", std::chrono::duration_cast<std::chrono::seconds>(entry.last_activity.time_since_epoch()).count()}
        });
    }

    std::filesystem::path tmp = metadata_file_;
    tmp += ".tmp";

    std::ofstream f(tmp);
    if(!f) throw std::runtime_error("Failed to open partial file database for writing");

    f << j.dump(4);
    f.close();

    std::filesystem::rename(tmp, metadata_file_);
}

void PartialMetadata::cleanup_expired() {
    std::vector<uint32_t> expired_ids;
    {
        std::lock_guard lock(partmeta_mutex_);
        auto now = std::chrono::system_clock::now();
        for (const auto& [id, entry] : entries_) {
            if (now - entry.last_activity > TRANSFER_TIMEOUT) { // compare saved time with now
                expired_ids.push_back(id);
            }
        }
    }

    if (expired_ids.empty()) return;

    for (uint32_t id : expired_ids) {
        fsutils::remove_file(get_partial_path(id)); // get_partial_path()/delete_partial_metadata() lock internally,
        delete_partial_metadata(id);                 // so this must run outside the lock above
    }

    save();
}

void PartialMetadata::load() {
    std::lock_guard lock(partmeta_mutex_);
    entries_.clear();

    if(!fsutils::exists(metadata_file_)) return;

    std::ifstream f(metadata_file_);
    if(!f) throw std::runtime_error("Failed to open partial file database");

    // check if database is empty
    if(f.peek() == std::ifstream::traits_type::eof()) {
        entries_.clear();
        return;
    }

    json j;
    f >> j;

    for(const auto& e : j.at("entries")) {
        PartialMetadataEntry entry;
        entry.id = e.at("id").get<uint32_t>();
        entry.absolute_path = std::filesystem::path(e.at("absolute_path").get<std::string>());
        entry.size = e.at("size").get<uint32_t>();
        entry.file_hash = fsutils::hex_to_hash(e.at("file_hash").get<std::string>());
        entry.type = static_cast<TransferType>(e.at("type").get<int>());
        entry.chunks = e.at("chunks").get<std::vector<protocol::ChunkInfo>>();
        entry.chunk_state = e.at("chunk_state").get<std::vector<bool>>();
        auto ts = e.at("last_activity").get<uint64_t>();
        entry.last_activity = std::chrono::system_clock::time_point(std::chrono::seconds(ts));

        entries_.emplace(entry.id, std::move(entry));
        next_id_ = std::max(next_id_, entry.id + 1);
    }

    f.close();
}

std::vector<PartialMetadataEntry> PartialMetadata::get_entries() {
    std::lock_guard lock(partmeta_mutex_);
    std::vector<PartialMetadataEntry> result;
    result.reserve(entries_.size());
    for (const auto& [id, entry] : entries_) {
        result.push_back(entry);
    }
    return result;
}

std::optional<PartialMetadataEntry> PartialMetadata::get_entry(uint32_t id) {
    std::lock_guard lock(partmeta_mutex_);
    auto it = entries_.find(id);
    if(it == entries_.end()) return std::nullopt;
    return it->second;
}
