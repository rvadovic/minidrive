#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <sodium.h>

#include "filesystem/utils.hpp"
#include "protocol/message.hpp"

// Client-side vault crypto. This is the whole zero-knowledge boundary: everything in this file runs
// where the plaintext already is, and nothing it produces lets the server read a byte of content.
//
// Key hierarchy (see docs/vault.md):
//   password --Argon2id(vault_salt)--> MK  --unwraps--> VK --unwraps--> per-file DEK --> chunks
//   device private keys              -----unwrap------> VK      (a shortcut past Argon2id only)
namespace vault {

namespace fs = std::filesystem;

inline constexpr size_t KEY_BYTES = crypto_aead_xchacha20poly1305_ietf_KEYBYTES;   // 32
inline constexpr size_t NONCE_BYTES = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES; // 24
inline constexpr size_t TAG_BYTES = crypto_aead_xchacha20poly1305_ietf_ABYTES;      // 16
inline constexpr size_t SALT_BYTES = crypto_pwhash_SALTBYTES;                       // 16

// Plaintext bytes per chunk. Chosen so that plaintext + tag lands exactly on fsutils::CHUNK_SIZE:
// every ciphertext chunk but the last is then exactly one CHUNK_SIZE, which is what lets the
// existing "offset = CHUNK_SIZE * index" arithmetic on both sides stay untouched by encryption.
inline constexpr uint32_t PLAIN_CHUNK_SIZE = fsutils::CHUNK_SIZE - static_cast<uint32_t>(TAG_BYTES);

using Key = std::array<uint8_t, KEY_BYTES>;

// Argon2id tuning for master-key derivation. Deliberately heavier than the login gate in
// server/src/password.cpp (which uses the INTERACTIVE tier): that protects one session against an
// offline crack of a leaked users.json, this one stands between a password and every file.
struct Argon2Params {
    uint64_t opslimit = crypto_pwhash_OPSLIMIT_MODERATE;
    uint64_t memlimit = crypto_pwhash_MEMLIMIT_MODERATE;
    int algorithm = crypto_pwhash_ALG_ARGON2ID13;
};

// Ceilings on the parameters a *server* is allowed to hand back at login. Generous next to the
// MODERATE tier actually used (3 / 256 MiB), so per-account cost can still be raised later, but
// bounded: an unbounded memlimit is a lever a hostile server could pull on this machine's memory.
inline constexpr uint64_t MAX_OPSLIMIT = 16;
inline constexpr uint64_t MAX_MEMLIMIT = 2ull * 1024 * 1024 * 1024; // 2 GiB

std::vector<uint8_t> random_bytes(size_t count);
Key random_key();
void wipe(Key& key);

// False when the parameters are unusable or libsodium runs out of memory for them
bool derive_master_key(const std::string& password, const std::vector<uint8_t>& salt,
                       const Argon2Params& params, Key& out);

// XChaCha20-Poly1305 with a fresh random nonce. Used for VK-under-MK and DEK-under-VK alike.
bool wrap_key(const Key& key_to_wrap, const Key& wrapping_key, protocol::WrappedBlob& out);
bool unwrap_key(const protocol::WrappedBlob& blob, const Key& wrapping_key, Key& out);

// Whole-file encryption, chunk by chunk, under one data key.
//
// Each chunk is sealed with a nonce derived deterministically from (DEK, chunk index) - safe
// without a counter because every file has its own DEK - and authenticated against additional data
// carrying that index and whether the chunk is the last one. That additional data is what makes
// reordering, dropping and appending chunks detectable rather than merely unlikely.
bool encrypt_file(const fs::path& plain, const fs::path& cipher, const Key& dek, std::string& error);
bool decrypt_file(const fs::path& cipher, const fs::path& plain, const Key& dek, std::string& error);

// Ciphertext length for a given plaintext length, so a caller can check the 4 GiB transfer cap
// before spending time encrypting.
uint64_t ciphertext_size(uint64_t plaintext_size);

// One device's hybrid key pair. The private halves are what never leave the machine.
struct DeviceKeys {
    std::string device_id;
    std::string device_name;
    std::array<uint8_t, crypto_scalarmult_curve25519_BYTES> x25519_pub{};
    std::array<uint8_t, crypto_scalarmult_curve25519_SCALARBYTES> x25519_sec{};
    std::vector<uint8_t> mlkem_pub; // 1184 bytes for ML-KEM-768
    std::vector<uint8_t> mlkem_sec; // 2400 bytes for ML-KEM-768
};

// ML-KEM-768 comes from OpenSSL 3.5+, and this is EVP_PKEY_encapsulate/decapsulate directly - not
// the TLS code path, which only negotiates ML-KEM inside a handshake and exposes no general KEM.
bool mlkem_available();

bool generate_device_keys(const std::string& device_name, DeviceKeys& out, std::string& error);

// Seals VK to a device's public halves: X25519 with a fresh ephemeral key, ML-KEM-768
// encapsulation, and both shared secrets bound into one wrapping key together with the full
// transcript. An attacker has to break both to learn VK, which is the point of a hybrid.
bool wrap_vk_to_device(const Key& vault_key, const std::vector<uint8_t>& x25519_pub,
                       const std::vector<uint8_t>& mlkem_pub, protocol::WrappedBlob& out,
                       std::string& error);
bool unwrap_vk_with_device(const protocol::WrappedBlob& blob, const DeviceKeys& keys, Key& out,
                           std::string& error);

// Device key storage, one file per (user, server) pairing under the client root. Keeping a private
// key on disk in the clear is a real limitation, documented in docs/vault.md: it buys skipping
// Argon2id on a trusted machine, and is worth exactly as much as the file's permissions.
bool save_device_keys(const fs::path& file, const std::string& account, const DeviceKeys& keys);
std::optional<DeviceKeys> load_device_keys(const fs::path& file, const std::string& account);
void forget_device_keys(const fs::path& file, const std::string& account);

} // namespace vault
