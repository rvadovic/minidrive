#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <cstdint>
#include <sodium.h>

namespace protocol {

using json = nlohmann::json;

// Every member below carries a default initializer. Not decoration: these structs are built all
// over the codebase with aggregate literals that list only the first few fields
// (`protocol::Response{status, code, message, file_hash}`), and without defaults each such literal
// raises one -Wmissing-field-initializers warning per omitted field. The vault added four fields to
// Response and five to Request, which tripled that noise across ~60 call sites. Defaults silence
// the whole class at its source, and make a plain `protocol::Request req;` zero-initialized rather
// than holding indeterminate integers.

// Info about chunk for a file
struct ChunkInfo {
    uint32_t index = 0; // chunk index
    uint32_t size = 0; // size of chunk
    std::string chunk_hash{}; //hash_to_hex value
};

// One entry of a recursive directory listing (SYNC / directory transfers)
struct FileEntry {
    std::string relative_path{}; // Path relative to the listed directory, '/' separated
    uint32_t size = 0; // Size of file, 0 for directories
    std::string file_hash{}; // hash_to_hex value, empty for directories
    uint64_t last_modified = 0; // Seconds since epoch
    bool is_directory = false;
};

// One storage medium the server has configured (TIERS responses).
// Deliberately carries no filesystem path - the server's disk layout is not client business.
struct TierInfo {
    std::string name{}; // Logical tier name, used as the SET_TIER argument
    std::string description{}; // Admin supplied text
    bool is_current = false; // True for the tier the calling user is on
};

// One key sealed under another key, as it travels and as the server stores it. The server never
// holds the key that opens any of these - to it every field here is opaque base64.
struct WrappedBlob {
    std::string ciphertext{}; // base64 XChaCha20-Poly1305 ciphertext+tag
    std::string nonce{}; // base64, 24 bytes
    std::string kem_ct{}; // base64 ML-KEM-768 ciphertext, device path only
    std::string eph_pub{}; // base64 ephemeral X25519 public key, device path only

    bool empty() const { return ciphertext.empty(); }
};

// One device enrolled in a user's vault. Only public halves and the VK wrapped to them are ever
// sent to the server; the matching private keys never leave the device that generated them.
struct DeviceInfo {
    std::string device_id{}; // Server-visible identifier, used by REVOKE_DEVICE
    std::string device_name{}; // Human label chosen at enrollment
    std::string algorithm{}; // "x25519+mlkem768", so a future scheme is distinguishable
    std::string x25519_pub{}; // base64
    std::string mlkem_pub{}; // base64
    WrappedBlob wrapped_vk{}; // VK sealed to this device's hybrid public keys
    uint64_t created_at = 0;
    uint64_t last_seen = 0;
};

// Everything a client needs to unlock a vault it already owns, and everything the server keeps
// about it. The server can reproduce none of it: the salt and parameters only describe how the
// client should re-derive its master key from a password the server never sees.
struct VaultInfo {
    bool enabled = false;
    std::string salt{}; // base64 per-user Argon2id salt
    uint64_t opslimit = 0; // Argon2id parameters, stored so they can be raised per account later
    uint64_t memlimit = 0;
    int algorithm = 0; // crypto_pwhash algorithm id
    WrappedBlob wrapped_vk_password{}; // VK sealed under the password-derived master key
};

// Client request JSON protocol
struct Request {
    std::string cmd{}; // command
    std::string first_argument{};
    std::string second_argument{};
    uint32_t size = 0;
    std::string file_hash{};
    std::vector<ChunkInfo> chunks{};
    // Vault fields. All conditionally serialized, so a plain (non-vaulted) account produces
    // byte-identical requests to the ones this protocol has always sent.
    VaultInfo vault{}; // VAULT_INIT: the key material the client generated for its new vault
    DeviceInfo device{}; // ENROLL_DEVICE: this device's public halves plus VK wrapped to them
    WrappedBlob wrapped_dek{}; // UPLOAD: this file's data key, sealed under the vault key
    std::string plaintext_hash{}; // UPLOAD: hash of the file before encryption, what SYNC compares on
    uint32_t plaintext_size = 0; // UPLOAD: size before encryption
};

// Server rsponse JSON protocol
struct Response {
    std::string status{}; // server status
    uint16_t code = 0; // code from protocol/codes.hpp
    std::string message{};
    std::string file_hash{};
    std::vector<ChunkInfo> chunks{};
    std::vector<FileEntry> files{}; // Recursive listing, only used by SYNC responses
    std::vector<TierInfo> tiers{}; // Configured storage media, only used by TIERS responses
    VaultInfo vault{}; // Attached to the response that completes authentication, when a vault exists
    std::vector<DeviceInfo> devices{}; // DEVICES listing, and the enrolled devices at login
    WrappedBlob wrapped_dek{}; // DOWNLOAD: the requested file's data key, sealed under the vault key
    std::string plaintext_hash{}; // DOWNLOAD: what the decrypted file must hash to
};

// Binary protocol for file transfers
#pragma pack(push, 1)
struct ChunkHeader {
    uint32_t transfer_id;
    uint32_t index; // chunk index
    uint32_t size; // chunk size
    uint8_t flags; // defined in protocol/flags.hpp
};
#pragma pack(pop)

// Parsing
void to_json(json& json, const ChunkInfo& ci);
void from_json(const json& json, ChunkInfo& ci);
void to_json(json& json, const FileEntry& fe);
void from_json(const json& json, FileEntry& fe);
void to_json(json& json, const TierInfo& ti);
void from_json(const json& json, TierInfo& ti);
void to_json(json& json, const WrappedBlob& wb);
void from_json(const json& json, WrappedBlob& wb);
void to_json(json& json, const DeviceInfo& di);
void from_json(const json& json, DeviceInfo& di);
void to_json(json& json, const VaultInfo& vi);
void from_json(const json& json, VaultInfo& vi);
void to_json(json& json, const Request& req);
void to_json(json& json, const Response& res);
void from_json(const json& json, Request& req);
void from_json(const json& json, Response& res);
}