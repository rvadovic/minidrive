#include "password.hpp"
#include <sodium.h>
#include <stdexcept>
#include <cstring>
#include <array>

namespace password {
    std::string hash_password(const std::string& password) {
        std::array<char, crypto_pwhash_STRBYTES> hash;

        // Already includes salt, no need for manual salt handling
        if (crypto_pwhash_str(hash.data(), password.c_str(), password.size(),
                              crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) { // Modes boosting security
            throw std::runtime_error("Out of memory while hashing password");
        }

        return std::string(hash.data());
    }

    // Extracts salt from stored string
    bool verify_password(const std::string& password, const std::string& hash) {
        return crypto_pwhash_str_verify(hash.c_str(), password.c_str(), password.size()) == 0;
    }
}