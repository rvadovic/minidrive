#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace password {

    // Generates salt, hash the password, outputs string
    std::string hash_password(const std::string& password);
    bool verify_password(const std::string& password, const std::string& hash);
}