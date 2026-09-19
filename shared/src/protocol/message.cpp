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

void to_json(json& j, const TierInfo& ti) {
    j = {
        {"name", ti.name},
        {"description", ti.description},
        {"is_current", ti.is_current}
    };
}

void from_json(const json& j, TierInfo& ti) {
    ti = {
        j.at("name").get<std::string>(),
        j.at("description").get<std::string>(),
        j.at("is_current").get<bool>()
    };
}

// Optional sub-objects are omitted entirely when empty rather than sent as nulls, matching the
// existing chunks/files/tiers convention: a peer that predates the vault sees the same messages.
void to_json(json& j, const WrappedBlob& wb) {
    j = json::object();
    j["ciphertext"] = wb.ciphertext;
    j["nonce"] = wb.nonce;
    if(!wb.kem_ct.empty()) j["kem_ct"] = wb.kem_ct;
    if(!wb.eph_pub.empty()) j["eph_pub"] = wb.eph_pub;
}

void from_json(const json& j, WrappedBlob& wb) {
    wb.ciphertext = j.value("ciphertext", std::string());
    wb.nonce = j.value("nonce", std::string());
    wb.kem_ct = j.value("kem_ct", std::string());
    wb.eph_pub = j.value("eph_pub", std::string());
}

void to_json(json& j, const DeviceInfo& di) {
    j = {
        {"device_id", di.device_id},
        {"device_name", di.device_name},
        {"algorithm", di.algorithm},
        {"x25519_pub", di.x25519_pub},
        {"mlkem_pub", di.mlkem_pub},
        {"created_at", di.created_at},
        {"last_seen", di.last_seen}
    };
    if(!di.wrapped_vk.empty()) j["wrapped_vk"] = di.wrapped_vk;
}

void from_json(const json& j, DeviceInfo& di) {
    di.device_id = j.value("device_id", std::string());
    di.device_name = j.value("device_name", std::string());
    di.algorithm = j.value("algorithm", std::string());
    di.x25519_pub = j.value("x25519_pub", std::string());
    di.mlkem_pub = j.value("mlkem_pub", std::string());
    di.created_at = j.value("created_at", uint64_t{0});
    di.last_seen = j.value("last_seen", uint64_t{0});
    if(j.contains("wrapped_vk")) from_json(j.at("wrapped_vk"), di.wrapped_vk);
}

void to_json(json& j, const VaultInfo& vi) {
    j = {
        {"enabled", vi.enabled},
        {"salt", vi.salt},
        {"opslimit", vi.opslimit},
        {"memlimit", vi.memlimit},
        {"algorithm", vi.algorithm}
    };
    if(!vi.wrapped_vk_password.empty()) j["wrapped_vk_password"] = vi.wrapped_vk_password;
}

void from_json(const json& j, VaultInfo& vi) {
    vi.enabled = j.value("enabled", false);
    vi.salt = j.value("salt", std::string());
    vi.opslimit = j.value("opslimit", uint64_t{0});
    vi.memlimit = j.value("memlimit", uint64_t{0});
    vi.algorithm = j.value("algorithm", 0);
    if(j.contains("wrapped_vk_password")) from_json(j.at("wrapped_vk_password"), vi.wrapped_vk_password);
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

    if(req.vault.enabled) {
        j["vault"] = req.vault;
    }

    if(!req.device.x25519_pub.empty()) {
        j["device"] = req.device;
    }

    if(!req.wrapped_dek.empty()) {
        j["wrapped_dek"] = req.wrapped_dek;
        j["plaintext_hash"] = req.plaintext_hash;
        j["plaintext_size"] = req.plaintext_size;
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

    if(!res.tiers.empty()) {
        j["tiers"] = res.tiers;
    }

    if(res.vault.enabled) {
        j["vault"] = res.vault;
    }

    if(!res.devices.empty()) {
        j["devices"] = res.devices;
    }

    if(!res.wrapped_dek.empty()) {
        j["wrapped_dek"] = res.wrapped_dek;
        j["plaintext_hash"] = res.plaintext_hash;
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

    if(j.contains("vault")) {
        from_json(j.at("vault"), req.vault);
    }

    if(j.contains("device")) {
        from_json(j.at("device"), req.device);
    }

    if(j.contains("wrapped_dek")) {
        from_json(j.at("wrapped_dek"), req.wrapped_dek);
        req.plaintext_hash = j.value("plaintext_hash", std::string());
        req.plaintext_size = j.value("plaintext_size", uint32_t{0});
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

    if(j.contains("tiers")) {
        res.tiers = j.at("tiers").get<std::vector<TierInfo>>();
    }

    if(j.contains("vault")) {
        from_json(j.at("vault"), res.vault);
    }

    if(j.contains("devices")) {
        res.devices = j.at("devices").get<std::vector<DeviceInfo>>();
    }

    if(j.contains("wrapped_dek")) {
        from_json(j.at("wrapped_dek"), res.wrapped_dek);
        res.plaintext_hash = j.value("plaintext_hash", std::string());
    }
}

}