#include "password.hpp"
#include <sodium.h>
#include <stdexcept>
#include <cstring>

namespace password {
    std::string hash_password(const std::string& password) {
        std::string hash;
        hash.resize(crypto_pwhash_STRBYTES);

        if (crypto_pwhash_str(hash.data(), password.c_str(), password.size(),
                              crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
            throw std::runtime_error("Out of memory while hashing password");
        }
        hash.resize(std::strlen(hash.c_str()));

        return hash;
    }

    bool verify_password(const std::string& password, const std::string& hash) {
        return crypto_pwhash_str_verify(hash.c_str(), password.c_str(), password.size()) == 0;
    }
}