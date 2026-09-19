#include "crypto/base64.hpp"

#include <sodium.h>

namespace mdcrypto {

namespace {
constexpr int VARIANT = sodium_base64_VARIANT_ORIGINAL;
}

std::string to_base64(const uint8_t* data, size_t size) {
    if(size == 0) return std::string();

    const size_t encoded_len = sodium_base64_encoded_len(size, VARIANT); // includes the NUL byte
    std::string out(encoded_len, '\0');
    sodium_bin2base64(out.data(), out.size(), data, size, VARIANT);
    out.pop_back(); // sodium_bin2base64 NUL-terminates, std::string tracks its own length
    return out;
}

std::string to_base64(const std::vector<uint8_t>& data) {
    return to_base64(data.data(), data.size());
}

bool from_base64(const std::string& text, std::vector<uint8_t>& out) {
    out.clear();
    if(text.empty()) return true;

    // 3 bytes of input per 4 characters, rounded up - never an underestimate
    out.resize((text.size() / 4 + 1) * 3);

    size_t decoded_len = 0;
    if(sodium_base642bin(out.data(), out.size(), text.data(), text.size(),
                         nullptr, &decoded_len, nullptr, VARIANT) != 0) {
        out.clear();
        return false;
    }
    out.resize(decoded_len);
    return true;
}

} // namespace mdcrypto
