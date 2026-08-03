#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

// One remembered path from the last successful sync of a local<->remote pair.
struct SyncManifestEntry {
    std::string hash; // hash_to_hex of the file as it was after the last sync, empty for directories
    uint64_t mtime = 0; // Last modified at that time, seconds since epoch
    uint32_t size = 0; // Size at that time
    bool is_directory = false;
};

// File-based JSON database of "what did local and remote look like after the last successful sync
// of this pair" - the third leg of the three-way merge in sync_diff.hpp. Stored inside the local
// folder itself (<local_dir>/.minidrive-sync/<escaped remote_dir>.json) so it travels with the
// folder and needs no server-side state. Mirrors PartialMetadata's temp-file + rename save pattern.
//
// Known limitation: the baseline is keyed only by the remote directory path, not by which
// server/user it was synced against. Syncing one local folder against two different servers that
// reuse the same remote path name would share a baseline.
class SyncManifest {
public:
    SyncManifest(const std::filesystem::path& local_dir, const std::string& remote_dir);

    const std::map<std::string, SyncManifestEntry>& get_entries() const;
    const SyncManifestEntry* get(const std::string& relative_path) const;
    void put(const std::string& relative_path, const SyncManifestEntry& entry);
    void remove(const std::string& relative_path);
    const std::string& remote_dir() const;

    bool save(); // Atomic write, returns false instead of throwing

private:
    std::filesystem::path manifest_file_; // Path of the database file
    std::string remote_dir_; // Remote directory this baseline was recorded against
    std::map<std::string, SyncManifestEntry> entries_; // Relative path -> remembered state

    void load(); // Reads manifest_file_, treats any problem as "no baseline"
};
