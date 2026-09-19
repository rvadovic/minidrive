#include <asio.hpp>
#include <memory>
#include <vector>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include "session.hpp"
#include "protocol/message.hpp"
#include "protocol/commands.hpp"
#include "protocol/statuses.hpp"
#include "protocol/codes.hpp"
#include "protocol/flags.hpp"
#include "filesystem/utils.hpp"

using asio::ip::tcp;
using nlohmann::json;

namespace {
// Identifies a session to Storage's per-user lock. Starts at 1 because 0 marks the lock as free.
std::atomic<uint64_t> next_session_id{1};
} // namespace

Session::Session(std::shared_ptr<transport::IStream> stream, std::shared_ptr<Storage> storage, std::function<void(std::shared_ptr<Session>)> on_exit)
    // Order matches the declaration order in session.hpp; members are initialized in that order
    // regardless, and listing them out of order only produced a -Wreorder warning.
    : stream_(std::move(stream)),
      strand_(stream_->get_executor()),
      root_(storage->get_root()),
      storage_(storage),
      on_exit_(on_exit),
      db_(storage_->get_database()),
      requests_{
          {protocol::commands::LIST,     [this](auto& req){ list(req); }},
          {protocol::commands::UPLOAD,   [this](auto& req){ upload(req); }},
          {protocol::commands::DOWNLOAD, [this](auto& req){ download(req); }},
          {protocol::commands::DELETE,   [this](auto& req){ delete_file(req); }},
          {protocol::commands::CD,       [this](auto& req){ cd(req); }},
          {protocol::commands::MKDIR,    [this](auto& req){ mkdir(req); }},
          {protocol::commands::RMDIR,    [this](auto& req){ rmdir(req); }},
          {protocol::commands::MOVE,     [this](auto& req){ move(req); }},
          {protocol::commands::COPY,     [this](auto& req){ copy(req); }},
          {protocol::commands::SYNC,     [this](auto& req){ sync(req); }},
          {protocol::commands::TIERS,    [this](auto& req){ tiers(req); }},
          {protocol::commands::SET_TIER, [this](auto& req){ set_tier(req); }},
          {protocol::commands::VAULT_INIT,    [this](auto& req){ vault_init(req); }},
          {protocol::commands::ENROLL_DEVICE, [this](auto& req){ enroll_device(req); }},
          {protocol::commands::REVOKE_DEVICE, [this](auto& req){ revoke_device(req); }},
          {protocol::commands::DEVICES,       [this](auto& req){ devices(req); }},
          {protocol::commands::LOGIN,     [this](auto& req){ login(req); }},
          {protocol::commands::NEED_INPUT, [this](auto& req){ need_input(req); }},
          {protocol::commands::AUTH, [this](auto& req){ auth(req); }}
      },
      session_id_(next_session_id.fetch_add(1)) {
        transfer_.transfer_id = UINT32_MAX;
}

UserLock Session::acquire_lock() {
    return UserLock(storage_, username_, session_id_);
}

void Session::start() {
    auto self = shared_from_this();
    // Plain TCP reports success immediately; TLS completes its handshake here before any framing.
    stream_->async_server_handshake([this, self](const std::error_code& ec) {
        if(exiting_) return;
        if(ec) {
            // At rung 3.5 this is where a client with the wrong CA, a stale pin or no TLS at all
            // ends up. It is a per-connection failure, not a server fault, so it is logged and the
            // session is dropped rather than escalated.
            spdlog::warn("Handshake failed: {}", ec.message());
            handle_error(ec);
            return;
        }
        spdlog::info("Connection established: {}", stream_->describe_connection());
        read_header_json();
    });
}

void Session::read_header_json() {
    auto self = shared_from_this();
    auto msg_len_local = std::make_shared<uint32_t>();

    stream_->async_read_exact(msg_len_local.get(), sizeof(uint32_t), [this, self, msg_len_local](const std::error_code& ec, std::size_t) {
        if(exiting_ || ec) {
            if(ec && ec != asio::error::operation_aborted) {
                handle_error(ec);
            }
            return;
        }
        msg_len_ = ntohl(*msg_len_local);
        // The length is peer-controlled: without this bound, four bytes buy a 4 GiB allocation
        // before a single byte of body has been read or authenticated.
        if(msg_len_ == 0 || msg_len_ > transport::MAX_MESSAGE_SIZE) {
            spdlog::warn("[{}] Rejected control message of {} bytes (limit {})", username_, msg_len_, transport::MAX_MESSAGE_SIZE);
            handle_error(asio::error::message_size);
            return;
        }
        buffer_.resize(msg_len_);
        read_body_json();

    });
}

void Session::read_body_json() {
    auto self = shared_from_this();
    if(msg_len_ == 0) {
        handle_error(asio::error::invalid_argument);
        return;
    }
    stream_->async_read_exact(buffer_.data(), buffer_.size(), [this, self](const std::error_code& ec, std::size_t) {
        if(exiting_ || ec) {
            if(ec && ec != asio::error::operation_aborted) {
                handle_error(ec);
            }
            return;
        }
        std::string msg(buffer_.begin(), buffer_.end());
        json j;
        try {
            j = json::parse(msg);
        } catch (json::parse_error& e) {
            handle_error(asio::error::invalid_argument);
            return;
        }
        // The one funnel every command passes through. UserLock releases on unwind, which is what
        // makes catching here safe: a caught exception cannot strand the user in permanent 503.
        responded_ = false;
        SessionState state_before = state_;
        try {
            handle_request(j);
        } catch(const std::exception& e) {
            recover_from_exception("request", e, state_before);
        }
        if(exiting_) return;
        read_next();
    });
}

void Session::write_response_json(const json& j) {
    auto self = shared_from_this();

    const std::string body = j.dump();

    if(body.size() > std::numeric_limits<uint32_t>::max()) {
        spdlog::error("[{}] Response body too large to send ({} bytes)", username_, body.size());
        return;
    }

    responded_ = true;
    stream_->async_write_all(transport::frame_json(body), [this, self](const std::error_code& ec, std::size_t) {
        if(ec) {
            handle_error(ec);
            return;
        }
    });
}

void Session::write_response_json_exit(const json& j) {
    auto self = shared_from_this();

    const std::string body = j.dump();

    if(body.size() > std::numeric_limits<uint32_t>::max()) {
        spdlog::error("[{}] Response body too large to send ({} bytes)", username_, body.size());
        finish_exit();
        return;
    }

    stream_->async_write_all(transport::frame_json(body), [this, self](const std::error_code&, std::size_t) {
        finish_exit();
    });
}

void Session::read_header_chunk() {
    auto self = shared_from_this();

    auto header = std::make_shared<protocol::ChunkHeader>();

    stream_->async_read_exact(header.get(), sizeof(protocol::ChunkHeader), [this, self, header](const std::error_code& ec, std::size_t) {
        if(exiting_ || ec) {
            if(ec && ec != asio::error::operation_aborted) {
                handle_error(ec);
            }
            return;
        }
        ch_.transfer_id = ntohl(header->transfer_id);
        ch_.index = ntohl(header->index);
        ch_.size = ntohl(header->size);
        ch_.flags = header->flags;
        // Same peer-controlled-length problem as the JSON header above, and the protocol never
        // produces a chunk larger than one CHUNK_SIZE.
        if(ch_.size > transport::MAX_CHUNK_PAYLOAD) {
            spdlog::warn("[{}] Rejected chunk of {} bytes (limit {})", username_, ch_.size, transport::MAX_CHUNK_PAYLOAD);
            handle_error(asio::error::message_size);
            return;
        }
        read_body_chunk();
    });
}

void Session::read_body_chunk() {
    auto self = shared_from_this();

    auto data = std::make_shared<std::vector<uint8_t>>(ch_.size);

    stream_->async_read_exact(data->data(), data->size(), [this, self, data](const std::error_code& ec, std::size_t) {
        if(exiting_ || ec) {
            if(ec && ec != asio::error::operation_aborted) {
                handle_error(ec);
            }
            return;
        }
        try {
            handle_chunk(ch_, *data);
        } catch(const std::exception& e) {
            // Mid-transfer there is no request to answer with a JSON 500 - the peer is speaking
            // chunk framing - so end this session; do_exit() keeps the transfer resumable.
            recover_from_exception("chunk", e, SessionState::EXIT);
        }
        if(exiting_) return;
        read_next();
    });
}

void Session::send_chunk(const protocol::ChunkHeader& ch, const std::vector<uint8_t>& data) {
    auto self = shared_from_this();

    stream_->async_write_all(transport::frame_chunk(ch, data), [this, self](const std::error_code& ec, std::size_t) {
        if(ec) {
            handle_error(ec);
            return;
        }
    });
}

void Session::send_chunk_exit(const protocol::ChunkHeader& ch, const std::vector<uint8_t>& data) {
    auto self = shared_from_this();

    stream_->async_write_all(transport::frame_chunk(ch, data), [this, self](const std::error_code&, std::size_t) {
        finish_exit();
    });
}

void Session::send_res(protocol::Response& res) {
    spdlog::debug("[{}] -> {} {} {}", username_, res.status, res.code, res.message);
    res.chunks.clear();

    // auth() sets this flag rather than sending a second message, because a back-to-back response
    // races against whatever the client already has buffered on stdin (bugs #1 and #7). Whichever
    // response the successful login actually produces is the one that carries the vault material.
    if(send_vault_info_) {
        send_vault_info_ = false;
        if(vault_ && vault_->is_initialized()) {
            res.vault = vault_->get_info();
            for(const auto& device : vault_->get_devices()) {
                res.devices.push_back(protocol::DeviceInfo{
                    device.device_id, device.device_name, device.algorithm,
                    device.x25519_pub, device.mlkem_pub, device.wrapped_vk,
                    device.created_at, device.last_seen
                });
            }
        }
    }

    json j;
    protocol::to_json(j, res);
    write_response_json(j);
}

std::string Session::vault_key_for(const std::filesystem::path& absolute_path) const {
    return fsutils::relative(user_dir_, absolute_path).generic_string();
}

void Session::read_next() {
    if(exiting_) return;
    if(state_ == SessionState::UPLOADING || state_ == SessionState::DOWNLOADING) {
        read_header_chunk();
    } else {
        read_header_json();
    }
}

void Session::recover_from_exception(const char* where, const std::exception& e, SessionState state_before) {
    spdlog::critical("[{}] Unexpected exception in {} handler: {}", username_.empty() ? "-" : username_, where, e.what());

    if(!responded_ && state_ == state_before && !exiting_) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::INTERNAL_SERVER_ERROR,
            "Internal server error.",
            ""
        };
        send_res(res);
        return;
    }
    // Either the peer already has its answer or the state machine moved, so the session's state
    // can no longer be trusted. Ending it is safe; every other session is unaffected.
    exit();
}

bool Session::require_database() {
    if(db_->is_healthy()) return true;
    refuse_database();
    return false;
}

void Session::refuse_database() {
    spdlog::error("[{}] Refusing login: account database is unavailable.", username_.empty() ? "-" : username_);
    // One response, then close: leaving the session in LOGIN would only have every following
    // command refused, and a separate goodbye would be a second response to this request.
    protocol::Response res {
        protocol::statuses::ERROR,
        protocol::codes::INTERNAL_SERVER_ERROR,
        "Account database is unavailable.",
        ""
    };
    json j;
    protocol::to_json(j, res);
    exiting_ = true;
    write_response_json_exit(j);
}

void Session::handle_error(const std::error_code& ec) {
    spdlog::warn("[{}] Network error: {} ({})", username_, ec.message(), ec.value());
    exit();
}

void Session::handle_request(const json& j) {
    protocol::Request req;
    protocol::from_json(j, req);
    spdlog::info("[{}] <- {}", username_.empty() ? "-" : username_, req.cmd);
    if(req.cmd == protocol::commands::EXIT) {
        exit();
        return;
    }
    auto it = requests_.find(req.cmd);
    if (it != requests_.end()) {
        it->second(req);
    } else {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::BAD_REQUEST,
            "Unknown request",
            "",
        };
        send_res(res);
    }
}

void Session::handle_chunk(const protocol::ChunkHeader& ch, const std::vector<uint8_t>& data) {
    if(state_ == SessionState::UPLOADING) {
        // The id belongs to the server (allocated in upload(), or carried by the resume kickoff).
        // It used to be adopted from this header, and since every fresh client numbers its transfers
        // from 1, two clients uploading as the same user both claimed id 1 and wrote into the same
        // <user>/.partial/1.part. Data-carrying chunks for another id are not part of this transfer.
        // Abort headers are exempt: the client clears its own id before sending them.
        if((ch.flags == protocol::flags::SEND || ch.flags == protocol::flags::LAST)
           && ch.transfer_id != transfer_.transfer_id) {
            spdlog::warn("[{}] Chunk for transfer {} received during transfer {}.", username_, ch.transfer_id, transfer_.transfer_id);
            upload_abort(false, true, protocol::flags::CHUNK_MISMATCH);
            return;
        }
        if(ch.flags == protocol::flags::SEND) {
            if(valid_chunk(ch.index, ch.size, data)) {
                uploading(ch.index, ch.size, data, protocol::flags::OK);
                return;
            }
            spdlog::warn("[{}] Invalid chunk received (index {}).", username_, ch.index);
            upload_abort(false, true, protocol::flags::CHUNK_MISMATCH);
            return;
        } else if(ch.flags == protocol::flags::LAST) {
            if(valid_chunk(ch.index, ch.size, data)) {
                uploading(ch.index, ch.size, data, protocol::flags::DONE);
                return;
            }
            spdlog::warn("[{}] Invalid last chunk received (index {}).", username_, ch.index);
            upload_abort(false, true, protocol::flags::CHUNK_MISMATCH);
            return;
        } else if(ch.flags == protocol::flags::ERROR) {
            upload_abort(false, false, protocol::flags::ERROR);
            return;
        } else if(ch.flags == protocol::flags::EXIT) {
            upload_abort(true, false, protocol::flags::EXIT);
            exit();
            return;
        }
    } else if (state_ == SessionState::DOWNLOADING) {
        if(ch.flags == protocol::flags::OK) {
            if(ch.transfer_id != transfer_.transfer_id) {
                download_abort(false, true, protocol::flags::ERROR);
                return;
            }
            transfer_.chunk_state[ch.index] = true;
            partmeta_->mark_chunk_received(transfer_.transfer_id, ch.index);
            downloading();
        } else if (ch.flags == protocol::flags::DONE) {
            transfer_.chunk_state[ch.index] = true;
            download_done();
            return;
        } else if(ch.flags == protocol::flags::ERROR) {
            download_abort(false, false, protocol::flags::ERROR);
            return;
        } else if(ch.flags == protocol::flags::EXIT) {
            upload_abort(true, false, protocol::flags::EXIT);
            exit();
        }
    }
}

void Session::handle_resumes() {
    if (!resuming_) {
        std::vector<PartialMetadataEntry> entries = partmeta_->get_entries();
        if (entries.empty()) return;
        resuming_ = true;
        for (const auto& entry : entries) {
            files_to_be_resumed.push(entry);
        }
    }

    if(files_to_be_resumed.empty()) {
        resuming_ = false;
        state_ = SessionState::READY;
        protocol::Response res {
            protocol::statuses::OK,
            protocol::codes::OK,
            "Ready.",
            ""
        };
        send_res(res); // a caller (e.g. need_input()'s "n" branch) may be waiting on a reply here
        return;
    }

    state_ = SessionState::NEED_INPUT_RESUME_TREANSFER;
    PartialMetadataEntry file_to_resume = files_to_be_resumed.front();
    protocol::Response res {
        protocol::statuses::RESUME,
        protocol::codes::OK,
        "Do you want to resume " + to_string(file_to_resume.type) + " of " + fsutils::relative(user_dir_ ,file_to_resume.absolute_path).string() + "? (y/n)",
        ""
    };
    send_res(res);
}

void Session::login(protocol::Request& req) {
    if(state_ != SessionState::LOGIN) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Already logged in",
            ""
        };
        send_res(res);
        return;
    }

    if(req.first_argument.empty()) {
        username_ = "public";
        if(!setup_dir()) return; // setup_dir() already answered with the error
        protocol::Response res {
            protocol::statuses::OK,
            protocol::codes::OK,
            "[warning] no username provided. Operating in public mode.",
            ""
        };
        send_res(res);
        state_ = SessionState::READY; // resume is private-mode only, no handle_resumes() here
        return;
    }

    if(req.first_argument == "public") {
            username_ = "public";
            if(!setup_dir()) return; // setup_dir() already answered with the error
            protocol::Response res {
                protocol::statuses::OK,
                protocol::codes::OK,
                "[warning] username \"public\" is reserved for public mode. Operating in public mode.",
                ""
            };
            send_res(res);
            state_ = SessionState::READY; // resume is private-mode only, no handle_resumes() here
            return;
        }

    // Public mode never reads users.json, so only an account login needs it to be readable
    if(!require_database()) return;

    username_ = req.first_argument;
    if(!db_->user_exists(req.first_argument)) {
        protocol::Response res {
            protocol::statuses::NEED_INPUT,
            protocol::codes::UNAUTHORIZED,
            "User does not exist. Do you want to register? (Y/n)",
            ""
        };
        send_res(res);
        state_ = SessionState::NEED_INPUT_REGISTER;
        return;
    } else {
        protocol::Response res {
            protocol::statuses::AUTH,
            protocol::codes::OK,
            "Please provide your password.",
            ""
        };
        send_res(res);
        state_ = SessionState::AUTH;
        return;
    }
}

bool Session::setup_dir() {
    if(username_ != "public") {
        // Private data lives on the user's storage tier, which is not necessarily the control root
        std::filesystem::path base = storage_->get_user_root(username_);
        if(base.empty()) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::INTERNAL_SERVER_ERROR,
                "Your storage tier is not configured on this server. Contact the administrator.",
                ""
            };
            send_res(res);
            return false;
        }
        if(fsutils::is_directory(std::filesystem::path(base / "private" / username_))) {
            if(!fsutils::is_directory(std::filesystem::path(base / "private" / username_ / "files"))) {
                fsutils::mkdir(std::filesystem::path(base / "private" / username_ / "files"));
            }
            if(!fsutils::is_directory(std::filesystem::path(base / "private" / username_ / ".partial"))) {
                fsutils::mkdir(std::filesystem::path(base / "private" / username_ / ".partial"));
                fsutils::create_empty_file(std::filesystem::path(base / "private" / username_ / ".partial/partmeta.json"));
            }
            if(!fsutils::is_file(std::filesystem::path(base / "private" / username_ / ".partial/partmeta.json"))) {
                fsutils::create_empty_file(std::filesystem::path(base / "private" / username_ / ".partial/partmeta.json"));
            }

        } else {
            fsutils::mkdir(std::filesystem::path(base / "private" / username_));
            fsutils::mkdir(std::filesystem::path(base / "private" / username_ / "files"));
            fsutils::mkdir(std::filesystem::path(base / "private" / username_ / ".partial"));
            fsutils::create_empty_file(std::filesystem::path(base / "private" / username_ / ".partial/partmeta.json"));
        }
        // Sits beside .partial/, outside files/, so it never shows up in a listing or a SYNC diff
        if(!fsutils::is_directory(std::filesystem::path(base / "private" / username_ / ".vault"))) {
            fsutils::mkdir(std::filesystem::path(base / "private" / username_ / ".vault"));
        }
        user_dir_ = fsutils::absolute(std::filesystem::path(base / "private" / username_ / "files"));
    } else {
        if(fsutils::is_directory(std::filesystem::path(root_ / "public"))) {
            if(!fsutils::is_directory(std::filesystem::path(root_ / "public" / "files"))) {
                fsutils::mkdir(std::filesystem::path(root_ / "public" / "files"));
            }
            if(!fsutils::is_directory(std::filesystem::path(root_ / "public" / ".partial"))) {
                fsutils::mkdir(std::filesystem::path(root_ / "public" / ".partial"));
                fsutils::create_empty_file(std::filesystem::path(root_ / "public/.partial/partmeta.json"));
            }
            if(!fsutils::is_file(std::filesystem::path(root_ / "public/.partial/partmeta.json"))) {
                fsutils::create_empty_file(std::filesystem::path(root_ / "public/.partial/partmeta.json"));
            }
        } else {
            fsutils::mkdir(std::filesystem::path(root_ / "public"));
            fsutils::mkdir(std::filesystem::path(root_ / "public" / "files"));
            fsutils::mkdir(std::filesystem::path(root_ / "public"/ ".partial"));
            fsutils::create_empty_file(std::filesystem::path(root_ / "public/.partial/partmeta.json"));
        }
        user_dir_ = fsutils::absolute(std::filesystem::path(root_ / "public" / "files"));
    }
    current_dir_ = user_dir_;
    partmeta_ = storage_->get_partmeta(username_);
    // Both are null in public mode by design: a shared anonymous area has no owner whose password
    // could root a key hierarchy. Every vault handler treats null as "no vault here".
    vault_ = storage_->get_vault(username_);
    dek_ = storage_->get_dek_manifest(username_);
    return true;
}

void Session::auth(protocol::Request& req) {
    if(state_ != SessionState::AUTH) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Not in authentication state",
            ""
        };
        send_res(res);
        return;
    }

    if(req.first_argument.empty()) {
        protocol::Response res {
            protocol::statuses::AUTH,
            protocol::codes::BAD_REQUEST,
            "Password cannot be empty. Try again.",
            ""
        };
        send_res(res);
        return;
    } else if(db_->user_exists(username_)) {
        // Consulted before the password is verified, so an attempt made during a lockout is
        // rejected without ever reaching record_failure() and therefore cannot extend the lockout.
        // That is what stops the lockout itself becoming a way to keep a real user locked out.
        AuthLimiter& limiter = storage_->get_auth_limiter();
        AuthLimiter::Decision decision = limiter.check(username_);
        if(!decision.allowed) {
            spdlog::warn("[{}] Authentication refused: locked out for another {}s.",
                         username_, decision.retry_after_seconds);
            protocol::Response res {
                protocol::statuses::AUTH,
                protocol::codes::TOO_MANY_REQUESTS,
                "Too many failed attempts. Try again in " +
                    std::to_string(decision.retry_after_seconds) + " second(s).",
                ""
            };
            send_res(res);
            return;
        }

        if(db_->validate_user(username_, req.first_argument)) {
            limiter.record_success(username_);
            spdlog::info("[{}] Authentication succeeded.", username_);
            if(!setup_dir()) return; // setup_dir() already answered with the error
            state_ = SessionState::READY;
            // The next response out of send_res() carries the salt, Argon2id parameters and sealed
            // vault key, if this account has a vault - everything the client needs to derive its
            // master key locally, and nothing that lets the server derive it.
            send_vault_info_ = true;

            // Send exactly one response: either the normal OK, or the resume question in its
            // place -- never both, since a second back-to-back response races against whatever
            // the client already has buffered on stdin (same class of bug as the login race).
            std::vector<PartialMetadataEntry> entries = partmeta_->get_entries();
            if(entries.empty()) {
                protocol::Response res {
                    protocol::statuses::OK,
                    protocol::codes::OK,
                    "Authentication successful.",
                    ""
                };
                send_res(res);
            } else {
                resuming_ = true;
                for(const auto& entry : entries) {
                    files_to_be_resumed.push(entry);
                }
                handle_resumes();
            }
            return;
        } else {
            limiter.record_failure(username_);
            spdlog::warn("[{}] Authentication failed: invalid password.", username_);

            // Report the lockout on the attempt that caused it, rather than letting the caller
            // discover it on the next try - and say the same thing to everyone, since the message
            // itself must not become a signal about whether the username is worth attacking.
            AuthLimiter::Decision after = limiter.check(username_);
            if(!after.allowed) {
                spdlog::warn("[{}] Locked out for {}s after repeated failures.", username_, after.retry_after_seconds);
                protocol::Response res {
                    protocol::statuses::AUTH,
                    protocol::codes::TOO_MANY_REQUESTS,
                    "Too many failed attempts. Try again in " +
                        std::to_string(after.retry_after_seconds) + " second(s).",
                    ""
                };
                send_res(res);
                return;
            }

            protocol::Response res {
                protocol::statuses::AUTH,
                protocol::codes::UNAUTHORIZED,
                "Invalid password. Try again.",
                ""
            };
            send_res(res);
            return;
        }
    } else if(!db_->user_exists(username_)) {
        // Refused when users.json has become unreadable since LOGIN - the fail-closed Database
        // will not write over it, and then an existing account can also look "missing" here.
        if(!db_->add_user(username_, req.first_argument, storage_->get_default_tier())) {
            spdlog::error("[{}] Registration failed: account database could not be written.", username_);
            refuse_database();
            return;
        }
        spdlog::info("[{}] Registered new user.", username_);
        if(!setup_dir()) return; // setup_dir() already answered with the error
        protocol::Response res {
            protocol::statuses::OK,
            protocol::codes::OK,
            "Registration successful.",
            ""
        };
        send_res(res);
        state_ = SessionState::READY;
        handle_resumes();
        return;
    }
}

void Session::need_input(protocol::Request& req) {
    switch (state_) {
        case SessionState::NEED_INPUT_REGISTER:
            if(req.first_argument == "y") {
                protocol::Response res {
                    protocol::statuses::AUTH,
                    protocol::codes::UNAUTHORIZED,
                    "For registration, please provide a password.",
                    ""
                };
                send_res(res);
                state_ = SessionState::AUTH;
                return; 
            } else if(req.first_argument == "n") {
                username_ = "public";
                if(!setup_dir()) return; // setup_dir() already answered with the error

                protocol::Response res {
                    protocol::statuses::OK,
                    protocol::codes::OK,
                    "[warning] no registrartion. Operating in public mode.",
                    ""
                };
                send_res(res);
                state_ = SessionState::READY;
                return; 
            } else {
                protocol::Response res {
                    protocol::statuses::ERROR,
                    protocol::codes::BAD_REQUEST,
                    "Invalid input for registration. Y/n expected.",
                    ""
                };
                send_res(res);
                return; 
            }
            break;
        case SessionState::NEED_INPUT_RESUME_TREANSFER:
            if(req.first_argument == "y") {
                PartialMetadataEntry entry = files_to_be_resumed.front();
                files_to_be_resumed.pop();

                // The resumed transfer outlives this handler, so its lock is parked straight away
                // rather than moved on a success path - there is no failure path between here and
                // the transfer starting.
                lock_ = acquire_lock();
                if(!lock_) {
                    protocol::Response res {
                        protocol::statuses::ERROR,
                        protocol::codes::SERVICE_UNAVAILABLE,
                        "Server is busy, skipping " + to_string(entry.type) + " of " + fsutils::relative(user_dir_, entry.absolute_path).string(),
                        ""
                    };
                    send_res(res);
                    handle_resumes();
                    return;
                }

                transfer_.transfer_id = entry.id;
                transfer_.fmeta.absolute_path = entry.absolute_path;
                transfer_.fmeta.size = entry.size;
                transfer_.fmeta.hash = entry.file_hash;
                transfer_.chunks = entry.chunks;
                transfer_.chunk_state = entry.chunk_state;
                transfer_.partial_path = partmeta_->get_partial_path(entry.id);
                // Without these a resumed vaulted upload would finish and be stored with no key
                // entry, leaving a file nobody - including its owner - could ever open again
                transfer_.wrapped_dek = entry.wrapped_dek;
                transfer_.plaintext_hash = entry.plaintext_hash;
                transfer_.plaintext_size = entry.plaintext_size;

                protocol::Response res {
                    protocol::statuses::RESUME,
                    protocol::codes::OK,
                    to_string(entry.type) + " " + fsutils::relative(user_dir_, entry.absolute_path).string(),
                    std::to_string(entry.id) // reuse file_hash field to carry the transfer id being resumed
                };

                // A resumed download needs its data key just as much as a fresh one does. The
                // DOWNLOAD response that would normally carry it never happens here, so it rides
                // on the kickoff instead - otherwise the client would finish the transfer and
                // write the server's ciphertext to disk under the plaintext file's name.
                if(entry.type == TransferType::DOWNLOAD && dek_ != nullptr) {
                    if(std::optional<DekEntry> dek_entry = dek_->get(vault_key_for(entry.absolute_path))) {
                        res.wrapped_dek = dek_entry->wrapped_dek;
                        res.plaintext_hash = dek_entry->plaintext_hash;
                    }
                }

                send_res(res);

                if(entry.type == TransferType::UPLOAD) {
                    state_ = SessionState::UPLOADING; // client drives sending; transfer_ is seeded from the entry above, so no upload_init() is needed
                } else {
                    state_ = SessionState::DOWNLOADING;
                    downloading(); // server drives sending, resumes at first chunk_state==false
                }
                return;
            } else if(req.first_argument == "n") {
                PartialMetadataEntry entry = files_to_be_resumed.front();
                files_to_be_resumed.pop();
                fsutils::remove_file(partmeta_->get_partial_path(entry.id));
                partmeta_->delete_partial_metadata(entry.id);
            } else {
                protocol::Response res {
                    protocol::statuses::ERROR,
                    protocol::codes::BAD_REQUEST,
                    "Invalid input for resume confirmation. Y/n expected.",
                    ""
                };
                send_res(res);
                return;
            }
            handle_resumes();
            break;
        case SessionState::NEED_INPUT_SET_TIER:
            if(req.first_argument == "y") {
                finish_set_tier();
                return;
            } else if(req.first_argument == "n") {
                pending_tier_.clear();
                state_ = SessionState::READY;
                lock_.release(); // The lock set_tier() parked for the confirmation
                protocol::Response res {
                    protocol::statuses::OK,
                    protocol::codes::OK,
                    "Tier change cancelled.",
                    ""
                };
                send_res(res);
                return;
            } else {
                protocol::Response res {
                    protocol::statuses::ERROR,
                    protocol::codes::BAD_REQUEST,
                    "Invalid input for tier change. Y/n expected.",
                    ""
                };
                send_res(res);
                return; // stays in NEED_INPUT_SET_TIER, lock still held, question can be answered again
            }
            break;
        case SessionState::NEED_INPUT_REVOKE_DEVICE:
            if(req.first_argument == "y") {
                finish_revoke_device();
                return;
            } else if(req.first_argument == "n") {
                pending_device_.clear();
                state_ = SessionState::READY;
                protocol::Response res {
                    protocol::statuses::OK,
                    protocol::codes::OK,
                    "Device revocation cancelled.",
                    ""
                };
                send_res(res);
                return;
            } else {
                protocol::Response res {
                    protocol::statuses::ERROR,
                    protocol::codes::BAD_REQUEST,
                    "Invalid input for device revocation. Y/n expected.",
                    ""
                };
                send_res(res);
                return; // stays in NEED_INPUT_REVOKE_DEVICE, the question can be answered again
            }
            break;
        default:
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::SERVICE_UNAVAILABLE,
                "No input required at this time.",
                ""
            };
            send_res(res);
            return; 
    }
    // Implementation of need_input
}

void Session::exit() {
    bool expected = false;
    if(!exiting_.compare_exchange_strong(expected, true)) return;

    // exit() is also called from Server::exit_all_sessions() on the signal-handling thread, which
    // until now touched state_/transfer_/the socket while this session's own handlers could be
    // running on another pool thread. Hop onto the strand so the teardown is serialized with
    // everything else this session does. exiting_ is set above, synchronously, so in-flight
    // handlers still bail out immediately rather than waiting for this post to run.
    auto self = shared_from_this();
    asio::post(strand_, [this, self] { do_exit(); });
}

void Session::do_exit() {
    bool socket_opened = stream_->is_open();

    if(state_ == SessionState::UPLOADING ) {
        upload_abort_exit(true, socket_opened, protocol::flags::EXIT);
        return;
    } else if (state_ == SessionState::DOWNLOADING) {
        download_abort_exit(true, socket_opened, protocol::flags::EXIT);
        return;
    } else if (socket_opened) {
        protocol::Response res{
            protocol::statuses::EXIT,
            protocol::codes::OK,
            "Goodbye.",
            ""
        };
        res.chunks.clear();
        json j;
        protocol::to_json(j, res);
        write_response_json_exit(j);
        return;
    }
    finish_exit();
}

void Session::finish_exit() {
    auto self = shared_from_this();

    // Whatever a transfer or a pending tier change parked. Synchronous handlers hold theirs on the
    // stack and cannot still be running here, so this is the only lock left to let go of; the
    // member's destructor is the backstop if a path ever reaches destruction without getting here.
    lock_.release();

    std::error_code ec;
    stream_->shutdown(ec);
    stream_->close(ec);

    state_ = SessionState::EXIT;
    on_exit_(self);
}

void Session::list(protocol::Request& req) {
    if(state_ != SessionState::READY) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Session not ready.",
            ""
        };
        send_res(res);
        return;
    }
    UserLock lock = acquire_lock();
    if(!lock) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Server is busy",
            ""
        };
        send_res(res);
        return;
    }

    if(req.first_argument.empty()) {
        std::vector<fsutils::FileMetadata> files = fsutils::scan_directory(current_dir_, false);
        if(fsutils::is_scan_dir_error(files)) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::INTERNAL_SERVER_ERROR,
                "Failed to list directory",
                ""
            };
            send_res(res);
            return;
        }
        auto curr_rel = fsutils::relative(user_dir_, current_dir_);
        std::string file_list("Current directory: " + curr_rel.string() + "\n");
        for (const auto& file : files) {
            std::filesystem::path relative = fsutils::relative(current_dir_, file.absolute_path);
            file_list += relative.string() + "\n";
        }
        protocol::Response res {
            protocol::statuses::OK,
            protocol::codes::OK,
            file_list,
            ""
        };
        send_res(res);
        return;
    }else {
        // Containment first, before the filesystem is asked anything at all: answering "does not
        // exist" for a path outside the root would tell the caller whether it exists.
        std::optional<std::filesystem::path> requested_dir = guard_path(user_dir_, current_dir_, req.first_argument);
        if(!requested_dir) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::FORBIDDEN,
                "Access denied.",
                ""
            };
            send_res(res);
            return;
        }
        if(!fsutils::is_directory(*requested_dir)) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::BAD_REQUEST,
                "Directory does not exist.",
                ""
            };
            send_res(res);
            return;
        }
        std::vector<fsutils::FileMetadata> files = fsutils::scan_directory(*requested_dir, false);
        if(fsutils::is_scan_dir_error(files)) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::INTERNAL_SERVER_ERROR,
                "Failed to list directory",
                ""
            };
            send_res(res);
            return;
        }
        std::string file_list;
        for (const auto& file : files) {
            std::filesystem::path relative = fsutils::relative(*requested_dir, file.absolute_path);
            file_list += relative.string() + "\n";
        }
        protocol::Response res {
            protocol::statuses::OK,
            protocol::codes::OK,
            file_list,
            ""
        };
        send_res(res);
        return;
    }
}

void Session::delete_file(protocol::Request& req) {
    if(state_ != SessionState::READY) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Session not ready.",
            ""
        };
        send_res(res);
        return;
    }
    UserLock lock = acquire_lock();
    if(!lock) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Server is busy",
            ""
        };
        send_res(res);
        return;
    }
    std::optional<std::filesystem::path> requested_file = guard_path(user_dir_, current_dir_, req.first_argument);
    if(!requested_file) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::FORBIDDEN,
            "Access denied.",
            ""
        };
        send_res(res);
        return;
    }
    if(!fsutils::is_file(*requested_file)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::BAD_REQUEST,
            "File does not exist.",
            ""
        };
        send_res(res);
        return;
    }
    if(!fsutils::remove_file(*requested_file)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::INTERNAL_SERVER_ERROR,
            "Cannot remove file",
            ""
        };
        send_res(res);
        return;
    }
    // A key for a file that is gone is dead weight that would also be handed back if the same
    // path were later reused by a plain upload
    if(dek_ != nullptr) dek_->remove(vault_key_for(*requested_file));
    protocol::Response res {
        protocol::statuses::OK,
        protocol::codes::OK,
        "File deleted",
        ""
    };
    send_res(res);
}

void Session::upload(protocol::Request& req) {
    if(state_ != SessionState::READY) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Session not ready.",
            ""
        };
        send_res(res);
        return;
    }
    UserLock lock = acquire_lock();
    if(!lock) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Server is busy",
            ""
        };
        send_res(res);
        return;
    }
    std::optional<std::filesystem::path> guarded = guard_path(user_dir_, current_dir_, req.first_argument);
    if(!guarded) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::FORBIDDEN,
            "Access denied.",
            ""
        };
        send_res(res);
        return;
    }
    const std::filesystem::path requested_file = *guarded;
    if(fsutils::is_file(requested_file)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::PRECONDITION_FAILED,
            "File already exists.",
            ""
        };
        send_res(res);
        return;
    }
    if(fsutils::is_directory(requested_file)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::PRECONDITION_FAILED,
            "Requested file is an existing directory.",
            ""
        };
        send_res(res);
        return;
    }
    if(req.size > UINT32_MAX) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::PRECONDITION_FAILED,
            "File too large. Max 4GB.",
            ""
        };
        send_res(res);
        return;
    }
    fsutils::FileMetadata fmeta{
        requested_file,
        req.size,
        0,
        fsutils::hex_to_hash(req.file_hash)
    };

    transfer_.fmeta = fmeta;
    transfer_.chunks = req.chunks;
    transfer_.chunk_state = std::vector<bool>(req.chunks.size(), false);
    transfer_.transfer_id = UINT32_MAX; // upload_init() allocates the real one
    // What arrives on the wire for a vaulted account is ciphertext, and req.file_hash above is its
    // hash: the server verifies and stores exactly the bytes it is given, as it always has. These
    // three describe what is inside, and only the client can act on them.
    transfer_.wrapped_dek = req.wrapped_dek;
    transfer_.plaintext_hash = req.plaintext_hash;
    transfer_.plaintext_size = req.plaintext_size;

    if(!upload_init()) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::INTERNAL_SERVER_ERROR,
            "Failed to prepare the upload.",
            ""
        };
        send_res(res);
        return;
    }

    protocol::Response res {
        protocol::statuses::OK,
        protocol::codes::OK,
        "Starting upload to file: " + fsutils::relative(user_dir_, requested_file).string(),
        std::to_string(transfer_.transfer_id) // file_hash carries the transfer id, as the RESUME kickoff already does
    };
    send_res(res);
    // The transfer outlives this handler, so it takes the lock with it; upload_done()/upload_abort()
    // release it. Moving only here means every error path above released by simply returning.
    lock_ = std::move(lock);
    state_ = SessionState::UPLOADING;
    return;
}

void Session::download(protocol::Request& req) {
    if(state_ != SessionState::READY) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Session not ready.",
            ""
        };
        send_res(res);
        return;
    }
    UserLock lock = acquire_lock();
    if(!lock) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Server is busy",
            ""
        };
        send_res(res);
        return;
    }
    std::optional<std::filesystem::path> guarded = guard_path(user_dir_, current_dir_, req.first_argument);
    if(!guarded) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::FORBIDDEN,
            "Access denied.",
            ""
        };
        send_res(res);
        return;
    }
    const std::filesystem::path requested_file = *guarded;
    if(!fsutils::is_file(requested_file)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::PRECONDITION_FAILED,
            "File does not exist.",
            ""
        };
        send_res(res);
        return;
    }
    if(fsutils::is_directory(requested_file)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::PRECONDITION_FAILED,
            "Requested file is an existing directory.",
            ""
        };
        send_res(res);
        return;
    }
    fsutils::FileMetadata fmeta = fsutils::scan_file(requested_file);

    if(fsutils::is_scan_file_error(fmeta)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::INTERNAL_SERVER_ERROR,
            "Failed to scan file.",
            ""
        };
        send_res(res);
        return;
    }

    if(fmeta.size == 0) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::PRECONDITION_FAILED,
            "File is empty.",
            ""
        };
        send_res(res);
        return;
    }

    std::vector<protocol::ChunkInfo> chunks = fsutils::compute_chunks(fmeta);

    if(fsutils::is_compute_chunks_error(chunks)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::INTERNAL_SERVER_ERROR,
            "Gathering chunk data failed.",
            ""
        };
        send_res(res);
        return;
    }

    transfer_.fmeta = fmeta;
    transfer_.chunks = chunks;
    transfer_.chunk_state = std::vector<bool>(chunks.size(), false);

    if(fmeta.size > UINT32_MAX) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::PRECONDITION_FAILED,
            "File too large. Max 4GB.",
            ""
        };
        send_res(res);
        return;
    }

    protocol::Response res {
        protocol::statuses::OK,
        protocol::codes::OK,
        "Starting download.",
        fsutils::hash_to_hex(transfer_.fmeta.hash)
    };
    res.chunks = transfer_.chunks;

    // A file stored before the vault existed has no entry here, so it downloads as plain bytes -
    // which is correct, because that is what is on disk. Only sealed files carry a sealed key.
    if(dek_ != nullptr) {
        if(std::optional<DekEntry> entry = dek_->get(vault_key_for(requested_file))) {
            res.wrapped_dek = entry->wrapped_dek;
            res.plaintext_hash = entry->plaintext_hash;
        }
    }
    json j;
    protocol::to_json(j, res);
    write_response_json(j);
    // Handed to the transfer, same as upload(); download_done()/download_abort() release it
    lock_ = std::move(lock);
    state_ = SessionState::DOWNLOADING;
    download_init();
    return;
}

void Session::cd(protocol::Request& req) {
    if(state_ != SessionState::READY) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Session not ready.",
            ""
        };
        send_res(res);
        return;
    }
    std::optional<std::filesystem::path> requested_dir = guard_path(user_dir_, current_dir_, req.first_argument);
    if(!requested_dir) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::FORBIDDEN,
            "Access denied.",
            ""
        };
        send_res(res);
        return;
    }
    if(!fsutils::is_directory(*requested_dir)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::BAD_REQUEST,
            "Directory does not exist.",
            ""
        };
        send_res(res);
        return;
    }
    current_dir_ = *requested_dir;
    protocol::Response res {
        protocol::statuses::OK,
        protocol::codes::OK,
        "Directory changed.",
        ""
    };
    send_res(res);
}
void Session::mkdir(protocol::Request& req) {
    if(state_ != SessionState::READY) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Session not ready.",
            ""
        };
        send_res(res);
        return;
    }
    if(req.first_argument.empty()) {
        protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::BAD_REQUEST,
                "Directory path cannot be empty.",
                ""
            };
            send_res(res);
            return;
    }
    UserLock lock = acquire_lock();
    if(!lock) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Server is busy",
            ""
        };
        send_res(res);
        return;
    }
    std::optional<std::filesystem::path> requested_dir = guard_path(user_dir_, current_dir_, req.first_argument);
    if(!requested_dir) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::FORBIDDEN,
            "Access denied.",
            ""
        };
        send_res(res);
        return;
    }
    if(fsutils::is_directory(*requested_dir)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::PRECONDITION_FAILED,
            "Directory already exists.",
            ""
        };
        send_res(res);
        return;
    }
    if(!fsutils::mkdir(*requested_dir)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::INTERNAL_SERVER_ERROR,
            "Cannot create directory.",
            ""
        };
        send_res(res);
        return;
    }
    protocol::Response res {
        protocol::statuses::OK,
        protocol::codes::OK,
        "Directory created.",
        ""
    };
    send_res(res);
}
void Session::rmdir(protocol::Request& req) {
    if(state_ != SessionState::READY) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Session not ready.",
            ""
        };
        send_res(res);
        return;
    }
    if(req.first_argument.empty()) {
        protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::BAD_REQUEST,
                "Directory path cannot be empty.",
                ""
            };
            send_res(res);
            return;
    }
    UserLock lock = acquire_lock();
    if(!lock) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Server is busy",
            ""
        };
        send_res(res);
        return;
    }
    std::optional<std::filesystem::path> requested_dir = guard_path(user_dir_, current_dir_, req.first_argument);
    // The root itself is inside the root, so guard_path accepts it - removing it is a separate
    // rule, and deliberately answers the same 403 so the two are indistinguishable from outside.
    if(!requested_dir || *requested_dir == user_dir_) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::FORBIDDEN,
            "Access denied.",
            ""
        };
        send_res(res);
        return;
    }
    if(!fsutils::is_directory(*requested_dir)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::BAD_REQUEST,
            "Directory does no  exist.",
            ""
        };
        send_res(res);
        return;
    }
    if(!fsutils::rmdir(*requested_dir)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::INTERNAL_SERVER_ERROR,
            "Cannot delete directory.",
            ""
        };
        send_res(res);
        return;
    }
    // rmdir takes the whole subtree with it, and so must every key under it
    if(dek_ != nullptr) dek_->remove_subtree(vault_key_for(*requested_dir));
    protocol::Response res {
        protocol::statuses::OK,
        protocol::codes::OK,
        "Directory deleted.",
        ""
    };
    send_res(res);
}
void Session::move(protocol::Request& req) {
    if(state_ != SessionState::READY) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Session not ready.",
            ""
        };
        send_res(res);
        return;
    }
    if(req.first_argument.empty() || req.second_argument.empty()) {
        protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::BAD_REQUEST,
                "File path cannot be empty.",
                ""
            };
            send_res(res);
            return;
    }
    UserLock lock = acquire_lock();
    if(!lock) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Server is busy",
            ""
        };
        send_res(res);
        return;
    }
    std::optional<std::filesystem::path> src = guard_path(user_dir_, current_dir_, req.first_argument);
    std::optional<std::filesystem::path> dst = guard_path(user_dir_, current_dir_, req.second_argument);
    if(!src || !dst) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::FORBIDDEN,
            "Access denied.",
            ""
        };
        send_res(res);
        return;
    }
    if(!(fsutils::is_directory(*src) || fsutils::is_file(*src))) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::PRECONDITION_FAILED,
            "Incorrect paths.",
            ""
        };
        send_res(res);
        return;
    }
    if(fsutils::is_directory(*dst) || fsutils::is_file(*dst)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::PRECONDITION_FAILED,
            "Destination path already exists.",
            ""
        };
        send_res(res);
        return;
    }
    if(!(fsutils::move_path(*src, *dst, false))) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::INTERNAL_SERVER_ERROR,
            "Cannot move paths.",
            ""
        };
        send_res(res);
        return;
    }
    // The manifest is keyed by path, so a rename has to move the keys with the bytes - including
    // every key beneath a renamed directory
    if(dek_ != nullptr) dek_->rename(vault_key_for(*src), vault_key_for(*dst));
    protocol::Response res {
        protocol::statuses::OK,
        protocol::codes::OK,
        "Path moved.",
        ""
    };
    send_res(res);
}
void Session::copy(protocol::Request& req) {
    if(state_ != SessionState::READY) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Session not ready.",
            ""
        };
        send_res(res);
        return;
    }
    if(req.first_argument.empty() || req.second_argument.empty()) {
        protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::BAD_REQUEST,
                "File path cannot be empty.",
                ""
            };
            send_res(res);
            return;
    }
    UserLock lock = acquire_lock();
    if(!lock) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Server is busy",
            ""
        };
        send_res(res);
        return;
    }
    std::optional<std::filesystem::path> src = guard_path(user_dir_, current_dir_, req.first_argument);
    std::optional<std::filesystem::path> dst = guard_path(user_dir_, current_dir_, req.second_argument);
    if(!src || !dst) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::FORBIDDEN,
            "Access denied.",
            ""
        };
        send_res(res);
        return;
    }
    if(!(fsutils::is_directory(*src) || fsutils::is_file(*src))) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::PRECONDITION_FAILED,
            "Incorrect paths.",
            ""
        };
        send_res(res);
        return;
    }
    if(fsutils::is_directory(*dst) || fsutils::is_file(*dst)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::PRECONDITION_FAILED,
            "Destination path already exists.",
            ""
        };
        send_res(res);
        return;
    }
    if(!(fsutils::copy_path(*src, *dst, false))) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::INTERNAL_SERVER_ERROR,
            "Cannot copy paths.",
            ""
        };
        send_res(res);
        return;
    }
    // The copy is byte-identical ciphertext, so it opens under the very same data key. Reusing it
    // is not nonce reuse: it is the same plaintext under the same key, which is what makes SYNC's
    // "copy instead of re-uploading identical bytes" optimization work for vaulted accounts too.
    if(dek_ != nullptr) dek_->copy(vault_key_for(*src), vault_key_for(*dst));
    protocol::Response res {
        protocol::statuses::OK,
        protocol::codes::OK,
        "Path copied.",
        ""
    };
    send_res(res);
}

// SYNC is a stateless listing request: it returns a recursive hash+mtime listing of the requested
// directory and nothing else. Every mutation the client decides on afterwards is driven by the
// ordinary single-item UPLOAD/DOWNLOAD/DELETE/MOVE/COPY/MKDIR/RMDIR handlers, one request at a time.
// That is why the per-user lock must be released on *every* path out of this function before the
// client can send the next request: Storage's lock is a non-reentrant busy-boolean, so a lock left
// held here would make every follow-up command in the batch fail with "Server is busy" forever.
void Session::sync(protocol::Request& req) {
    if(state_ != SessionState::READY) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Session not ready.",
            ""
        };
        send_res(res);
        return;
    }
    if(req.first_argument.empty()) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::BAD_REQUEST,
            "Directory path cannot be empty.",
            ""
        };
        send_res(res);
        return;
    }
    UserLock lock = acquire_lock();
    if(!lock) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Server is busy",
            ""
        };
        send_res(res);
        return;
    }
    std::optional<std::filesystem::path> guarded = guard_path(user_dir_, current_dir_, req.first_argument);
    if(!guarded) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::FORBIDDEN,
            "Access denied.",
            ""
        };
        send_res(res);
        return;
    }
    const std::filesystem::path requested_dir = *guarded;
    if(!fsutils::is_directory(requested_dir)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::PRECONDITION_FAILED,
            "Remote sync directory does not exist.",
            ""
        };
        send_res(res);
        return;
    }

    std::vector<fsutils::FileMetadata> files = fsutils::scan_directory(requested_dir, true);
    if(fsutils::is_scan_dir_error(files)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::INTERNAL_SERVER_ERROR,
            "Failed to list directory",
            ""
        };
        send_res(res);
        return;
    }

    protocol::Response res {
        protocol::statuses::OK,
        protocol::codes::OK,
        "Listing of " + fsutils::relative(user_dir_, requested_dir).string(),
        ""
    };

    for(const auto& file : files) {
        bool is_dir = fsutils::is_directory(file.absolute_path);

        // What the client diffs against is its own local plaintext, so the listing has to be in
        // the plaintext domain too. For a sealed file the server cannot compute that hash - it
        // reports the one the client recorded at upload time. Independent per-file data keys mean
        // hashing the ciphertext would give a different answer for identical content, which would
        // break not just the comparison but move and copy detection with it. A file with no entry
        // is stored as plain bytes, so its real hash already is its plaintext hash.
        std::string hash;
        uint32_t size = file.size;
        if(!is_dir) {
            hash = fsutils::hash_to_hex(file.hash);
            std::optional<DekEntry> entry = dek_ != nullptr ? dek_->get(vault_key_for(file.absolute_path)) : std::nullopt;
            if(entry && !entry->plaintext_hash.empty()) {
                hash = entry->plaintext_hash;
                size = entry->plaintext_size;
            }
        }

        res.files.push_back(protocol::FileEntry{
            fsutils::relative(requested_dir, file.absolute_path).generic_string(),
            is_dir ? 0u : size,
            is_dir ? std::string() : hash,
            file.last_modified,
            is_dir
        });
    }

    send_res(res);
}

void Session::tiers(protocol::Request& req) {
    (void)req;
    if(state_ != SessionState::READY) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Session not ready.",
            ""
        };
        send_res(res);
        return;
    }

    // Reads configuration only, touches no user files, so it takes no user lock (same as cd())
    std::string current = storage_->get_user_tier(username_);

    protocol::Response res {
        protocol::statuses::OK,
        protocol::codes::OK,
        username_ == "public"
            ? "Available storage tiers (public mode uses the server's shared public storage):"
            : "Available storage tiers:",
        ""
    };

    for(const auto& tier : storage_->get_tiers()) {
        // The tier's path is deliberately not sent - clients have no business knowing the
        // server's disk layout, only the names they are allowed to pick from.
        res.tiers.push_back(protocol::TierInfo{
            tier.name,
            tier.description,
            !current.empty() && tier.name == current
        });
    }

    send_res(res);
}

void Session::set_tier(protocol::Request& req) {
    if(state_ != SessionState::READY) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Session not ready.",
            ""
        };
        send_res(res);
        return;
    }

    if(req.first_argument.empty()) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::BAD_REQUEST,
            "Tier name cannot be empty.",
            ""
        };
        send_res(res);
        return;
    }

    if(username_ == "public") {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::FORBIDDEN,
            "Public mode has no storage tier. Log in with a username to change tiers.",
            ""
        };
        send_res(res);
        return;
    }

    // The server only ever places users on media it was actually configured with
    if(storage_->find_tier(req.first_argument) == nullptr) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::NOT_FOUND,
            "Unknown storage tier '" + req.first_argument + "'. Use TIERS to see available media.",
            ""
        };
        send_res(res);
        return;
    }

    std::string current = storage_->get_user_tier(username_);
    if(current == req.first_argument) {
        protocol::Response res {
            protocol::statuses::OK,
            protocol::codes::OK,
            "Already on tier '" + current + "'.",
            ""
        };
        send_res(res);
        return;
    }

    // Partial transfer records store absolute destination paths, so moving the tree out from
    // under them would silently break every resume. Reconnecting offers to finish or discard them.
    std::vector<PartialMetadataEntry> pending = partmeta_->get_entries();
    if(!pending.empty()) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::CONFLICT,
            "You have " + std::to_string(pending.size()) +
                " unfinished transfer(s). Reconnect to finish or discard them before changing tier.",
            ""
        };
        send_res(res);
        return;
    }

    // Held across the confirmation so a concurrent upload cannot start under the migration.
    // finish_exit() releases it unconditionally, so a client that never answers cannot brick the user.
    UserLock lock = acquire_lock();
    if(!lock) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Server is busy",
            ""
        };
        send_res(res);
        return;
    }

    uint64_t files = 0;
    uint64_t bytes = 0;
    std::vector<fsutils::FileMetadata> contents = fsutils::scan_directory(user_dir_, true);
    if(!fsutils::is_scan_dir_error(contents)) {
        for(const auto& file : contents) {
            if(fsutils::is_directory(file.absolute_path)) continue;
            files++;
            bytes += file.size;
        }
    }

    pending_tier_ = req.first_argument;
    protocol::Response res {
        protocol::statuses::NEED_INPUT,
        protocol::codes::OK,
        "Move " + std::to_string(files) + " file(s), " + std::to_string(bytes) + " bytes from tier '" +
            current + "' to '" + pending_tier_ + "'? This may take a while. (Y/n)",
        ""
    };
    send_res(res);
    // The confirmation round-trip happens between handlers, so the lock is parked until
    // finish_set_tier() takes it back or need_input()'s "n" branch releases it
    lock_ = std::move(lock);
    state_ = SessionState::NEED_INPUT_SET_TIER;
}

void Session::finish_set_tier() {
    // Reached only from need_input()'s "y" branch. Taking the parked lock back onto the stack means
    // every return below releases it, including the ones that answer with an error.
    UserLock lock = std::move(lock_);

    std::string target = pending_tier_;
    pending_tier_.clear();
    state_ = SessionState::READY;

    std::string current = storage_->get_user_tier(username_);
    const StorageTier* from = storage_->find_tier(current);
    const StorageTier* to = storage_->find_tier(target);

    if(from == nullptr || to == nullptr) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::INTERNAL_SERVER_ERROR,
            "Storage tier is no longer configured on this server.",
            ""
        };
        send_res(res);
        return;
    }

    // Synchronous on purpose: the per user lock already makes this user's other commands fail
    // fast with "Server is busy", and the io_context runs a thread per core so the server keeps
    // serving everyone else. A very large move still occupies one of those threads.
    MigrationResult result = storage_->migrate_user(username_, *from, *to);

    if(!result.ok) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::INTERNAL_SERVER_ERROR,
            result.error,
            ""
        };
        send_res(res);
        return;
    }

    if(!db_->set_storage_class(username_, target)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::INTERNAL_SERVER_ERROR,
            "Data was moved to tier '" + target + "' but the change could not be recorded.",
            ""
        };
        send_res(res);
        return;
    }

    // Re-point user_dir_, current_dir_ and partmeta_ at the new medium
    if(!setup_dir()) {
        return; // setup_dir() already answered with the error
    }

    protocol::Response res {
        protocol::statuses::OK,
        protocol::codes::OK,
        "Moved to tier '" + target + "'. " + std::to_string(result.files) + " file(s), " +
            std::to_string(result.bytes) + " bytes.",
        ""
    };
    send_res(res);
}

// The four vault handlers below share a shape worth stating once: the server validates structure,
// stores bytes, and returns bytes. It performs no cryptography and holds no key that opens any of
// what it stores, which is exactly what makes the account zero-knowledge for file contents.

void Session::vault_init(protocol::Request& req) {
    if(state_ != SessionState::READY) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Session not ready.",
            ""
        };
        send_res(res);
        return;
    }

    if(vault_ == nullptr) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::FORBIDDEN,
            "Public mode has no vault. Log in with a username to use end-to-end encryption.",
            ""
        };
        send_res(res);
        return;
    }

    if(vault_->is_initialized()) {
        // Re-initializing would mint a fresh vault key and orphan every file already sealed under
        // the old one, with nothing on the server able to recover them.
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::CONFLICT,
            "This account already has a vault.",
            ""
        };
        send_res(res);
        return;
    }

    UserLock lock = acquire_lock();
    if(!lock) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Server is busy",
            ""
        };
        send_res(res);
        return;
    }

    std::string error;
    if(!vault_->initialize(req.vault, error)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::BAD_REQUEST,
            error,
            ""
        };
        send_res(res);
        return;
    }

    spdlog::info("[{}] Vault initialized.", username_);
    protocol::Response res {
        protocol::statuses::OK,
        protocol::codes::OK,
        "Vault created. Files uploaded from now on are encrypted before they leave this machine.",
        ""
    };
    send_res(res);
}

void Session::enroll_device(protocol::Request& req) {
    if(state_ != SessionState::READY) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Session not ready.",
            ""
        };
        send_res(res);
        return;
    }

    if(vault_ == nullptr || !vault_->is_initialized()) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::PRECONDITION_FAILED,
            "This account has no vault. Run VAULT_INIT first.",
            ""
        };
        send_res(res);
        return;
    }

    const protocol::DeviceInfo& info = req.device;
    if(info.device_id.empty() || info.x25519_pub.empty() || info.mlkem_pub.empty() || info.wrapped_vk.empty()) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::BAD_REQUEST,
            "Incomplete device enrollment.",
            ""
        };
        send_res(res);
        return;
    }

    VaultDevice device{
        info.device_id,
        info.device_name.empty() ? std::string("unnamed device") : info.device_name,
        info.algorithm.empty() ? std::string("x25519+mlkem768") : info.algorithm,
        info.x25519_pub,
        info.mlkem_pub,
        info.wrapped_vk,
        static_cast<uint64_t>(std::time(nullptr)),
        0
    };

    std::string error;
    if(!vault_->add_device(device, error)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::CONFLICT,
            error,
            ""
        };
        send_res(res);
        return;
    }

    spdlog::info("[{}] Enrolled vault device '{}' ({}).", username_, device.device_name, device.algorithm);
    protocol::Response res {
        protocol::statuses::OK,
        protocol::codes::OK,
        "Device '" + device.device_name + "' enrolled. It can now unlock the vault without re-deriving "
            "the master key from your password.",
        ""
    };
    send_res(res);
}

void Session::revoke_device(protocol::Request& req) {
    if(state_ != SessionState::READY) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Session not ready.",
            ""
        };
        send_res(res);
        return;
    }

    if(vault_ == nullptr || !vault_->is_initialized()) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::PRECONDITION_FAILED,
            "This account has no vault.",
            ""
        };
        send_res(res);
        return;
    }

    if(req.first_argument.empty()) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::BAD_REQUEST,
            "Device id cannot be empty. Use DEVICES to list enrolled devices.",
            ""
        };
        send_res(res);
        return;
    }

    std::optional<VaultDevice> device = vault_->get_device(req.first_argument);
    if(!device) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::NOT_FOUND,
            "No device with that id is enrolled.",
            ""
        };
        send_res(res);
        return;
    }

    pending_device_ = req.first_argument;
    protocol::Response res {
        protocol::statuses::NEED_INPUT,
        protocol::codes::OK,
        "Revoke device '" + device->device_name + "'? It will no longer be able to unlock the vault, "
            "but the vault key is not rotated, so a copy it already holds stays valid. (Y/n)",
        ""
    };
    send_res(res);
    state_ = SessionState::NEED_INPUT_REVOKE_DEVICE;
}

void Session::finish_revoke_device() {
    std::string target = pending_device_;
    pending_device_.clear();
    state_ = SessionState::READY;

    if(vault_ == nullptr || !vault_->remove_device(target)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::INTERNAL_SERVER_ERROR,
            "The device could not be revoked.",
            ""
        };
        send_res(res);
        return;
    }

    spdlog::info("[{}] Revoked vault device {}.", username_, target);
    protocol::Response res {
        protocol::statuses::OK,
        protocol::codes::OK,
        "Device revoked.",
        ""
    };
    send_res(res);
}

void Session::devices(protocol::Request& req) {
    (void)req;
    if(state_ != SessionState::READY) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Session not ready.",
            ""
        };
        send_res(res);
        return;
    }

    // Reads the device table only, touches no user files, so it takes no user lock (same as cd())
    if(vault_ == nullptr || !vault_->is_initialized()) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::PRECONDITION_FAILED,
            "This account has no vault. Run VAULT_INIT first.",
            ""
        };
        send_res(res);
        return;
    }

    protocol::Response res {
        protocol::statuses::OK,
        protocol::codes::OK,
        "Devices enrolled in this vault:",
        ""
    };

    for(const auto& device : vault_->get_devices()) {
        res.devices.push_back(protocol::DeviceInfo{
            device.device_id, device.device_name, device.algorithm,
            device.x25519_pub, device.mlkem_pub, device.wrapped_vk,
            device.created_at, device.last_seen
        });
    }

    send_res(res);
}

bool Session::valid_file(const std::filesystem::path& partial_file, const std::array<uint8_t, crypto_generichash_BYTES>& expected) {
    std::array<uint8_t, crypto_generichash_BYTES> hash = fsutils::hash_file(partial_file);
    return hash == expected && !fsutils::is_hash_error(hash);
}

bool Session::valid_chunk(const uint32_t& index, const uint32_t& size, const std::vector<uint8_t>& data) {
    protocol::ChunkInfo chunk = transfer_.chunks[index];
    if(chunk.size != size) return false;
    if(chunk.index != index) return false;
    std::array<uint8_t, crypto_generichash_BYTES> hash = fsutils::hash_chunk(data);
    return hash == fsutils::hex_to_hash(chunk.chunk_hash) && !fsutils::is_hash_error(hash);
}

bool Session::upload_init() {
    // transfer_.transfer_id is UINT32_MAX for a fresh upload, so the id is allocated here out of
    // this user's partial metadata database - which is shared by all of their sessions and hands
    // out each id once. Called before the accepting response is sent, so the client can be told
    // which id to use instead of choosing one itself.
    transfer_.transfer_id = partmeta_->add_partial_metadata(TransferType::UPLOAD, transfer_.fmeta, transfer_.chunks, transfer_.transfer_id);
    transfer_.partial_path = partmeta_->get_partial_path(transfer_.transfer_id);
    spdlog::debug("[{}] Upload {} -> partial file {}", username_, transfer_.transfer_id, transfer_.partial_path.string());
    if(transfer_.partial_path.empty()) {
        spdlog::error("[{}] Failed to get partial path for upload {}.", username_, transfer_.transfer_id);
        partmeta_->delete_partial_metadata(transfer_.transfer_id);
        transfer_.transfer_id = UINT32_MAX;
        return false;
    }
    if(!fsutils::is_file(transfer_.partial_path)) {
        fsutils::create_empty_file(transfer_.partial_path);
    }
    // Persisted with the rest of the transfer's state, so an upload interrupted halfway can be
    // resumed and still land in the manifest under the data key its ciphertext was produced with
    if(!transfer_.wrapped_dek.empty()) {
        partmeta_->set_vault_info(transfer_.transfer_id, transfer_.wrapped_dek,
                                  transfer_.plaintext_hash, transfer_.plaintext_size);
    }
    return true;
}

void Session::uploading(const uint32_t& index, const uint32_t& size, const std::vector<uint8_t>& data, uint8_t flag) {
    transfer_.chunk_state[index] = true;
    partmeta_->mark_chunk_received(transfer_.transfer_id, index);

    uint32_t offset = fsutils::CHUNK_SIZE * index;
    if(!fsutils::write_chunk(transfer_.partial_path, offset, data)) {
        flag = protocol::flags::ERROR;
        spdlog::error("[{}] Failed to write chunk {} of transfer {} to file.", username_, index, transfer_.transfer_id);
        upload_abort(false, true, flag);
        return;
    }

    if(flag == protocol::flags::DONE) {
        if(!valid_file(transfer_.partial_path, transfer_.fmeta.hash)) {
            flag = protocol::flags::ERROR;
            spdlog::error("[{}] Uploaded file hash mismatch for transfer {}.", username_, transfer_.transfer_id);
            upload_abort(false, true, flag);
            return;
        }
        // Move into place before acknowledging: the client takes a DONE chunk as "stored", so this
        // failing after the fact reported a success no file backed up.
        if(!fsutils::move_path(transfer_.partial_path, transfer_.fmeta.absolute_path, true)) {
            flag = protocol::flags::ERROR;
            spdlog::error("[{}] Failed to store the uploaded file of transfer {} at {}.",
                          username_, transfer_.transfer_id, transfer_.fmeta.absolute_path.string());
            upload_abort(false, true, flag);
            return;
        }
        // Recorded only once the ciphertext is verified and in place. Doing it earlier would leave
        // a key entry pointing at a file that never arrived; doing it later would acknowledge a
        // stored file whose key the server had not yet written down.
        if(!transfer_.wrapped_dek.empty() && dek_ != nullptr) {
            dek_->put(vault_key_for(transfer_.fmeta.absolute_path),
                      DekEntry{transfer_.wrapped_dek, transfer_.plaintext_hash, transfer_.plaintext_size});
        }
    }
    protocol::ChunkHeader ch{
        transfer_.transfer_id,
        index,
        0,
        flag
    };

    std::vector<uint8_t> response_data;

    send_chunk(ch, response_data);

    if(flag == protocol::flags::DONE) {
        upload_done();
    }
}

void Session::upload_done() {
    // The file was already moved into place by uploading(), before the DONE chunk went out
    partmeta_->delete_partial_metadata(transfer_.transfer_id);

    transfer_.partial_path = std::filesystem::path("");
    transfer_.transfer_id = UINT32_MAX;
    transfer_.fmeta = fsutils::FileMetadata{};
    transfer_.chunk_state.clear();
    transfer_.chunks.clear();
    transfer_.wrapped_dek = protocol::WrappedBlob{};
    transfer_.plaintext_hash.clear();
    transfer_.plaintext_size = 0;

    lock_.release(); // The transfer's lock, parked by upload()/download() or by a resume kickoff
    if(resuming_) { handle_resumes(); } else { state_ = SessionState::READY; }
}

void Session::upload_abort(bool save, bool notify, uint8_t flag) {
    if(save) {
        partmeta_->save();
    } else {
        fsutils::remove_file(transfer_.partial_path);
        partmeta_->delete_partial_metadata(transfer_.transfer_id);

        transfer_.partial_path = std::filesystem::path("");
        transfer_.transfer_id = UINT32_MAX;
        transfer_.fmeta = fsutils::FileMetadata{};
        transfer_.chunk_state.clear();
        transfer_.chunks.clear();
        transfer_.wrapped_dek = protocol::WrappedBlob{};
        transfer_.plaintext_hash.clear();
        transfer_.plaintext_size = 0;
    }

    if(notify) {
        protocol::ChunkHeader ch{
            transfer_.transfer_id,
            UINT32_MAX,
            0,
            flag
        };

        std::vector<uint8_t> data;

        send_chunk(ch, data);
    }

    lock_.release(); // The transfer's lock, parked by upload()/download() or by a resume kickoff
    if(resuming_) { handle_resumes(); } else { state_ = SessionState::READY; }
}

void Session::upload_abort_exit(bool save, bool notify, uint8_t flag) {
    if(save) {
        partmeta_->save();
    } else {
        fsutils::remove_file(transfer_.partial_path);
        partmeta_->delete_partial_metadata(transfer_.transfer_id);

        transfer_.partial_path = std::filesystem::path("");
        transfer_.transfer_id = UINT32_MAX;
        transfer_.fmeta = fsutils::FileMetadata{};
        transfer_.chunk_state.clear();
        transfer_.chunks.clear();
        transfer_.wrapped_dek = protocol::WrappedBlob{};
        transfer_.plaintext_hash.clear();
        transfer_.plaintext_size = 0;
    }

    if(notify) {
        protocol::ChunkHeader ch{
            transfer_.transfer_id,
            UINT32_MAX,
            0,
            flag
        };

        std::vector<uint8_t> data;

        send_chunk_exit(ch, data);
    }
}

void Session::download_init() {
    transfer_.transfer_id = partmeta_->add_partial_metadata(TransferType::DOWNLOAD, transfer_.fmeta, transfer_.chunks, UINT32_MAX);
    downloading();
}

void Session::downloading() {
    uint32_t index = UINT32_MAX;

    for(uint32_t i = 0; i < transfer_.chunk_state.size(); ++i) {
        if(!transfer_.chunk_state[i]) {
            index = i;
            break;
        }
    }
    if(index == UINT32_MAX) { // Every chunk already acknowledged - nothing left to send
        spdlog::warn("[{}] downloading() called with no pending chunks (transfer {})", username_, transfer_.transfer_id);
        return;
    }
    protocol::ChunkInfo chunk = transfer_.chunks[index];
    uint8_t flag = protocol::flags::SEND;

    if(chunk.index == (transfer_.chunks.size() - 1)) {
        flag = protocol::flags::LAST;
    }

    protocol::ChunkHeader chunk_header{
        transfer_.transfer_id,
        chunk.index,
        chunk.size,
        flag
    };


    uint32_t offset = fsutils::CHUNK_SIZE * chunk.index;

    std::vector<uint8_t> data = fsutils::read_chunk(transfer_.fmeta.absolute_path, offset, chunk.size);
    if(data.empty()) {

        return;
    }
    send_chunk(chunk_header, data);
}

void Session::download_done() {
    partmeta_->delete_partial_metadata(transfer_.transfer_id);

    transfer_.partial_path = std::filesystem::path("");
    transfer_.transfer_id = UINT32_MAX;
    transfer_.fmeta = fsutils::FileMetadata{};
    transfer_.chunk_state.clear();
    transfer_.chunks.clear();
    transfer_.wrapped_dek = protocol::WrappedBlob{};
    transfer_.plaintext_hash.clear();
    transfer_.plaintext_size = 0;

    lock_.release(); // The transfer's lock, parked by upload()/download() or by a resume kickoff
    if(resuming_) { handle_resumes(); } else { state_ = SessionState::READY; }
}

void Session::download_abort(bool save, bool notify, uint8_t flag) {
    if(save) {
        partmeta_->save();
    } else {
        partmeta_->delete_partial_metadata(transfer_.transfer_id);

        transfer_.partial_path = std::filesystem::path("");
        transfer_.transfer_id = UINT32_MAX;
        transfer_.fmeta = fsutils::FileMetadata{};
        transfer_.chunk_state.clear();
        transfer_.chunks.clear();
        transfer_.wrapped_dek = protocol::WrappedBlob{};
        transfer_.plaintext_hash.clear();
        transfer_.plaintext_size = 0;
    }

    if(notify) {
        protocol::ChunkHeader chunk_header{
            transfer_.transfer_id,
            UINT32_MAX,
            0,
            flag
        };

        std::vector<uint8_t> data;

        send_chunk(chunk_header, data);
    }
    lock_.release(); // The transfer's lock, parked by upload()/download() or by a resume kickoff
    if(resuming_) { handle_resumes(); } else { state_ = SessionState::READY; }
}

void Session::download_abort_exit(bool save, bool notify, uint8_t flag) {
    if(save) {
        partmeta_->save();
    } else {
        partmeta_->delete_partial_metadata(transfer_.transfer_id);

        transfer_.partial_path = std::filesystem::path("");
        transfer_.transfer_id = UINT32_MAX;
        transfer_.fmeta = fsutils::FileMetadata{};
        transfer_.chunk_state.clear();
        transfer_.chunks.clear();
        transfer_.wrapped_dek = protocol::WrappedBlob{};
        transfer_.plaintext_hash.clear();
        transfer_.plaintext_size = 0;
    }

    if(notify) {
        protocol::ChunkHeader chunk_header{
            transfer_.transfer_id,
            UINT32_MAX,
            0,
            flag
        };

        std::vector<uint8_t> data;

        send_chunk_exit(chunk_header, data);
    }
}