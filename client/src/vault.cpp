#include "vault.hpp"

#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>

#include "crypto/base64.hpp"

#ifdef MINIDRIVE_ENABLE_TLS
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#endif

using nlohmann::json;

namespace vault {

namespace {

// Chunk index as it appears both in the nonce derivation and in the authenticated additional data.
// Big-endian, matching every other integer this protocol puts on the wire, so the format is not
// silently tied to the endianness of whatever machine happened to write the file.
void put_be32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>((value >> 24) & 0xff);
    out[1] = static_cast<uint8_t>((value >> 16) & 0xff);
    out[2] = static_cast<uint8_t>((value >> 8) & 0xff);
    out[3] = static_cast<uint8_t>(value & 0xff);
}

// nonce = BLAKE2b(key = DEK, message = chunk index). Deterministic, and safe precisely because the
// key is per file: a (key, nonce) pair is therefore never reused across two different plaintexts.
void chunk_nonce(const Key& dek, uint32_t index, std::array<uint8_t, NONCE_BYTES>& out) {
    std::array<uint8_t, 4> counter{};
    put_be32(counter.data(), index);
    crypto_generichash(out.data(), out.size(), counter.data(), counter.size(), dek.data(), dek.size());
}

// Additional data binds each chunk to its position and to whether it ends the file. Without it a
// server could silently truncate a file, or swap two chunks, and every individual chunk would
// still authenticate perfectly.
std::array<uint8_t, 5> chunk_ad(uint32_t index, bool is_last) {
    std::array<uint8_t, 5> ad{};
    put_be32(ad.data(), index);
    ad[4] = is_last ? 1 : 0;
    return ad;
}

std::string blob_field(const std::string& base64, std::vector<uint8_t>& out) {
    if(!mdcrypto::from_base64(base64, out)) return "malformed key material";
    return std::string();
}

} // namespace

std::vector<uint8_t> random_bytes(size_t count) {
    std::vector<uint8_t> out(count);
    randombytes_buf(out.data(), out.size());
    return out;
}

Key random_key() {
    Key key{};
    randombytes_buf(key.data(), key.size());
    return key;
}

void wipe(Key& key) {
    sodium_memzero(key.data(), key.size());
}

bool derive_master_key(const std::string& password, const std::vector<uint8_t>& salt,
                       const Argon2Params& params, Key& out) {
    if(salt.size() != SALT_BYTES) return false;
    if(params.opslimit == 0 || params.memlimit == 0) return false;

    // These parameters arrive from the server, and the whole point of the vault is not trusting it.
    // Unbounded, they are a lever on this machine's memory and CPU: a hostile or compromised server
    // could answer a login with a 100 GiB memlimit. The ceilings are far above the MODERATE tier
    // actually used, so raising per-account cost later stays possible.
    if(params.opslimit > MAX_OPSLIMIT || params.memlimit > MAX_MEMLIMIT) return false;

    return crypto_pwhash(out.data(), out.size(),
                         password.c_str(), password.size(),
                         salt.data(),
                         params.opslimit, static_cast<size_t>(params.memlimit),
                         params.algorithm) == 0;
}

bool wrap_key(const Key& key_to_wrap, const Key& wrapping_key, protocol::WrappedBlob& out) {
    std::array<uint8_t, NONCE_BYTES> nonce{};
    randombytes_buf(nonce.data(), nonce.size());

    std::vector<uint8_t> ciphertext(KEY_BYTES + TAG_BYTES);
    unsigned long long ciphertext_len = 0;

    if(crypto_aead_xchacha20poly1305_ietf_encrypt(
           ciphertext.data(), &ciphertext_len,
           key_to_wrap.data(), key_to_wrap.size(),
           nullptr, 0, nullptr,
           nonce.data(), wrapping_key.data()) != 0) {
        return false;
    }

    ciphertext.resize(ciphertext_len);
    out.ciphertext = mdcrypto::to_base64(ciphertext);
    out.nonce = mdcrypto::to_base64(nonce.data(), nonce.size());
    return true;
}

bool unwrap_key(const protocol::WrappedBlob& blob, const Key& wrapping_key, Key& out) {
    std::vector<uint8_t> ciphertext;
    std::vector<uint8_t> nonce;
    if(!mdcrypto::from_base64(blob.ciphertext, ciphertext)) return false;
    if(!mdcrypto::from_base64(blob.nonce, nonce)) return false;
    if(nonce.size() != NONCE_BYTES) return false;
    if(ciphertext.size() != KEY_BYTES + TAG_BYTES) return false;

    unsigned long long plaintext_len = 0;
    if(crypto_aead_xchacha20poly1305_ietf_decrypt(
           out.data(), &plaintext_len, nullptr,
           ciphertext.data(), ciphertext.size(),
           nullptr, 0,
           nonce.data(), wrapping_key.data()) != 0) {
        return false;
    }
    return plaintext_len == KEY_BYTES;
}

uint64_t ciphertext_size(uint64_t plaintext_size) {
    if(plaintext_size == 0) return 0;
    const uint64_t chunks = (plaintext_size + PLAIN_CHUNK_SIZE - 1) / PLAIN_CHUNK_SIZE;
    return plaintext_size + chunks * TAG_BYTES;
}

bool encrypt_file(const fs::path& plain, const fs::path& cipher, const Key& dek, std::string& error) {
    const uint64_t plain_size = fsutils::get_file_size(plain);
    if(plain_size == fsutils::SIZE_ERROR) {
        error = "Cannot read " + plain.string();
        return false;
    }
    if(plain_size == 0) {
        error = "Empty files cannot be encrypted.";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(cipher.parent_path(), ec);

    std::ifstream in(plain, std::ios::binary);
    if(!in) {
        error = "Cannot open " + plain.string();
        return false;
    }
    std::ofstream out(cipher, std::ios::binary | std::ios::trunc);
    if(!out) {
        error = "Cannot write " + cipher.string();
        return false;
    }

    const uint64_t total_chunks = (plain_size + PLAIN_CHUNK_SIZE - 1) / PLAIN_CHUNK_SIZE;

    std::vector<uint8_t> block(PLAIN_CHUNK_SIZE);
    std::vector<uint8_t> sealed(PLAIN_CHUNK_SIZE + TAG_BYTES);

    for(uint64_t index = 0; index < total_chunks; ++index) {
        in.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(block.size()));
        const std::streamsize read = in.gcount();
        if(read <= 0) {
            error = "Unexpected end of " + plain.string();
            return false;
        }

        std::array<uint8_t, NONCE_BYTES> nonce{};
        chunk_nonce(dek, static_cast<uint32_t>(index), nonce);
        const std::array<uint8_t, 5> ad = chunk_ad(static_cast<uint32_t>(index), index + 1 == total_chunks);

        unsigned long long sealed_len = 0;
        if(crypto_aead_xchacha20poly1305_ietf_encrypt(
               sealed.data(), &sealed_len,
               block.data(), static_cast<unsigned long long>(read),
               ad.data(), ad.size(), nullptr,
               nonce.data(), dek.data()) != 0) {
            error = "Encryption failed.";
            return false;
        }

        out.write(reinterpret_cast<const char*>(sealed.data()), static_cast<std::streamsize>(sealed_len));
        if(!out) {
            error = "Cannot write " + cipher.string();
            return false;
        }
    }

    out.flush();
    return static_cast<bool>(out);
}

bool decrypt_file(const fs::path& cipher, const fs::path& plain, const Key& dek, std::string& error) {
    const uint64_t cipher_size = fsutils::get_file_size(cipher);
    if(cipher_size == fsutils::SIZE_ERROR || cipher_size == 0) {
        error = "Cannot read the downloaded file.";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(plain.parent_path(), ec);

    std::ifstream in(cipher, std::ios::binary);
    if(!in) {
        error = "Cannot open the downloaded file.";
        return false;
    }

    // Decryption fails one chunk at a time, and by then earlier chunks are already plaintext. If
    // that went straight to the destination, a tampered or truncated file would leave a partial
    // one sitting there looking like a successful download. It only appears once it is whole.
    fs::path scratch = plain;
    scratch += ".decrypting";

    std::ofstream out(scratch, std::ios::binary | std::ios::trunc);
    if(!out) {
        error = "Cannot write " + plain.string();
        return false;
    }

    // Every failure below leaves the destination untouched and takes the scratch file with it
    const auto fail = [&](const std::string& why) {
        error = why;
        out.close();
        std::error_code remove_ec;
        std::filesystem::remove(scratch, remove_ec);
        return false;
    };

    // Ciphertext chunks are exactly CHUNK_SIZE except the last, which is how the stored file lines
    // up with the transfer machinery's fixed stride in the first place.
    const uint64_t total_chunks = (cipher_size + fsutils::CHUNK_SIZE - 1) / fsutils::CHUNK_SIZE;

    std::vector<uint8_t> sealed(fsutils::CHUNK_SIZE);
    std::vector<uint8_t> block(PLAIN_CHUNK_SIZE);

    for(uint64_t index = 0; index < total_chunks; ++index) {
        in.read(reinterpret_cast<char*>(sealed.data()), static_cast<std::streamsize>(sealed.size()));
        const std::streamsize read = in.gcount();
        if(read <= static_cast<std::streamsize>(TAG_BYTES)) {
            return fail("The downloaded file is truncated.");
        }

        std::array<uint8_t, NONCE_BYTES> nonce{};
        chunk_nonce(dek, static_cast<uint32_t>(index), nonce);
        const std::array<uint8_t, 5> ad = chunk_ad(static_cast<uint32_t>(index), index + 1 == total_chunks);

        unsigned long long block_len = 0;
        if(crypto_aead_xchacha20poly1305_ietf_decrypt(
               block.data(), &block_len, nullptr,
               sealed.data(), static_cast<unsigned long long>(read),
               ad.data(), ad.size(),
               nonce.data(), dek.data()) != 0) {
            // Reached by a wrong key, a tampered chunk, reordering, and truncation alike - the
            // additional data folds all four into this one failure.
            return fail("The file did not decrypt: it was modified, truncated, or is not yours.");
        }

        out.write(reinterpret_cast<const char*>(block.data()), static_cast<std::streamsize>(block_len));
        if(!out) {
            return fail("Cannot write " + plain.string());
        }
    }

    out.flush();
    if(!out) {
        return fail("Cannot write " + plain.string());
    }
    out.close();

    std::filesystem::rename(scratch, plain, ec);
    if(ec) {
        std::error_code remove_ec;
        std::filesystem::remove(scratch, remove_ec);
        error = "Cannot write " + plain.string();
        return false;
    }
    return true;
}

#ifdef MINIDRIVE_ENABLE_TLS

namespace {

constexpr const char* MLKEM_ALG = "ML-KEM-768";

// Combines the two shared secrets into one wrapping key. Everything that identifies the exchange
// goes into the hash as well, so a secret can never be reinterpreted under a different transcript.
Key combine_secrets(const std::vector<uint8_t>& x25519_shared,
                    const std::vector<uint8_t>& kem_shared,
                    const std::vector<uint8_t>& eph_pub,
                    const std::vector<uint8_t>& device_x25519_pub,
                    const std::vector<uint8_t>& kem_ct) {
    static constexpr char CONTEXT[] = "minidrive vault device wrap v1";

    crypto_generichash_state state;
    crypto_generichash_init(&state, nullptr, 0, KEY_BYTES);
    crypto_generichash_update(&state, reinterpret_cast<const uint8_t*>(CONTEXT), sizeof(CONTEXT) - 1);
    crypto_generichash_update(&state, x25519_shared.data(), x25519_shared.size());
    crypto_generichash_update(&state, kem_shared.data(), kem_shared.size());
    crypto_generichash_update(&state, eph_pub.data(), eph_pub.size());
    crypto_generichash_update(&state, device_x25519_pub.data(), device_x25519_pub.size());
    crypto_generichash_update(&state, kem_ct.data(), kem_ct.size());

    Key key{};
    crypto_generichash_final(&state, key.data(), key.size());
    return key;
}

// ML-KEM keys travel as raw octet strings, which is what the server stores and what a public half
// has to be rebuilt from before anything can be encapsulated to it.
EVP_PKEY* import_mlkem(const std::vector<uint8_t>& raw, bool is_private) {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, MLKEM_ALG, nullptr);
    if(ctx == nullptr) return nullptr;

    EVP_PKEY* key = nullptr;
    OSSL_PARAM params[2];
    params[0] = OSSL_PARAM_construct_octet_string(
        is_private ? OSSL_PKEY_PARAM_PRIV_KEY : OSSL_PKEY_PARAM_PUB_KEY,
        const_cast<uint8_t*>(raw.data()), raw.size());
    params[1] = OSSL_PARAM_construct_end();

    if(EVP_PKEY_fromdata_init(ctx) <= 0 ||
       EVP_PKEY_fromdata(ctx, &key, is_private ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY, params) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return nullptr;
    }
    EVP_PKEY_CTX_free(ctx);
    return key;
}

} // namespace

bool mlkem_available() {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, MLKEM_ALG, nullptr);
    if(ctx == nullptr) return false;
    const bool ok = EVP_PKEY_keygen_init(ctx) > 0;
    EVP_PKEY_CTX_free(ctx);
    return ok;
}

bool generate_device_keys(const std::string& device_name, DeviceKeys& out, std::string& error) {
    if(!mlkem_available()) {
        // Enrolling with the classical half alone would quietly hand back a weaker guarantee than
        // the one advertised, so this refuses instead of degrading.
        error = "This build's OpenSSL has no ML-KEM-768, so a hybrid device key cannot be generated. "
                "OpenSSL 3.5 or newer is required for device enrollment.";
        return false;
    }

    std::array<uint8_t, crypto_box_PUBLICKEYBYTES> x_pub{};
    std::array<uint8_t, crypto_box_SECRETKEYBYTES> x_sec{};
    if(crypto_box_keypair(x_pub.data(), x_sec.data()) != 0) {
        error = "Failed to generate the classical device key.";
        return false;
    }

    EVP_PKEY* key = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, MLKEM_ALG, nullptr);
    if(ctx == nullptr || EVP_PKEY_keygen_init(ctx) <= 0 || EVP_PKEY_generate(ctx, &key) <= 0) {
        if(ctx != nullptr) EVP_PKEY_CTX_free(ctx);
        error = "Failed to generate the post-quantum device key.";
        return false;
    }
    EVP_PKEY_CTX_free(ctx);

    size_t pub_len = 0;
    size_t sec_len = 0;
    EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, nullptr, 0, &pub_len);
    EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PRIV_KEY, nullptr, 0, &sec_len);

    std::vector<uint8_t> mlkem_pub(pub_len);
    std::vector<uint8_t> mlkem_sec(sec_len);
    if(pub_len == 0 || sec_len == 0 ||
       EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, mlkem_pub.data(), mlkem_pub.size(), &pub_len) <= 0 ||
       EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PRIV_KEY, mlkem_sec.data(), mlkem_sec.size(), &sec_len) <= 0) {
        EVP_PKEY_free(key);
        error = "Failed to export the post-quantum device key.";
        return false;
    }
    EVP_PKEY_free(key);

    // 16 random bytes, hex: the id only has to be unique within one account's device table
    std::vector<uint8_t> id = random_bytes(16);

    out.device_id = mdcrypto::to_base64(id);
    out.device_name = device_name;
    std::memcpy(out.x25519_pub.data(), x_pub.data(), out.x25519_pub.size());
    std::memcpy(out.x25519_sec.data(), x_sec.data(), out.x25519_sec.size());
    out.mlkem_pub = std::move(mlkem_pub);
    out.mlkem_sec = std::move(mlkem_sec);

    sodium_memzero(x_sec.data(), x_sec.size());
    return true;
}

bool wrap_vk_to_device(const Key& vault_key, const std::vector<uint8_t>& x25519_pub,
                       const std::vector<uint8_t>& mlkem_pub, protocol::WrappedBlob& out,
                       std::string& error) {
    if(x25519_pub.size() != crypto_scalarmult_curve25519_BYTES) {
        error = "The device's classical public key has the wrong size.";
        return false;
    }

    // Classical half: an ephemeral X25519 key pair, so each wrap is independent of every other
    std::array<uint8_t, crypto_box_PUBLICKEYBYTES> eph_pub{};
    std::array<uint8_t, crypto_box_SECRETKEYBYTES> eph_sec{};
    if(crypto_box_keypair(eph_pub.data(), eph_sec.data()) != 0) {
        error = "Failed to generate an ephemeral key.";
        return false;
    }

    std::vector<uint8_t> x25519_shared(crypto_scalarmult_BYTES);
    if(crypto_scalarmult(x25519_shared.data(), eph_sec.data(), x25519_pub.data()) != 0) {
        sodium_memzero(eph_sec.data(), eph_sec.size());
        error = "The device's classical public key was rejected.";
        return false;
    }
    sodium_memzero(eph_sec.data(), eph_sec.size());

    // Post-quantum half
    EVP_PKEY* peer = import_mlkem(mlkem_pub, false);
    if(peer == nullptr) {
        error = "The device's post-quantum public key could not be read.";
        return false;
    }

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_pkey(nullptr, peer, nullptr);
    size_t ct_len = 0;
    size_t ss_len = 0;
    if(ctx == nullptr || EVP_PKEY_encapsulate_init(ctx, nullptr) <= 0 ||
       EVP_PKEY_encapsulate(ctx, nullptr, &ct_len, nullptr, &ss_len) <= 0) {
        if(ctx != nullptr) EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(peer);
        error = "Post-quantum encapsulation failed.";
        return false;
    }

    std::vector<uint8_t> kem_ct(ct_len);
    std::vector<uint8_t> kem_shared(ss_len);
    if(EVP_PKEY_encapsulate(ctx, kem_ct.data(), &ct_len, kem_shared.data(), &ss_len) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(peer);
        error = "Post-quantum encapsulation failed.";
        return false;
    }
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(peer);
    kem_ct.resize(ct_len);
    kem_shared.resize(ss_len);

    const std::vector<uint8_t> eph_pub_bytes(eph_pub.begin(), eph_pub.end());
    Key wrapping = combine_secrets(x25519_shared, kem_shared, eph_pub_bytes, x25519_pub, kem_ct);

    sodium_memzero(x25519_shared.data(), x25519_shared.size());
    sodium_memzero(kem_shared.data(), kem_shared.size());

    const bool ok = wrap_key(vault_key, wrapping, out);
    wipe(wrapping);

    if(!ok) {
        error = "Failed to seal the vault key to the device.";
        return false;
    }

    out.kem_ct = mdcrypto::to_base64(kem_ct);
    out.eph_pub = mdcrypto::to_base64(eph_pub_bytes);
    return true;
}

bool unwrap_vk_with_device(const protocol::WrappedBlob& blob, const DeviceKeys& keys, Key& out,
                           std::string& error) {
    std::vector<uint8_t> kem_ct;
    std::vector<uint8_t> eph_pub;
    if(!mdcrypto::from_base64(blob.kem_ct, kem_ct) || !mdcrypto::from_base64(blob.eph_pub, eph_pub)) {
        error = "The stored device blob is malformed.";
        return false;
    }
    if(eph_pub.size() != crypto_scalarmult_curve25519_BYTES) {
        error = "The stored device blob is malformed.";
        return false;
    }

    std::vector<uint8_t> x25519_shared(crypto_scalarmult_BYTES);
    if(crypto_scalarmult(x25519_shared.data(), keys.x25519_sec.data(), eph_pub.data()) != 0) {
        error = "The classical half of the device unwrap failed.";
        return false;
    }

    EVP_PKEY* own = import_mlkem(keys.mlkem_sec, true);
    if(own == nullptr) {
        error = "This device's post-quantum key could not be loaded.";
        return false;
    }

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_pkey(nullptr, own, nullptr);
    size_t ss_len = 0;
    if(ctx == nullptr || EVP_PKEY_decapsulate_init(ctx, nullptr) <= 0 ||
       EVP_PKEY_decapsulate(ctx, nullptr, &ss_len, kem_ct.data(), kem_ct.size()) <= 0) {
        if(ctx != nullptr) EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(own);
        error = "Post-quantum decapsulation failed.";
        return false;
    }

    std::vector<uint8_t> kem_shared(ss_len);
    if(EVP_PKEY_decapsulate(ctx, kem_shared.data(), &ss_len, kem_ct.data(), kem_ct.size()) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(own);
        error = "Post-quantum decapsulation failed.";
        return false;
    }
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(own);
    kem_shared.resize(ss_len);

    const std::vector<uint8_t> device_pub(keys.x25519_pub.begin(), keys.x25519_pub.end());
    Key wrapping = combine_secrets(x25519_shared, kem_shared, eph_pub, device_pub, kem_ct);

    sodium_memzero(x25519_shared.data(), x25519_shared.size());
    sodium_memzero(kem_shared.data(), kem_shared.size());

    const bool ok = unwrap_key(blob, wrapping, out);
    wipe(wrapping);

    if(!ok) {
        error = "The vault key did not open with this device's keys.";
        return false;
    }
    return true;
}

#else // MINIDRIVE_ENABLE_TLS

// Without OpenSSL there is no ML-KEM, so device enrollment is simply unavailable. The password
// path is untouched: a vault still works, it just cannot take shortcuts past Argon2id.
bool mlkem_available() { return false; }

bool generate_device_keys(const std::string&, DeviceKeys&, std::string& error) {
    error = "This build has no OpenSSL, so device enrollment is unavailable.";
    return false;
}

bool wrap_vk_to_device(const Key&, const std::vector<uint8_t>&, const std::vector<uint8_t>&,
                       protocol::WrappedBlob&, std::string& error) {
    error = "This build has no OpenSSL, so device enrollment is unavailable.";
    return false;
}

bool unwrap_vk_with_device(const protocol::WrappedBlob&, const DeviceKeys&, Key&, std::string& error) {
    error = "This build has no OpenSSL, so device unlock is unavailable.";
    return false;
}

#endif // MINIDRIVE_ENABLE_TLS

namespace {

json read_device_file(const fs::path& file) {
    std::ifstream f(file);
    if(!f) return json::object();
    if(f.peek() == std::ifstream::traits_type::eof()) return json::object();

    json j;
    try {
        f >> j;
    } catch(const json::exception&) {
        return json::object();
    }
    if(!j.is_object()) return json::object();
    return j;
}

bool write_device_file(const fs::path& file, const json& j) {
    // Private keys sit here in the clear, so at least keep them off other accounts on this machine.
    // owner_only applies 0600 to the temp file before the keys are written, not after the rename.
    return fsutils::atomic_write_file(file, j.dump(4), /*owner_only=*/true);
}

} // namespace

bool save_device_keys(const fs::path& file, const std::string& account, const DeviceKeys& keys) {
    json j = read_device_file(file);
    j[account] = {
        {"device_id", keys.device_id},
        {"device_name", keys.device_name},
        {"x25519_pub", mdcrypto::to_base64(keys.x25519_pub.data(), keys.x25519_pub.size())},
        {"x25519_sec", mdcrypto::to_base64(keys.x25519_sec.data(), keys.x25519_sec.size())},
        {"mlkem_pub", mdcrypto::to_base64(keys.mlkem_pub)},
        {"mlkem_sec", mdcrypto::to_base64(keys.mlkem_sec)}
    };
    return write_device_file(file, j);
}

std::optional<DeviceKeys> load_device_keys(const fs::path& file, const std::string& account) {
    json j = read_device_file(file);
    if(!j.contains(account)) return std::nullopt;

    const json& item = j.at(account);
    DeviceKeys keys;
    keys.device_id = item.value("device_id", std::string());
    keys.device_name = item.value("device_name", std::string());

    std::vector<uint8_t> x_pub;
    std::vector<uint8_t> x_sec;
    if(!blob_field(item.value("x25519_pub", std::string()), x_pub).empty()) return std::nullopt;
    if(!blob_field(item.value("x25519_sec", std::string()), x_sec).empty()) return std::nullopt;
    if(x_pub.size() != keys.x25519_pub.size() || x_sec.size() != keys.x25519_sec.size()) return std::nullopt;
    std::memcpy(keys.x25519_pub.data(), x_pub.data(), x_pub.size());
    std::memcpy(keys.x25519_sec.data(), x_sec.data(), x_sec.size());

    if(!mdcrypto::from_base64(item.value("mlkem_pub", std::string()), keys.mlkem_pub)) return std::nullopt;
    if(!mdcrypto::from_base64(item.value("mlkem_sec", std::string()), keys.mlkem_sec)) return std::nullopt;
    if(keys.device_id.empty() || keys.mlkem_sec.empty()) return std::nullopt;

    return keys;
}

void forget_device_keys(const fs::path& file, const std::string& account) {
    json j = read_device_file(file);
    if(j.erase(account) == 0) return;
    write_device_file(file, j);
}

} // namespace vault
