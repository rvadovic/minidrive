#include "protocol/message.hpp"
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace protocol {

void to_json(json& j, const Request& req) {
    j = {
        {"cmd", req.cmd},
        {"first_argument", req.first_argument},
        {"second_argument", req.second_argument},
        {"size", req.size},
        {"hash", req.hash}
    };
}

void to_json(json& j, const Response& res) {
    j = {
        {"status", res.status},
        {"code", res.code},
        {"message", res.message},
        {"hash", res.hash}
    };
}

void from_json(const json& j, Request& req) {
    req = {
        j.at("cmd").get<std::string>(),
        j.at("first_argument").get<std::string>(),
        j.at("second_argument").get<std::string>(),
        j.at("size").get<uint32_t>(),
        j.at("hash").get<std::string>(),
    };

}

void from_json(const json& j, Response& res) {
    res = {
        j.at("status").get<std::string>(),
        j.at("code").get<uint16_t>(),
        j.at("message").get<std::string>(),
        j.at("hash").get<std::string>(),
    };
}

}