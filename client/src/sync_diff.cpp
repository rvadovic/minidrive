#include "sync_diff.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <ctime>
#include <set>
#include <sstream>

namespace {

const SyncEntry* find_entry(const std::map<std::string, SyncEntry>& map, const std::string& key) {
    auto it = map.find(key);
    if(it == map.end()) return nullptr;
    return &it->second;
}

size_t path_depth(const std::string& relative_path) {
    return static_cast<size_t>(std::count(relative_path.begin(), relative_path.end(), '/'));
}

// Is any surviving local path inside this directory? Used to decide whether a remote directory can
// be removed - a directory that still holds something locally must stay.
bool has_local_children(const std::map<std::string, SyncEntry>& local, const std::string& dir) {
    std::string prefix = dir + "/";
    auto it = local.lower_bound(prefix);
    return it != local.end() && it->first.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

std::string join_remote(const std::string& remote_dir, const std::string& relative_path) {
    std::string base = remote_dir;
    while(!base.empty() && base.back() == '/') base.pop_back();
    if(base == ".") base.clear();
    if(base.empty()) return relative_path;
    if(relative_path.empty()) return base;
    return base + "/" + relative_path;
}

std::filesystem::path conflict_copy_path(const std::filesystem::path& local_path) {
    std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H%M%S", &tm);

    std::string name = local_path.stem().string()
                     + " (conflict copy from server, " + stamp + ")"
                     + local_path.extension().string();
    return local_path.parent_path() / name;
}

SyncPlan compute_sync_plan(const std::map<std::string, SyncEntry>& local,
                           const std::map<std::string, SyncEntry>& remote,
                           const std::map<std::string, SyncEntry>& baseline,
                           const std::filesystem::path& local_dir,
                           const std::string& remote_dir) {
    SyncPlan plan;

    // Ordered buckets, concatenated at the end. Phase order matters: directories must exist before
    // anything lands in them, renames must happen before the paths they free get reused, and
    // deletes must precede uploads because the server's upload()/move()/copy() all refuse an
    // existing destination - replacing a remote file means "delete it, then upload".
    // Conflict copies are pulled first of all: the very next phases free and overwrite the remote
    // paths they read from, so anything later would be fetching bytes that are already gone.
    std::vector<SyncOp> conflict_downloads;
    std::vector<SyncOp> pre_deletes; // Type conflicts: the remote path must be freed before phase 1
    std::vector<SyncOp> mkdirs;
    std::vector<SyncOp> moves;
    std::vector<SyncOp> deletes;
    std::vector<SyncOp> rmdirs;
    std::vector<SyncOp> transfers;
    std::vector<SyncOp> copies;

    // Candidates for move/copy detection, filled during classification and resolved afterwards.
    std::vector<std::pair<std::string, SyncEntry>> new_local_files; // No remote and no baseline counterpart
    std::vector<std::pair<std::string, SyncEntry>> removed_remote_files; // Deleted locally, untouched remotely

    auto remote_of = [&](const std::string& relative_path) {
        return join_remote(remote_dir, relative_path);
    };

    // Queues an upload, preceded by a delete when the remote path is already taken.
    auto emit_upload = [&](const std::string& relative_path, const SyncEntry& entry, bool remote_exists) {
        if(remote_exists) {
            SyncOp del{};
            del.type = SyncOpType::DELETE_REMOTE;
            del.remote_path = remote_of(relative_path);
            deletes.push_back(del);
        }
        SyncOp op{};
        op.type = SyncOpType::UPLOAD;
        op.local_path = local_dir / std::filesystem::path(relative_path);
        op.remote_path = remote_of(relative_path);
        op.relative_path = relative_path;
        op.entry = entry;
        transfers.push_back(op);
    };

    auto emit_download = [&](const std::string& relative_path, const SyncEntry& entry) {
        SyncOp op{};
        op.type = SyncOpType::DOWNLOAD;
        op.local_path = local_dir / std::filesystem::path(relative_path);
        op.remote_path = remote_of(relative_path);
        op.relative_path = relative_path;
        op.entry = entry;
        transfers.push_back(op);
    };

    // A real conflict: both sides changed to something different since the baseline. Nothing is
    // discarded - the local file stays canonical on the remote path, and the server's version is
    // pulled down beside it under a new name.
    auto emit_conflict = [&](const std::string& relative_path, const SyncEntry& local_entry, bool remote_exists) {
        std::filesystem::path copy = conflict_copy_path(local_dir / std::filesystem::path(relative_path));
        SyncOp op{};
        op.type = SyncOpType::CONFLICT_DOWNLOAD;
        op.local_path = copy;
        op.remote_path = remote_of(relative_path);
        conflict_downloads.push_back(op);
        plan.conflicts.push_back(relative_path + " -> server version saved as \"" + copy.filename().string() + "\"");
        emit_upload(relative_path, local_entry, remote_exists);
    };

    std::set<std::string> keys;
    for(const auto& item : local) keys.insert(item.first);
    for(const auto& item : remote) keys.insert(item.first);
    for(const auto& item : baseline) keys.insert(item.first);

    for(const std::string& key : keys) {
        const SyncEntry* l = find_entry(local, key);
        const SyncEntry* r = find_entry(remote, key);
        const SyncEntry* b = find_entry(baseline, key);

        bool directory_involved = (l && l->is_directory) || (r && r->is_directory) || (b && b->is_directory);

        if(directory_involved) {
            if(l && l->is_directory) {
                if(r && r->is_directory) {
                    plan.baseline_set[key] = *l;
                } else if(r) { // A file remotely, a directory locally
                    SyncOp del{};
                    del.type = SyncOpType::DELETE_REMOTE;
                    del.remote_path = remote_of(key);
                    pre_deletes.push_back(del);
                    SyncOp op{};
                    op.type = SyncOpType::MKDIR_REMOTE;
                    op.remote_path = remote_of(key);
                    op.relative_path = key;
                    op.entry = *l;
                    mkdirs.push_back(op);
                    plan.conflicts.push_back(key + " -> is a directory locally but a file on the server");
                } else {
                    SyncOp op{};
                    op.type = SyncOpType::MKDIR_REMOTE;
                    op.remote_path = remote_of(key);
                    op.relative_path = key;
                    op.entry = *l;
                    mkdirs.push_back(op);
                }
            } else if(l) { // A file locally, a directory remotely or in the baseline
                if(r && r->is_directory) {
                    SyncOp op{};
                    op.type = SyncOpType::RMDIR_REMOTE;
                    op.remote_path = remote_of(key);
                    rmdirs.push_back(op);
                    plan.conflicts.push_back(key + " -> is a file locally but a directory on the server");
                }
                emit_upload(key, *l, false);
            } else if(r && r->is_directory) {
                if(b && b->is_directory && !has_local_children(local, key)) {
                    SyncOp op{};
                    op.type = SyncOpType::RMDIR_REMOTE;
                    op.remote_path = remote_of(key);
                    op.relative_path_from = key;
                    rmdirs.push_back(op);
                } else if(!b) { // New on the server only - mirror it locally
                    plan.local_mkdirs.push_back(local_dir / std::filesystem::path(key));
                    plan.baseline_set[key] = *r;
                }
            } else if(b) {
                plan.baseline_drop.push_back(key);
            }
            continue;
        }

        if(l && r && b) {
            bool local_changed = l->hash != b->hash;
            bool remote_changed = r->hash != b->hash;
            if(!local_changed && !remote_changed) {
                plan.skipped++;
                plan.baseline_set[key] = *l;
            } else if(local_changed && !remote_changed) {
                emit_upload(key, *l, true);
            } else if(!local_changed && remote_changed) {
                emit_download(key, *r);
            } else if(l->hash == r->hash) { // Both sides changed to the same content
                plan.skipped++;
                plan.baseline_set[key] = *l;
            } else {
                emit_conflict(key, *l, true);
            }
        } else if(l && r) { // Both new, no baseline
            if(l->hash == r->hash) {
                plan.skipped++;
                plan.baseline_set[key] = *l;
            } else {
                emit_conflict(key, *l, true);
            }
        } else if(l && b) { // Removed on the server since the baseline
            if(l->hash == b->hash) {
                plan.local_deletes.push_back(local_dir / std::filesystem::path(key));
                plan.baseline_drop.push_back(key);
            } else { // Never let a remote delete destroy a newer local edit
                emit_upload(key, *l, false);
                plan.conflicts.push_back(key + " -> changed locally but deleted on the server, re-uploaded");
            }
        } else if(l) {
            new_local_files.emplace_back(key, *l);
        } else if(r && b) { // Removed locally since the baseline
            if(r->hash == b->hash) {
                removed_remote_files.emplace_back(key, *r);
            } else { // Never let a stale local delete destroy a newer remote edit
                emit_download(key, *r);
                plan.conflicts.push_back(key + " -> deleted locally but changed on the server, re-downloaded");
            }
        } else if(r) {
            emit_download(key, *r);
        } else if(b) {
            plan.baseline_drop.push_back(key);
        }
    }

    // Move detection: a hash that disappears from one remote path and appears at a brand-new local
    // path is a rename, not a delete plus a re-upload. Only ever fires for byte-identical content,
    // so a file that was moved *and* edited simply falls out as an independent delete and upload.
    std::map<std::string, std::vector<size_t>> removed_by_hash;
    for(size_t i = 0; i < removed_remote_files.size(); ++i) {
        removed_by_hash[removed_remote_files[i].second.hash].push_back(i);
    }

    std::vector<bool> removed_consumed(removed_remote_files.size(), false);
    std::vector<bool> new_consumed(new_local_files.size(), false);

    for(size_t i = 0; i < new_local_files.size(); ++i) {
        auto it = removed_by_hash.find(new_local_files[i].second.hash);
        if(it == removed_by_hash.end()) continue;

        size_t source = std::numeric_limits<size_t>::max();
        for(size_t candidate : it->second) {
            if(!removed_consumed[candidate]) { source = candidate; break; }
        }
        if(source == std::numeric_limits<size_t>::max()) continue;

        removed_consumed[source] = true;
        new_consumed[i] = true;

        SyncOp op{};
        op.type = SyncOpType::MOVE_REMOTE;
        op.local_path = local_dir / std::filesystem::path(new_local_files[i].first);
        op.remote_path = remote_of(new_local_files[i].first);
        op.remote_path_from = remote_of(removed_remote_files[source].first);
        op.relative_path = new_local_files[i].first;
        op.relative_path_from = removed_remote_files[source].first;
        op.entry = new_local_files[i].second;
        moves.push_back(op);
    }

    for(size_t i = 0; i < removed_remote_files.size(); ++i) {
        if(removed_consumed[i]) continue;
        SyncOp op{};
        op.type = SyncOpType::DELETE_REMOTE;
        op.remote_path = remote_of(removed_remote_files[i].first);
        op.relative_path_from = removed_remote_files[i].first;
        deletes.push_back(op);
    }

    // Renames have to run in an order that never lands on a path another rename has not vacated
    // yet. Kahn's algorithm over "B frees the path A wants" edges; anything left over is a true
    // cycle (a swap), which is deliberately downgraded to delete + upload rather than broken with
    // temporary names.
    if(!moves.empty()) {
        std::map<std::string, size_t> by_source;
        for(size_t i = 0; i < moves.size(); ++i) by_source[moves[i].remote_path_from] = i;

        std::vector<std::vector<size_t>> successors(moves.size());
        std::vector<size_t> indegree(moves.size(), 0);
        for(size_t i = 0; i < moves.size(); ++i) {
            auto it = by_source.find(moves[i].remote_path);
            if(it != by_source.end() && it->second != i) {
                successors[it->second].push_back(i); // The occupant must move away first
                indegree[i]++;
            }
        }

        std::vector<size_t> ready;
        for(size_t i = 0; i < moves.size(); ++i) if(indegree[i] == 0) ready.push_back(i);

        std::vector<SyncOp> ordered;
        std::vector<bool> emitted(moves.size(), false);
        while(!ready.empty()) {
            size_t node = ready.back();
            ready.pop_back();
            ordered.push_back(moves[node]);
            emitted[node] = true;
            for(size_t next : successors[node]) {
                if(--indegree[next] == 0) ready.push_back(next);
            }
        }

        for(size_t i = 0; i < moves.size(); ++i) {
            if(emitted[i]) continue;
            SyncOp del{};
            del.type = SyncOpType::DELETE_REMOTE;
            del.remote_path = moves[i].remote_path_from;
            del.relative_path_from = moves[i].relative_path_from;
            deletes.push_back(del);
            emit_upload(moves[i].relative_path, moves[i].entry, false);
        }

        moves = std::move(ordered);
    }

    // Copy optimisation: a brand-new local file whose content already sits somewhere on the server
    // (or is about to) becomes a server-side copy instead of a second upload of identical bytes.
    std::map<std::string, std::string> source_by_hash;
    for(const auto& [key, entry] : remote) {
        if(entry.is_directory) continue;
        const SyncEntry* l = find_entry(local, key);
        if(l) {
            // The path keeps its current content only when both sides already agree on it -
            // anything being replaced by an upload holds different bytes by the time copies run.
            if(l->is_directory || l->hash != entry.hash) continue;
        } else if(baseline.count(key) != 0) {
            continue; // Being deleted or moved away
        }
        source_by_hash.emplace(entry.hash, remote_of(key));
    }
    for(const SyncOp& op : moves) source_by_hash[op.entry.hash] = op.remote_path;
    for(const SyncOp& op : transfers) {
        if(op.type == SyncOpType::UPLOAD) source_by_hash.emplace(op.entry.hash, op.remote_path);
    }

    for(size_t i = 0; i < new_local_files.size(); ++i) {
        if(new_consumed[i]) continue;
        const auto& [key, entry] = new_local_files[i];

        auto it = source_by_hash.find(entry.hash);
        if(it != source_by_hash.end()) {
            SyncOp op{};
            op.type = SyncOpType::COPY_REMOTE;
            op.local_path = local_dir / std::filesystem::path(key);
            op.remote_path = remote_of(key);
            op.remote_path_from = it->second;
            op.relative_path = key;
            op.entry = entry;
            copies.push_back(op);
            continue;
        }

        emit_upload(key, entry, false);
        source_by_hash.emplace(entry.hash, remote_of(key));
    }

    std::stable_sort(mkdirs.begin(), mkdirs.end(), [](const SyncOp& a, const SyncOp& b) {
        return path_depth(a.remote_path) < path_depth(b.remote_path);
    });
    std::stable_sort(rmdirs.begin(), rmdirs.end(), [](const SyncOp& a, const SyncOp& b) {
        return path_depth(a.remote_path) > path_depth(b.remote_path);
    });
    std::sort(plan.local_mkdirs.begin(), plan.local_mkdirs.end());

    plan.ops.insert(plan.ops.end(), conflict_downloads.begin(), conflict_downloads.end());
    plan.ops.insert(plan.ops.end(), pre_deletes.begin(), pre_deletes.end());
    plan.ops.insert(plan.ops.end(), mkdirs.begin(), mkdirs.end());
    plan.ops.insert(plan.ops.end(), moves.begin(), moves.end());
    plan.ops.insert(plan.ops.end(), deletes.begin(), deletes.end());
    plan.ops.insert(plan.ops.end(), rmdirs.begin(), rmdirs.end());
    plan.ops.insert(plan.ops.end(), transfers.begin(), transfers.end());
    plan.ops.insert(plan.ops.end(), copies.begin(), copies.end());

    return plan;
}
