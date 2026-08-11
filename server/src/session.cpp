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

Session::Session(tcp::socket socket, std::shared_ptr<Storage> storage, std::function<void(std::shared_ptr<Session>)> on_exit) 
    : socket_(std::move(socket)),
      storage_(storage),
      on_exit_(on_exit),
      root_(storage->get_root()),
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
          {protocol::commands::LOGIN,     [this](auto& req){ login(req); }},
          {protocol::commands::NEED_INPUT, [this](auto& req){ need_input(req); }},
          {protocol::commands::AUTH, [this](auto& req){ auth(req); }}
      } {
        transfer_.transfer_id = UINT32_MAX;
}

void Session::start() {
    read_header_json();
}

void Session::read_header_json() {
    auto self = shared_from_this();
    auto msg_len_local = std::make_shared<uint32_t>();

    asio::async_read(socket_, asio::buffer(msg_len_local.get(), sizeof(uint32_t)),[this, self, msg_len_local](std::error_code ec, std::size_t) {
        if(exiting_ || ec) {
            if(ec && ec != asio::error::operation_aborted) {
                handle_error(ec);
            }
            return;
        }
        msg_len_ = ntohl(*msg_len_local);
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
    asio::async_read(socket_, asio::buffer(buffer_),[this, self](std::error_code ec, std::size_t) {
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
        handle_request(j);
        read_next();
    });
}

void Session::write_response_json(const json& j) {
    auto self = shared_from_this();

    auto write_buffer = std::make_shared<std::string>(j.dump());

    if(write_buffer->size() > std::numeric_limits<uint32_t>::max()) {
        spdlog::error("[{}] Response body too large to send ({} bytes)", username_, write_buffer->size());
        return;
    }

    auto response_len = std::make_shared<uint32_t>(htonl(write_buffer->size()));

    std::vector<asio::const_buffer> buffers;
    buffers.push_back(asio::buffer(response_len.get(), sizeof(uint32_t)));
    buffers.push_back(asio::buffer(*write_buffer));

    asio::async_write(socket_, buffers, [this, self, write_buffer, response_len](std::error_code ec, std::size_t) {
        if(ec) {
            handle_error(ec);
            return;
        }
    });
}

void Session::write_response_json_exit(const json& j) {
    auto self = shared_from_this();

    auto write_buffer = std::make_shared<std::string>(j.dump());

    if(write_buffer->size() > std::numeric_limits<uint32_t>::max()) {
        spdlog::error("[{}] Response body too large to send ({} bytes)", username_, write_buffer->size());
        return;
    }

    auto response_len = std::make_shared<uint32_t>(htonl(write_buffer->size()));

    std::vector<asio::const_buffer> buffers;
    buffers.push_back(asio::buffer(response_len.get(), sizeof(uint32_t)));
    buffers.push_back(asio::buffer(*write_buffer));
    
    asio::async_write(socket_, buffers, [this, self, write_buffer, response_len](std::error_code ec, std::size_t) {
        finish_exit();
    });
}

void Session::read_header_chunk() {
    auto self = shared_from_this();

    auto header = std::make_shared<protocol::ChunkHeader>();

    asio::async_read(socket_, asio::buffer(header.get(), sizeof(protocol::ChunkHeader)), [this, self, header](std::error_code ec, std::size_t) {
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
        read_body_chunk();
    });
}

void Session::read_body_chunk() {
    auto self = shared_from_this();

    auto data = std::make_shared<std::vector<uint8_t>>(ch_.size);

    asio::async_read(socket_, asio::buffer(*data), [this, self, data](std::error_code ec, std::size_t) {
        if(exiting_ || ec) {
            if(ec && ec != asio::error::operation_aborted) {
                handle_error(ec);
            }
            return;
        }
        handle_chunk(ch_, *data);
        read_next();
    });
}

void Session::send_chunk(const protocol::ChunkHeader& ch, const std::vector<uint8_t>& data) {
    auto self = shared_from_this();

    auto header = std::make_shared<protocol::ChunkHeader>();
    header->transfer_id = htonl(ch.transfer_id);
    header->index = htonl(ch.index);
    header->size = htonl(ch.size);
    header->flags = ch.flags;

    auto data_to_write = std::make_shared<std::vector<uint8_t>>(data);

    std::vector<asio::const_buffer> buffers;
    buffers.push_back(asio::buffer(header.get(), sizeof(protocol::ChunkHeader)));
    buffers.push_back(asio::buffer(*data_to_write));

    asio::async_write(socket_, buffers, [this, self, header, data_to_write](std::error_code ec, std::size_t) {
        if(ec) {
            handle_error(ec);
            return;
        }
    });
}

void Session::send_chunk_exit(const protocol::ChunkHeader& ch, const std::vector<uint8_t>& data) {
    auto self = shared_from_this();

    auto header = std::make_shared<protocol::ChunkHeader>();
    header->transfer_id = htonl(ch.transfer_id);
    header->index = htonl(ch.index);
    header->size = htonl(ch.size);
    header->flags = ch.flags;

    auto data_to_write = std::make_shared<std::vector<uint8_t>>(data);

    std::vector<asio::const_buffer> buffers;
    buffers.push_back(asio::buffer(header.get(), sizeof(protocol::ChunkHeader)));
    buffers.push_back(asio::buffer(*data_to_write));

    asio::async_write(socket_, buffers, [this, self, header, data_to_write](std::error_code ec, std::size_t) {
        finish_exit();
    });
}

void Session::send_res(protocol::Response& res) {
    spdlog::debug("[{}] -> {} {} {}", username_, res.status, res.code, res.message);
    res.chunks.clear();
    json j;
    protocol::to_json(j, res);
    write_response_json(j);
}

void Session::read_next() {
    if(exiting_) return;
    if(state_ == SessionState::UPLOADING || state_ == SessionState::DOWNLOADING) {
        read_header_chunk();
    } else {
        read_header_json();
    }
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
        if(ch.flags == protocol::flags::SEND) {
            if(valid_chunk(ch.index, ch.size, data)) {
                if(transfer_.transfer_id == UINT32_MAX) { // transfer_id not initilized -> upload not initialized
                    transfer_.transfer_id = ch.transfer_id;
                    upload_init();
                }
                uploading(ch.index, ch.size, data, protocol::flags::OK);
                return;
            }
            spdlog::warn("[{}] Invalid chunk received (index {}).", username_, ch.index);
            upload_abort(false, true, protocol::flags::CHUNK_MISMATCH);
            return;
        } else if(ch.flags == protocol::flags::LAST) {
            if(transfer_.transfer_id == UINT32_MAX) { // transfer_id not initilized -> upload not initialized
                transfer_.transfer_id = ch.transfer_id;
                upload_init();
            }
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
        if(db_->validate_user(username_, req.first_argument)) {
            spdlog::info("[{}] Authentication succeeded.", username_);
            if(!setup_dir()) return; // setup_dir() already answered with the error
            state_ = SessionState::READY;

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
            spdlog::warn("[{}] Authentication failed: invalid password.", username_);
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
        db_->add_user(username_, req.first_argument, storage_->get_default_tier());
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

                if(!storage_->try_acquire_user_lock(username_)) {
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

                protocol::Response res {
                    protocol::statuses::RESUME,
                    protocol::codes::OK,
                    to_string(entry.type) + " " + fsutils::relative(user_dir_, entry.absolute_path).string(),
                    std::to_string(entry.id) // reuse file_hash field to carry the transfer id being resumed
                };
                send_res(res);

                if(entry.type == TransferType::UPLOAD) {
                    state_ = SessionState::UPLOADING; // client drives sending; handle_chunk() already skips upload_init() since transfer_id != UINT32_MAX
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
                storage_->release_user_lock(username_);
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

    bool socket_opened = socket_.is_open();

    if(state_ == SessionState::UPLOADING ) {
        upload_abort_exit(true, socket_opened, protocol::flags::EXIT);
        return;
    } else if (state_ == SessionState::DOWNLOADING) {
        download_abort_exit(true, socket_opened, protocol::flags::EXIT);
        return;
    } else if (socket_opened) {
        protocol::Response res{
            protocol::statuses::EXIT,

            
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

    storage_->release_user_lock(username_);

    std::error_code ec;
    socket_.shutdown(tcp::socket::shutdown_both, ec);
    socket_.close(ec);

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
    if(!storage_->try_acquire_user_lock(username_)) {
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
            storage_->release_user_lock(username_);
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
        storage_->release_user_lock(username_);
        return;
    }else {
        std::filesystem::path requested_dir = fsutils::resolve_path(user_dir_, current_dir_, req.first_argument);
        if(!fsutils::is_directory(requested_dir)) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::BAD_REQUEST,
                "Directory does not exist.",
                ""
            };
            send_res(res);
            storage_->release_user_lock(username_);
            return;
        }
        if(!fsutils::is_subpath(user_dir_, requested_dir)) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::FORBIDDEN,
                "Access denied.",
                ""
            };
            send_res(res);
            storage_->release_user_lock(username_);
            return;
        }
        std::vector<fsutils::FileMetadata> files = fsutils::scan_directory(requested_dir, false);
        if(fsutils::is_scan_dir_error(files)) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::INTERNAL_SERVER_ERROR,
                "Failed to list directory",
                ""
            };
            send_res(res);
            storage_->release_user_lock(username_);
            return;
        }
        std::string file_list;
        for (const auto& file : files) {
            std::filesystem::path relative = fsutils::relative(requested_dir, file.absolute_path);
            file_list += relative.string() + "\n";
        }
        protocol::Response res {
            protocol::statuses::OK,
            protocol::codes::OK,
            file_list,
            ""
        };
        send_res(res);
        storage_->release_user_lock(username_);
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
    if(!storage_->try_acquire_user_lock(username_)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Server is busy",
            ""
        };
        send_res(res);
        return;
    }
    std::filesystem::path requested_file = fsutils::resolve_path(user_dir_, current_dir_, req.first_argument);
    if(!fsutils::is_file(requested_file)) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::BAD_REQUEST,
                "File does not exist.",
                ""
            };
            send_res(res);
            storage_->release_user_lock(username_);
            return;
        }
        if(!fsutils::is_subpath(user_dir_, requested_file)) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::FORBIDDEN,
                "Access denied.",
                ""
            };
            send_res(res);
            storage_->release_user_lock(username_);
            return;
        }
        if(!fsutils::remove_file(requested_file)) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::INTERNAL_SERVER_ERROR,
                "Cannot remove file",
                ""
            };
            send_res(res);
            storage_->release_user_lock(username_);
            return;
        }
        protocol::Response res {
            protocol::statuses::OK,
            protocol::codes::OK,
            "File deleted",
            ""
        };
        send_res(res);
        storage_->release_user_lock(username_);
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
    if(!storage_->try_acquire_user_lock(username_)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Server is busy",
            ""
        };
        send_res(res);
        return;
    }
    std::filesystem::path requested_file = fsutils::resolve_path(user_dir_, current_dir_, req.first_argument);
    if(fsutils::is_file(requested_file)) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::PRECONDITION_FAILED,
                "File already exists.",
                ""
            };
            send_res(res);
            storage_->release_user_lock(username_);
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
            storage_->release_user_lock(username_);
            return;
    }
    if(!fsutils::is_subpath(user_dir_, requested_file)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::FORBIDDEN,
            "Access denied.",
            ""
        };
        send_res(res);
        storage_->release_user_lock(username_);
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
        storage_->release_user_lock(username_);
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
    transfer_.chunk_state = transfer_.chunk_state = std::vector<bool>(req.chunks.size(), false);

    protocol::Response res {
        protocol::statuses::OK,
        protocol::codes::OK,
        "Starting upload to file: " + fsutils::relative(user_dir_, requested_file).string(),
        ""
    };
    send_res(res);
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
    if(!storage_->try_acquire_user_lock(username_)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Server is busy",
            ""
        };
        send_res(res);
        return;
    }
    std::filesystem::path requested_file = fsutils::resolve_path(user_dir_, current_dir_, req.first_argument);
    if(!fsutils::is_file(requested_file)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::PRECONDITION_FAILED,
            "File does not exist.",
            ""
        };
        send_res(res);
        storage_->release_user_lock(username_);
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
        storage_->release_user_lock(username_);
        return;
    }
    if(!fsutils::is_subpath(user_dir_, requested_file)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::FORBIDDEN,
            "Access denied.",
            ""
        };
        send_res(res);
        storage_->release_user_lock(username_);
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
        storage_->release_user_lock(username_);
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
        storage_->release_user_lock(username_);
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
        storage_->release_user_lock(username_);
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
        storage_->release_user_lock(username_);
        return;
    }

    protocol::Response res {
        protocol::statuses::OK,
        protocol::codes::OK,
        "Starting download.",
        fsutils::hash_to_hex(transfer_.fmeta.hash)
    };
    res.chunks = transfer_.chunks;
    json j;
    protocol::to_json(j, res);
    write_response_json(j);
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
    std::filesystem::path requested_dir = fsutils::resolve_path(user_dir_, current_dir_, req.first_argument);
    if(!fsutils::is_directory(requested_dir)) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::BAD_REQUEST,
                "Directory does not exist.",
                ""
            };
            send_res(res);
            return;
        }
        if(!fsutils::is_subpath(user_dir_, requested_dir)) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::FORBIDDEN,
                "Access denied.",
                ""
            };
            send_res(res);
            return;
        }
        current_dir_ = requested_dir;
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
    if(!storage_->try_acquire_user_lock(username_)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Server is busy",
            ""
        };
        send_res(res);
        return;
    }
    std::filesystem::path requested_dir = fsutils::resolve_path(user_dir_, current_dir_, req.first_argument);
    if(fsutils::is_directory(requested_dir)) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::PRECONDITION_FAILED,
                "Directory already exists.",
                ""
            };
            send_res(res);
            storage_->release_user_lock(username_);
            return;
        }
    if(!fsutils::is_subpath(user_dir_, requested_dir)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::FORBIDDEN,
            "Access denied.",
            ""
        };
        send_res(res);
        storage_->release_user_lock(username_);
        return;
    }
    if(!fsutils::mkdir(requested_dir)) {
        protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::INTERNAL_SERVER_ERROR,
                "Cannot create directory.",
                ""
            };
            send_res(res);
            storage_->release_user_lock(username_);
            return;
    }
    protocol::Response res {
            protocol::statuses::OK,
            protocol::codes::OK,
            "Directory created.",
            ""
        };
        send_res(res);
        storage_->release_user_lock(username_);
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
    if(!storage_->try_acquire_user_lock(username_)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Server is busy",
            ""
        };
        send_res(res);
        return;
    }
    std::filesystem::path requested_dir = fsutils::resolve_path(user_dir_, current_dir_, req.first_argument);
    if(!fsutils::is_directory(requested_dir)) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::BAD_REQUEST,
                "Directory does no  exist.",
                ""
            };
            send_res(res);
            storage_->release_user_lock(username_);
            return;
        }
    if(!fsutils::is_subpath(user_dir_, requested_dir) || user_dir_ == requested_dir) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::FORBIDDEN,
            "Access denied.",
            ""
        };
        send_res(res);
        storage_->release_user_lock(username_);
        return;
    }
    if(!fsutils::rmdir(requested_dir)) {
        protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::INTERNAL_SERVER_ERROR,
                "Cannot delete directory.",
                ""
            };
            send_res(res);
            storage_->release_user_lock(username_);
            return;
    }
    protocol::Response res {
            protocol::statuses::OK,
            protocol::codes::OK,
            "Directory deleted.",
            ""
        };
        send_res(res);
        storage_->release_user_lock(username_);
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
    if(!storage_->try_acquire_user_lock(username_)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Server is busy",
            ""
        };
        send_res(res);
        return;
    }
    std::filesystem::path requested_src = fsutils::resolve_path(user_dir_, current_dir_, req.first_argument);
    std::filesystem::path requested_dst = fsutils::resolve_path(user_dir_, current_dir_, req.second_argument);
    if(!(fsutils::is_directory(requested_src) || fsutils::is_file(requested_src))) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::PRECONDITION_FAILED,
                "Incorrect paths.",
                ""
            };
            send_res(res);
            storage_->release_user_lock(username_);
            return;
    }
    if(fsutils::is_directory(requested_dst) || fsutils::is_file(requested_dst)) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::PRECONDITION_FAILED,
                "Destination path already exists.",
                ""
            };
            send_res(res);
            storage_->release_user_lock(username_);
            return;
    }
    if(!fsutils::is_subpath(user_dir_, requested_src) || !fsutils::is_subpath(user_dir_, requested_dst)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::FORBIDDEN,
            "Access denied.",
            ""
        };
        send_res(res);
        storage_->release_user_lock(username_);
        return;
    }
    if(!(fsutils::move_path(requested_src ,requested_dst, false))) {
        protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::INTERNAL_SERVER_ERROR,
                "Cannot move paths.",
                ""
            };
            send_res(res);
            storage_->release_user_lock(username_);
            return;
    }
    protocol::Response res {
            protocol::statuses::OK,
            protocol::codes::OK,
            "Path moved.",
            ""
        };
        send_res(res);
        storage_->release_user_lock(username_);
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
    if(!storage_->try_acquire_user_lock(username_)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Server is busy",
            ""
        };
        send_res(res);
        return;
    }
    std::filesystem::path requested_src = fsutils::resolve_path(user_dir_, current_dir_, req.first_argument);
    std::filesystem::path requested_dst = fsutils::resolve_path(user_dir_, current_dir_, req.second_argument);
    if(!(fsutils::is_directory(requested_src) || fsutils::is_file(requested_src))) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::PRECONDITION_FAILED,
                "Incorrect paths.",
                ""
            };
            send_res(res);
            storage_->release_user_lock(username_);
            return;
    }
    if(fsutils::is_directory(requested_dst) || fsutils::is_file(requested_dst)) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::PRECONDITION_FAILED,
                "Destination path already exists.",
                ""
            };
            send_res(res);
            storage_->release_user_lock(username_);
            return;
    }
    if(!fsutils::is_subpath(user_dir_, requested_src) || !fsutils::is_subpath(user_dir_, requested_dst)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::FORBIDDEN,
            "Access denied.",
            ""
        };
        send_res(res);
        storage_->release_user_lock(username_);
        return;
    }
    if(!(fsutils::copy_path(requested_src, requested_dst, false))) {
        protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::INTERNAL_SERVER_ERROR,
                "Cannot copy paths.",
                ""
            };
            send_res(res);
            storage_->release_user_lock(username_);
            return;
    }
    protocol::Response res {
            protocol::statuses::OK,
            protocol::codes::OK,
            "Path copied.",
            ""
        };
        send_res(res);
        storage_->release_user_lock(username_);
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
    if(!storage_->try_acquire_user_lock(username_)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Server is busy",
            ""
        };
        send_res(res);
        return;
    }
    std::filesystem::path requested_dir = fsutils::resolve_path(user_dir_, current_dir_, req.first_argument);
    if(!fsutils::is_subpath(user_dir_, requested_dir)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::FORBIDDEN,
            "Access denied.",
            ""
        };
        send_res(res);
        storage_->release_user_lock(username_);
        return;
    }
    if(!fsutils::is_directory(requested_dir)) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::PRECONDITION_FAILED,
            "Remote sync directory does not exist.",
            ""
        };
        send_res(res);
        storage_->release_user_lock(username_);
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
        storage_->release_user_lock(username_);
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
        res.files.push_back(protocol::FileEntry{
            fsutils::relative(requested_dir, file.absolute_path).generic_string(),
            is_dir ? 0u : file.size,
            is_dir ? std::string() : fsutils::hash_to_hex(file.hash),
            file.last_modified,
            is_dir
        });
    }

    send_res(res);
    storage_->release_user_lock(username_);
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
    if(!storage_->try_acquire_user_lock(username_)) {
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
    state_ = SessionState::NEED_INPUT_SET_TIER;
}

void Session::finish_set_tier() {
    // Reached only from need_input()'s "y" branch, which holds the user lock taken by set_tier()
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
        storage_->release_user_lock(username_);
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
        storage_->release_user_lock(username_);
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
        storage_->release_user_lock(username_);
        return;
    }

    // Re-point user_dir_, current_dir_ and partmeta_ at the new medium
    if(!setup_dir()) {
        storage_->release_user_lock(username_);
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
    storage_->release_user_lock(username_);
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

void Session::upload_init() {
    partmeta_->add_partial_metadata(TransferType::UPLOAD, transfer_.fmeta, transfer_.chunks, transfer_.transfer_id);
    transfer_.partial_path = partmeta_->get_partial_path(transfer_.transfer_id);
    spdlog::debug("[{}] Upload {} -> partial file {}", username_, transfer_.transfer_id, transfer_.partial_path.string());
    if(transfer_.partial_path.empty()) {
        spdlog::error("[{}] Failed to get partial path for upload {}.", username_, transfer_.transfer_id);
        upload_abort(false, true, protocol::flags::ERROR);
        return;
    }
    if(!fsutils::is_file(transfer_.partial_path)) {
        fsutils::create_empty_file(transfer_.partial_path);
    }
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
    fsutils::move_path(transfer_.partial_path, transfer_.fmeta.absolute_path, true);
    partmeta_->delete_partial_metadata(transfer_.transfer_id);

    transfer_.partial_path = std::filesystem::path("");
    transfer_.transfer_id = UINT32_MAX;
    transfer_.fmeta = fsutils::FileMetadata{};
    transfer_.chunk_state.clear();
    transfer_.chunks.clear();

    storage_->release_user_lock(username_);
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

    storage_->release_user_lock(username_);
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

    storage_->release_user_lock(username_);
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
    storage_->release_user_lock(username_);
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