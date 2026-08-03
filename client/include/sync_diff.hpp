#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "sync_manifest.hpp"

// What one queued batch item asks the client to do. Every remote-side type maps onto an existing
// single-item command handler on the server - no new wire primitive is introduced.
enum class SyncOpType {
    MKDIR_REMOTE,
    RMDIR_REMOTE,
    DELETE_REMOTE,
    MOVE_REMOTE,
    COPY_REMOTE,
    UPLOAD,
    DOWNLOAD,
    CONFLICT_DOWNLOAD // Download the server's version of a conflicted file under a new local name
};

// Uniform view of one path, whatever side it came from (local scan, remote listing, or baseline).
struct SyncEntry {
    std::string hash; // hash_to_hex, empty for directories
    uint64_t mtime = 0;
    uint32_t size = 0;
    bool is_directory = false;
};

struct SyncOp {
    SyncOpType type;
    std::filesystem::path local_path; // Local file involved (UPLOAD / DOWNLOAD / CONFLICT_DOWNLOAD)
    std::string remote_path; // Full remote destination path as sent to the server
    std::string remote_path_from; // Full remote source path (MOVE_REMOTE / COPY_REMOTE)
    std::string relative_path; // Baseline key to update on success, empty => no baseline update
    std::string relative_path_from; // Baseline key to drop on success (MOVE_REMOTE)
    SyncEntry entry; // Baseline value to record on success
};

struct SyncPlan {
    std::vector<SyncOp> ops; // Ordered, ready to execute one at a time
    std::vector<std::filesystem::path> local_mkdirs; // New local directories, created synchronously
    std::vector<std::filesystem::path> local_deletes; // Local files removed on the server, deleted synchronously
    std::map<std::string, SyncEntry> baseline_set; // Baseline updates that need no network op
    std::vector<std::string> baseline_drop; // Baseline keys that are gone on both sides
    std::vector<std::string> conflicts; // Human-readable description, one per conflicted path
    size_t skipped = 0; // Unchanged paths, for the summary line
};

// Builds "<remote_dir>/<relative>", tolerating an empty or "."/trailing-slash remote_dir.
std::string join_remote(const std::string& remote_dir, const std::string& relative_path);

// Three-way merge of the local tree, the remote tree, and the baseline recorded by the last
// successful sync. Pure: no I/O, no Client state, so it can be reasoned about (and tested) alone.
SyncPlan compute_sync_plan(const std::map<std::string, SyncEntry>& local,
                           const std::map<std::string, SyncEntry>& remote,
                           const std::map<std::string, SyncEntry>& baseline,
                           const std::filesystem::path& local_dir,
                           const std::string& remote_dir);

// Local file name a conflicting server-side version is downloaded to:
// "<stem> (conflict copy from server, YYYY-MM-DD HHMMSS)<ext>"
std::filesystem::path conflict_copy_path(const std::filesystem::path& local_path);
