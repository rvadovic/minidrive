#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Base64 helpers used by the vault's wire format. Every vault blob (salts, nonces, wrapped keys,
// KEM ciphertexts) is binary, and the control channel is JSON, so it has to be text somewhere.
// Backed by libsodium's constant-time sodium_bin2base64/base642bin rather than a hand-rolled table.
namespace mdcrypto {

std::string to_base64(const std::vector<uint8_t>& data);
std::string to_base64(const uint8_t* data, size_t size);

// Returns false on anything that is not valid standard base64, so a malformed blob from a peer is
// a rejected message rather than silently truncated key material.
bool from_base64(const std::string& text, std::vector<uint8_t>& out);

} // namespace mdcrypto
