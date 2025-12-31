#pragma once

namespace protocol::codes {
    
inline constexpr const int OK = 200;
inline constexpr const int ACCEPTED = 202;

inline constexpr const int BAD_REQUEST = 400;
inline constexpr const int UNAUTHORIZED = 401;
inline constexpr const int FORBIDDEN = 403;
inline constexpr const int NOT_FOUND = 404;
inline constexpr const int CONFLICT = 409;
inline constexpr const int PRECONDITION_FAILED = 412;

inline constexpr const int INTERNAL_SERVER_ERROR = 500;
inline constexpr const int SERVICE_UNAVAILABLE = 503;

}