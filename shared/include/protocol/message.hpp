#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <cstdint>

namespace protocol {

using json = nlohmann::json;

struct Request {
    std::string cmd;
    std::string first_argument;
    std::string second_argument;
    uint32_t size;
    std::string hash;
};

struct Response {
    std::string status;
    uint16_t code;
    std::string message;
    std::string hash;
};

void to_json(json& json, const Request& req);
void to_json(json& json, const Response& res);
void from_json(const json& json, Request& req);
void from_json(const json& json, Response& res);
}