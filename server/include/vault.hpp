#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "protocol/message.hpp"

// Server-side storage for an end-to-end encrypted account.
//
// Everything here is opaque to the server on purpose. It stores salts, parameters and sealed key
// blobs, hands them back to whoever authenticates, and never holds a key that opens any of them.
// Two files, both following the same file-backed-JSON-with-atomic-save pattern as PartialMetadata
// and SyncManifest, living in <user_root>/private/<user>/.vault/ next to .partial/:
//
//   vault.json         - the salt, the Argon2id parameters, VK sealed under the password-derived
//                        master key, and the enrolled device table. Small and rarely written.
//   dek_manifest.json  - one entry per stored file: that file's data key sealed under VK, plus the
//                        plaintext hash and size SYNC compares on. Written on every upload, which
//                        is exactly why it is a separate file from the device table.

// One device enrolled in a user's vault, as the server records it.
struct VaultDevice {
    std::string device_id;
    std::string device_name;
    std::string algorithm; // "x25519+mlkem768"
    std::string x25519_pub; // base64 public half
    std::string mlkem_pub; // base64 public half
    protocol::WrappedBlob wrapped_vk; // VK sealed to this device
    uint64_t created_at = 0;
    uint64_t last_seen = 0;
};

class VaultStore {
public:
    explicit VaultStore(std::filesystem::path file);

    bool is_initialized(); // True once VAULT_INIT has run for this account
    protocol::VaultInfo get_info(); // enabled=false when there is no vault yet
    // Writes the salt, parameters and wrapped VK exactly once. Refuses to overwrite an existing
    // vault: doing so would orphan every file already encrypted under the old VK.
    bool initialize(const protocol::VaultInfo& info, std::string& error);

    std::vector<VaultDevice> get_devices();
    std::optional<VaultDevice> get_device(const std::string& device_id);
    bool add_device(const VaultDevice& device, std::string& error);
    bool remove_device(const std::string& device_id);
    void touch_device(const std::string& device_id); // Records last_seen after a device unlock

private:
    std::filesystem::path file_;
    std::mutex mutex_;
    bool initialized_ = false;
    protocol::VaultInfo info_;
    std::vector<VaultDevice> devices_;

    void load(); // Called under mutex_
    bool save(); // Called under mutex_, temp file + rename
};

// One stored file's key material, keyed by path relative to the user's files/ directory.
struct DekEntry {
    protocol::WrappedBlob wrapped_dek;
    std::string plaintext_hash; // What the file hashed to before encryption - SYNC's comparison key
    uint32_t plaintext_size = 0;
};

class DekManifest {
public:
    explicit DekManifest(std::filesystem::path file);

    std::optional<DekEntry> get(const std::string& relative_path);
    std::map<std::string, DekEntry> get_entries();
    void put(const std::string& relative_path, const DekEntry& entry);
    void remove(const std::string& relative_path);
    void remove_subtree(const std::string& relative_path); // The path itself and everything under it
    void rename(const std::string& from, const std::string& to); // Whole subtree when from is a directory
    // Duplicates the source's entry, so a server-side COPY of identical ciphertext stays openable
    // under the very same data key. Not nonce reuse: it is the same plaintext under the same key.
    void copy(const std::string& from, const std::string& to);

private:
    std::filesystem::path file_;
    std::mutex mutex_;
    std::map<std::string, DekEntry> entries_;

    void load();
    bool save();
};
