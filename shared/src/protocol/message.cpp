#include "protocol/message.hpp"
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace protocol {

void to_json(json& j, const ChunkInfo& ci) {
    j = {
        {"index", ci.index},
        {"size", ci.size},
        {"chunk_hash", ci.chunk_hash}
    };
}

void from_json(const json& j, ChunkInfo& ci) {
    ci = {
        j.at("index").get<uint32_t>(),
        j.at("size").get<uint32_t>(),
        j.at("chunk_hash").get<std::string>()
    };
}

void to_json(json& j, const FileEntry& fe) {
    j = {
        {"relative_path", fe.relative_path},
        {"size", fe.size},
        {"file_hash", fe.file_hash},
        {"last_modified", fe.last_modified},
        {"is_directory", fe.is_directory}
    };
}

void from_json(const json& j, FileEntry& fe) {
    fe = {
        j.at("relative_path").get<std::string>(),
        j.at("size").get<uint32_t>(),
        j.at("file_hash").get<std::string>(),
        j.at("last_modified").get<uint64_t>(),
        j.at("is_directory").get<bool>()
    };
}

void to_json(json& j, const Request& req) {
    j = {
        {"cmd", req.cmd},
        {"first_argument", req.first_argument},
        {"second_argument", req.second_argument},
        {"size", req.size},
        {"file_hash", req.file_hash},
    };

    if(!req.chunks.empty()) {
        j["chunks"] = req.chunks;
    }
}

void to_json(json& j, const Response& res) {
    j = {
        {"status", res.status},
        {"code", res.code},
        {"message", res.message},
        {"file_hash", res.file_hash}
    };

    if(!res.chunks.empty()) {
        j["chunks"] = res.chunks;
    }

    if(!res.files.empty()) {
        j["files"] = res.files;
    }
}

void from_json(const json& j, Request& req) {
    req.cmd = j.at("cmd").get<std::string>();
    req.first_argument = j.at("first_argument").get<std::string>();
    req.second_argument = j.at("second_argument").get<std::string>();
    req.size = j.at("size").get<uint32_t>();
    req.file_hash = j.at("file_hash").get<std::string>();

    if(j.contains("chunks")) {
        req.chunks = j.at("chunks").get<std::vector<ChunkInfo>>();
    }

}

void from_json(const json& j, Response& res) {
    res.status = j.at("status").get<std::string>();
    res.code = j.at("code").get<uint16_t>();
    res.message = j.at("message").get<std::string>();
    res.file_hash = j.at("file_hash").get<std::string>();

    if(j.contains("chunks")) {
        res.chunks = j.at("chunks").get<std::vector<ChunkInfo>>();
    }

    if(j.contains("files")) {
        res.files = j.at("files").get<std::vector<FileEntry>>();
    }
}

}