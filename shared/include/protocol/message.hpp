#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <cstdint>
#include <sodium.h>

namespace protocol {

using json = nlohmann::json;

// Info about chunk for a file
struct ChunkInfo {
    uint32_t index; // chunk index
    u_int32_t size; // size of chunk
    std::string chunk_hash; //hash_to_hex value
};

// One entry of a recursive directory listing (SYNC / directory transfers)
struct FileEntry {
    std::string relative_path; // Path relative to the listed directory, '/' separated
    uint32_t size; // Size of file, 0 for directories
    std::string file_hash; // hash_to_hex value, empty for directories
    uint64_t last_modified; // Seconds since epoch
    bool is_directory;
};

// Client request JSON protocol
struct Request {
    std::string cmd; // command
    std::string first_argument;
    std::string second_argument;
    uint32_t size;
    std::string file_hash;
    std::vector<ChunkInfo> chunks;
};

// Server rsponse JSON protocol
struct Response {
    std::string status; // server status
    uint16_t code; // code from protocol/codes.hpp
    std::string message;
    std::string file_hash;
    std::vector<ChunkInfo> chunks;
    std::vector<FileEntry> files; // Recursive listing, only used by SYNC responses
};

// Binary protocol for file transfers
#pragma pack(push, 1)
struct ChunkHeader {
    uint32_t transfer_id;
    uint32_t index; // chunk index
    uint32_t size; // chunk size
    uint8_t flags; // defined in protocol/flags.hpp
};
#pragma pack(pop)

// Parsing
void to_json(json& json, const ChunkInfo& ci);
void from_json(const json& json, ChunkInfo& ci);
void to_json(json& json, const FileEntry& fe);
void from_json(const json& json, FileEntry& fe);
void to_json(json& json, const Request& req);
void to_json(json& json, const Response& res);
void from_json(const json& json, Request& req);
void from_json(const json& json, Response& res);
}