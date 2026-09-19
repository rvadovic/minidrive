#pragma once

namespace protocol::commands {

inline constexpr const char* LOGIN = "LOGIN";
inline constexpr const char* UPLOAD = "UPLOAD";
inline constexpr const char* DOWNLOAD = "DOWNLOAD";
inline constexpr const char* LIST = "LIST";
inline constexpr const char* EXIT = "EXIT";
inline constexpr const char* DELETE = "DELETE";
inline constexpr const char* CD = "CD";
inline constexpr const char* MKDIR = "MKDIR";
inline constexpr const char* RMDIR = "RMDIR";
inline constexpr const char* MOVE = "MOVE";
inline constexpr const char* COPY = "COPY";
inline constexpr const char* SYNC = "SYNC";
inline constexpr const char* NEED_INPUT = "NEED_INPUT";
inline constexpr const char* AUTH = "AUTH";
inline constexpr const char* TIERS = "TIERS"; // List storage media configured on the server
inline constexpr const char* SET_TIER = "SET_TIER"; // Move the calling user to another medium
inline constexpr const char* VAULT_INIT = "VAULT_INIT"; // Turn on end-to-end encryption for this account
inline constexpr const char* ENROLL_DEVICE = "ENROLL_DEVICE"; // Add a device that can unlock the vault
inline constexpr const char* REVOKE_DEVICE = "REVOKE_DEVICE"; // Remove one, blocking future device unlocks
inline constexpr const char* DEVICES = "DEVICES"; // List the devices enrolled in the vault

}