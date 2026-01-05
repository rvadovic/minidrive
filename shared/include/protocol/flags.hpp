#pragma once

#include<cstdint>

namespace protocol::flags {
    inline constexpr const uint8_t OK = 0;
    inline constexpr const uint8_t ERROR = 1;
    inline constexpr const uint8_t CHUNK_MISMATCH = 2;
    inline constexpr const uint8_t LAST = 3;
    inline constexpr const uint8_t SEND = 4;
    inline constexpr const uint8_t DONE = 5;
    inline constexpr const uint8_t EXIT = 6;
}