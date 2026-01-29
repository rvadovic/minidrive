#include <asio.hpp>
#include <iostream>
#include <vector>
#include "client.hpp"
#include "terminalNoEcho.hpp"
#include "protocol/message.hpp"
#include "protocol/commands.hpp"
#include "protocol/codes.hpp"
#include "protocol/statuses.hpp"
#include "protocol/flags.hpp"
#include <nlohmann/json.hpp>
#include "filesystem/utils.hpp"
#include <functional>

using asio::ip::tcp;
using nlohmann::json;

Client::Client(const std::string& username, asio::io_context& io_context, std::shared_ptr<asio::executor_work_guard<asio::io_context::executor_type>> guard)
    : username_(username), 
      io_context_(io_context),
      socket_(io_context_),
      guard_(std::move(guard)),
      commands_{
          {"LIST",     [this](auto& iss){ cmd_list(iss); }},
          {"HELP",     [this](auto& iss){ cmd_help(iss); }},
          {"UPLOAD",   [this](auto& iss){ cmd_upload(iss); }},
          {"DOWNLOAD", [this](auto& iss){ cmd_download(iss); }},
          {"DELETE",   [this](auto& iss){ cmd_delete(iss); }},
          {"CD",       [this](auto& iss){ cmd_cd(iss); }},
          {"MKDIR",    [this](auto& iss){ cmd_mkdir(iss); }},
          {"RMDIR",    [this](auto& iss){ cmd_rmdir(iss); }},
          {"MOVE",     [this](auto& iss){ cmd_move(iss); }},
          {"COPY",     [this](auto& iss){ cmd_copy(iss); }},
          {"SYNC",     [this](auto& iss){ cmd_sync(iss); }},
      } {
        input_thread_ = std::thread(&Client::input_loop, this);
        setup();
    }

Client::~Client() {
    exiting_ = true;

    if(input_thread_.joinable() && std::this_thread::get_id() != input_thread_.get_id()) {
        input_thread_.join();
    }
}

void Client::connect(const std::string& host, uint16_t port) {
    tcp::resolver resolver(socket_.get_executor());
    asio::async_connect(socket_, resolver.resolve(host, std::to_string(port)),
        [this](std::error_code ec, tcp::endpoint endpoint) {
            if(!ec) {
                handle_request(username_);
                read_header_json();
            } else {
                handle_error(ec);
                return;
            }
        });
}

void Client::print(int code, const std::string& message, bool prompt) {
    if(code == protocol::codes::OK) {
        std::cout << "OK" << ": " << message << std::endl;
    } else {
        std::cout << "ERROR" << ": " << "<" << code << ">" << " " << message << std::endl;
    }
    if(prompt) {
        std::cout << "> " << std::flush;
    }
}
void Client::setup() {
    root_ = std::filesystem::path("./data/client_cwd");
    if(!fsutils::is_directory(root_)) {
        fsutils::mkdir(root_);
    }
    if(!fsutils::is_directory(root_ / "files")) {
        fsutils::mkdir(root_ / "files");
    }
    if(!fsutils::is_directory(root_ / ".partial")) {
        fsutils::mkdir(root_ / ".partial");
    }
    if(!fsutils::is_file(root_ / ".partial/partmeta.json")) {
        fsutils::create_empty_file(root_ / ".partial/partmeta.json");
    }
    partmeta_.emplace(std::filesystem::path(root_ / ".partial/partmeta.json")); // Database for partial metadata
}

void Client::input_loop() {
    std::string line;

    while (!exiting_) {
        if (!std::getline(std::cin, line)) {
            exit();
            break;
        }
        asio::post(io_context_, [this, line] {
            handle_request(line);
        });
    }
}

void Client::send_json(const json& j) {
    auto write_buffer = std::make_shared<std::string>(j.dump());

    if(write_buffer->size() > std::numeric_limits<uint32_t>::max()) {
        print(protocol::codes::INTERNAL_SERVER_ERROR, "File data was too large.");
        return;
    }

    auto len = std::make_shared<uint32_t>(htonl(write_buffer->size())); // Host to network layer

    std::vector<asio::const_buffer> buffers;
    buffers.push_back(asio::buffer(len.get(), sizeof(uint32_t)));
    buffers.push_back(asio::buffer(*write_buffer));

    asio::async_write(socket_, buffers, [this](std::error_code ec, std::size_t) {
        if(exiting_ || ec) {
            if(ec && ec != asio::error::operation_aborted) {
                handle_error(ec);
            }
            return;
        }
    });
}

void Client::send_json_exit(const json& j) {
    auto write_buffer = std::make_shared<std::string>(j.dump());

    if(write_buffer->size() > std::numeric_limits<uint32_t>::max()) {
        print(protocol::codes::INTERNAL_SERVER_ERROR, "File data was too large.");
        return;
    }

    auto len = std::make_shared<uint32_t>(htonl(write_buffer->size())); // Host to network layer

    std::vector<asio::const_buffer> buffers;
    buffers.push_back(asio::buffer(len.get(), sizeof(uint32_t)));
    buffers.push_back(asio::buffer(*write_buffer));

    asio::async_write(socket_, buffers, [this](std::error_code ec, std::size_t) {
        finish_exit();
    });
}

void Client::read_header_json() {
    auto msg_len_local = std::make_shared<uint32_t>();
    
    asio::async_read(socket_, asio::buffer(msg_len_local.get(), sizeof(uint32_t)),[this, msg_len_local](std::error_code ec, std::size_t) {
        if(exiting_ || ec) {
            if(ec && ec != asio::error::operation_aborted) {
                handle_error(ec);
            }
            return;
        }
        msg_len_ = ntohl(*msg_len_local); // Network to host layer
        buffer_.resize(msg_len_);
        read_body_json(); // Read body right after header
    });
}

void Client::read_body_json() {
    asio::async_read(socket_, asio::buffer(buffer_),[this](std::error_code ec, std::size_t) {
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

        handle_response(j); // Handle message
        read_next(); // Decide which protocol to read next
    });
}

void Client::read_header_chunk() {
    auto header = std::make_shared<protocol::ChunkHeader>();

    asio::async_read(socket_, asio::buffer(header.get(), sizeof(protocol::ChunkHeader)), [this, header](std::error_code ec, std::size_t) {
        if(exiting_ || ec) {
            if(ec && ec != asio::error::operation_aborted) {
                handle_error(ec);
            }
            return;
        }
        ch_.transfer_id = ntohl(header->transfer_id); // Network to host layer
        ch_.index = ntohl(header->index);
        ch_.size = ntohl(header->size);
        ch_.flags = header->flags;
        read_body_chunk();
    });
}

void Client::read_body_chunk() {
    auto data = std::make_shared<std::vector<uint8_t>>(ch_.size);

    asio::async_read(socket_, asio::buffer(*data), [this, data](std::error_code ec, std::size_t) {
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

void Client::send_chunk(const protocol::ChunkHeader& ch, const std::vector<uint8_t>& data) {
    auto header = std::make_shared<protocol::ChunkHeader>();
    header->transfer_id = htonl(ch.transfer_id); // Host to network layer
    header->index = htonl(ch.index);
    header->size = htonl(ch.size);
    header->flags = ch.flags;

    auto data_to_write = std::make_shared<std::vector<uint8_t>>(data);

    std::vector<asio::const_buffer> buffers;
    buffers.push_back(asio::buffer(header.get(), sizeof(protocol::ChunkHeader)));
    buffers.push_back(asio::buffer(*data_to_write));

    asio::async_write(socket_, buffers, [this, header, data_to_write](std::error_code ec, std::size_t) {
        if(exiting_ || ec) {
            if(ec && ec != asio::error::operation_aborted) {
                handle_error(ec);
            }
            return;
        }
    });
}

void Client::send_chunk_exit(const protocol::ChunkHeader& ch, const std::vector<uint8_t>& data) {
     auto header = std::make_shared<protocol::ChunkHeader>();
    header->transfer_id = htonl(ch.transfer_id); // Host to network layer
    header->index = htonl(ch.index);
    header->size = htonl(ch.size);
    header->flags = ch.flags;

    auto data_to_write = std::make_shared<std::vector<uint8_t>>(data);

    std::vector<asio::const_buffer> buffers;
    buffers.push_back(asio::buffer(header.get(), sizeof(protocol::ChunkHeader)));
    buffers.push_back(asio::buffer(*data_to_write));

    asio::async_write(socket_, buffers, [this, header, data_to_write](std::error_code ec, std::size_t) {
        finish_exit();
    });
}

void Client::read_next() {
    if(exiting_) return;
    if(state_ == ClientState::DOWNLOADING || state_ == ClientState::UPLOADING) {
        read_header_chunk();
    } else {
        read_header_json();
    }
}

void Client::on_password_required() {
    password_guard_ = std::make_unique<TerminalNoEcho>();
    std::cout << "Password for " << username_ << ": " << std::flush;
}

void Client::handle_error(const std::error_code& ec) {
    if(ec.value() != 125) {
        std::cerr << "Network error: " << ec.message() << " (" << ec.value() << ")" << std::endl;
    }
    print(protocol::codes::INTERNAL_SERVER_ERROR, ec.message());
    socket_.close();
    exit();
}

// Mostly sets state_ of client
void Client::handle_response(const json& j) {
    protocol::Response res;
    protocol::from_json(j, res); // Parse
    
    if(res.status == protocol::statuses::AUTH) {
        state_ = ClientState::AUTH;
        print(res.code, res.message, false);
        on_password_required();
        return;
    } else if (res.status == protocol::statuses::EXIT) {
        exit();
        return;
    }
    print(res.code, res.message);

    if(res.status == protocol::statuses::NEED_INPUT) {
        state_ = ClientState::NEED_INPUT;
    } 
    else if(res.status == protocol::statuses::ERROR) {
        state_ = ClientState::READY;
    } 
    else if(res.status == protocol::statuses::OK) {
        if(state_ == ClientState::UPLOAD_INIT) {
            state_ = ClientState::UPLOADING;
            upload_init();
            return;
        } 
        else if(state_ == ClientState::DOWNLOAD_INIT) {
            download_init(res.chunks, res.file_hash);
            return;
        } 
        else {
            state_ = ClientState::READY;
        }
    } else if(res.status == protocol::statuses::BUSY) {
        print(res.code, res.message, false);
        state_ = ClientState::PROCESSING;
    }
}

void Client::handle_request(const std::string& line) {
    std::cout << std::endl;
    if(line == protocol::commands::EXIT) { // Priority over other commands
        exit();
        return;
    }

    if(state_ == ClientState::AUTH) {
        password_guard_.reset();
        auth(line);

    } else if(state_ == ClientState::READY) {
        std::istringstream iss(line); // For parsing
        std::string cmd;
        iss >> cmd;

        auto it = commands_.find(cmd); // Check map

        if(it == commands_.end()) {
            print(protocol::codes::BAD_REQUEST, "Invalid command \"" + cmd + "\".");
            return;
        }

        it->second(iss); // Call command

    } else if(state_ == ClientState::NEED_INPUT) {
        need_input(line);

    } else if(state_ == ClientState::LOGIN) {
        login();

    } else if(state_ == ClientState::PROCESSING || state_ == ClientState::UPLOAD_INIT || state_ == ClientState::DOWNLOAD_INIT) {
        print(protocol::codes::SERVICE_UNAVAILABLE, "Server is busy...");
    } else if (state_ == ClientState::EXIT) {
        return;

    } else if(state_ == ClientState::UPLOADING || state_ == ClientState::DOWNLOADING) { // Waiting for finished transfer and prompting for potentional exit
        print(protocol::codes::SERVICE_UNAVAILABLE, "Transfer in progress...");
    }
}

void Client::handle_chunk(const protocol::ChunkHeader& ch, const std::vector<uint8_t>& data) {
    if(state_ == ClientState::UPLOADING) {
        if(ch.flags == protocol::flags::OK) {
            if(ch.transfer_id != transfer_.transfer_id) {
                upload_abort(false, true, protocol::flags::ERROR);
                print(protocol::codes::INTERNAL_SERVER_ERROR, "Transfer ID mismatch.");
                state_ = ClientState::READY;
                return;
            }
            transfer_.chunk_state[ch.index] = true; // Set chunk at index to sent
            partmeta_->mark_chunk_received(transfer_.transfer_id, ch.index); // Set chunk at index to sent in partial metadata database
            uploading();
        } else if (ch.flags == protocol::flags::DONE) {
            transfer_.chunk_state[ch.index] = true;
            print(protocol::codes::OK, "Upload successful");
            upload_done();
            return;
        } else if(ch.flags == protocol::flags::ERROR) {
            upload_abort(false, false, protocol::flags::ERROR);
            print(protocol::codes::INTERNAL_SERVER_ERROR, "Upload failed.");
            state_ = ClientState::READY;
            return;
        } else if(ch.flags == protocol::flags::EXIT) {
            upload_abort(true, false, protocol::flags::EXIT);
            print(protocol::codes::INTERNAL_SERVER_ERROR, "Server unavailable.", false);
            exit();
        }
    } else if (state_ == ClientState::DOWNLOADING) {
        if(ch.flags == protocol::flags::SEND) {
            if(valid_chunk(ch.index, ch.size, data)) {
                if(transfer_.transfer_id == UINT32_MAX) { // transfer_id not initilized - upload not initialized
                    transfer_.transfer_id = ch.transfer_id;
                    download_prepare_partmeta();
                }
                downloading(ch.index, ch.size, data, protocol::flags::OK);
                return;
            }
            print(protocol::codes::INTERNAL_SERVER_ERROR, "Chunk mismatch");
            download_abort(false, true, protocol::flags::CHUNK_MISMATCH);
            return;
        } else if(ch.flags == protocol::flags::LAST) {
            if(valid_chunk(ch.index, ch.size, data)) {
                if(transfer_.transfer_id == UINT32_MAX) { // transfer_id not initilized - upload not initialized
                    transfer_.transfer_id = ch.transfer_id;
                    download_prepare_partmeta();
                }
                downloading(ch.index, ch.size, data, protocol::flags::DONE);
                return;
            }
            print(protocol::codes::INTERNAL_SERVER_ERROR, "Chunk mismatch");
            download_abort(false, true, protocol::flags::CHUNK_MISMATCH);
            return;
        } else if(ch.flags == protocol::flags::ERROR) {
            print(protocol::codes::INTERNAL_SERVER_ERROR, "Download failed");
            download_abort(false, false, protocol::flags::ERROR);
            return;
        } else if(ch.flags == protocol::flags::EXIT) {
            download_abort(true, false, protocol::flags::EXIT);
            exit();
            return;
        }
    }
}

void Client::login() {
    protocol::Request req{
            protocol::commands::LOGIN,
            username_,
            "",
            0,
            ""
        };
    req.chunks.clear();
    json j;
    protocol::to_json(j, req);
    send_json(j);
}

void Client::auth(const std::string password) {
    protocol::Request req{
        protocol::commands::AUTH,
        password,
        "",
        0,
        ""
    };
    req.chunks.clear();
    json j;
    protocol::to_json(j, req);
    send_json(j);
    state_ = ClientState::PROCESSING;
}

void Client::need_input(const std::string input) {
    std::string lower_input = input;
    std::transform(lower_input.begin(), lower_input.end(), lower_input.begin(), ::tolower);
    if(!(lower_input == "y" || lower_input == "n")) {
        print(protocol::codes::BAD_REQUEST, "Invalid input.");
        return;
    }
    protocol::Request req{
            protocol::commands::NEED_INPUT,
            lower_input,
            "",
            0,
            ""
        };
    req.chunks.clear();
    json j;
    protocol::to_json(j, req);
    send_json(j);
    state_ = ClientState::PROCESSING;
}

void Client::exit() {
    bool expected = false;

    if(!exiting_.compare_exchange_strong(expected, true)) return; // Prevent multiple running exit() 

    bool socket_opened = socket_.is_open();

    if(state_ == ClientState::UPLOADING ) {                         // Notify server
        upload_abort_exit(true, socket_opened, protocol::flags::EXIT);
    } else if (state_ == ClientState::DOWNLOADING) {
        download_abort_exit(true, socket_opened, protocol::flags::EXIT);
    } else if (socket_opened) {
        protocol::Request req{
            protocol::commands::EXIT,
            "",
            "",
            0,
            ""
        };
        req.chunks.clear();
        json j;
        protocol::to_json(j, req);
        send_json_exit(j);
    }
    finish_exit();
}

void Client::finish_exit() {
    state_ = ClientState::EXIT;
    std::error_code ec;
    socket_.cancel(ec);

    if(socket_.is_open()) {
        socket_.shutdown(tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }
    guard_->reset();
    io_context_.stop();
}

void Client::cmd_help(std::istringstream& iss) {
    for (const auto& [key, value] : commands_) {
        std:: cout << key << std::endl;
    }
    print(protocol::codes::OK, "The syntax of filesystem commands is: \"Command\" \"what\" \"where\"");
}

void Client::cmd_list(std::istringstream& iss) {
    protocol::Request req{
        protocol::commands::LIST,
        "",
        "",
        0,
        ""
    };

    std::string path;
    if((iss >> path)) {
        req.first_argument = path;
    }

    req.chunks.clear();
    json j;
    protocol::to_json(j, req);
    send_json(j);
    state_ = ClientState::PROCESSING;
}

void Client::cmd_upload(std::istringstream& iss) {
    std::string local_path;
    std::string remote_path;
    if(!(iss >> local_path)) {
        print(protocol::codes::BAD_REQUEST, "Missing local path argument.");
        return;
    }
    std::cout << local_path << std::endl;
    auto local = std::filesystem::path(local_path);

    if(!fsutils::exists(local)) {
        print(protocol::codes::BAD_REQUEST, "Local file does not exist.");
        return;
    }

    fsutils::FileMetadata fmeta = fsutils::scan_file(local);

    if(fsutils::is_scan_file_error(fmeta)) {
        print(protocol::codes::INTERNAL_SERVER_ERROR, "Gathering file metadata failed.");
        return;
    }

    if(fmeta.size == 0) {
        print(protocol::codes::BAD_REQUEST, "Local file is empty.");
        return;
    }

    std::vector<protocol::ChunkInfo> chunks = fsutils::compute_chunks(fmeta);

    if(fsutils::is_compute_chunks_error(chunks)) {
        print(protocol::codes::INTERNAL_SERVER_ERROR, "Generating file chunks failed.");
        return;
    }

    transfer_.fmeta = fmeta;
    transfer_.chunks = chunks;
    transfer_.chunk_state = std::vector<bool>(chunks.size(), false);
    
    protocol::Request req{
            protocol::commands::UPLOAD,
            "",
            "",
            fmeta.size,
            fsutils::hash_to_hex(fmeta.hash),
            chunks
        };

    if((iss >> remote_path)) {
        req.first_argument = remote_path;
    } else {
        req.first_argument = local.filename();
    }

    json j;
    protocol::to_json(j, req);
    send_json(j);
    state_ = ClientState::UPLOAD_INIT;
}

void Client::cmd_download(std::istringstream& iss) {
    std::string local_path;
    std::string remote_path;
    if(!(iss >> remote_path)) {
        print(protocol::codes::BAD_REQUEST, "Missing remote path argument.");
        return;
    }

    protocol::Request req{
        protocol::commands::DOWNLOAD,
        remote_path,
        "",
        0,
        ""
    };

    std::filesystem::path local;
    if(iss >> local_path) {
        local = std::filesystem::path(local_path);
    } else {
        std::filesystem::path remote(remote_path);
        local = std::filesystem::current_path() / remote.filename();
    }

    if(fsutils::exists(local)) {
        print(protocol::codes::BAD_REQUEST, "Requested file already exists in current working directory.");
        return;
    }

    fsutils::FileMetadata fmeta{
        local,
        0,
        0,
        {}
    };

    transfer_.fmeta = fmeta;

    req.chunks.clear();
    json j;
    protocol::to_json(j, req);
    send_json(j);
    state_ = ClientState::DOWNLOAD_INIT;
}

void Client::cmd_delete(std::istringstream& iss) {
    std::string path;
    if(!(iss >> path)) {
        print(protocol::codes::BAD_REQUEST, "Missing path argument.");
        return;
    }
    protocol::Request req{
        protocol::commands::DELETE,
        path,
        "",
        0,
        ""
    };
    req.chunks.clear();
    json j;
    protocol::to_json(j, req);
    send_json(j);
    state_ = ClientState::PROCESSING;
}

void Client::cmd_cd(std::istringstream& iss) {
    std::string path;
    if(!(iss >> path)) {
        print(protocol::codes::BAD_REQUEST, "Missing path argument.");
        return;
    }
    protocol::Request req{
        protocol::commands::CD,
        path,
        "",
        0,
        ""
    };
    req.chunks.clear();
    json j;
    protocol::to_json(j, req);
    send_json(j);
    state_ = ClientState::PROCESSING;
}

void Client::cmd_mkdir(std::istringstream& iss) {
    std::string path;
    if(!(iss >> path)) {
        print(protocol::codes::BAD_REQUEST, "Missing path argument.");
        return;
    }
    protocol::Request req{
        protocol::commands::MKDIR,
        path,
        "",
        0,
        ""
    };
    req.chunks.clear();
    json j;
    protocol::to_json(j, req);
    send_json(j);
    state_ = ClientState::PROCESSING;
}

void Client::cmd_rmdir(std::istringstream& iss) {
    std::string path;
    if(!(iss >> path)) {
        print(protocol::codes::BAD_REQUEST, "Missing path argument.");
        return;
    }
    protocol::Request req{
        protocol::commands::RMDIR,
        path,
        "",
        0,
        ""
    };
    req.chunks.clear();
    json j;
    protocol::to_json(j, req);
    send_json(j);
    state_ = ClientState::PROCESSING;
}

void Client::cmd_move(std::istringstream& iss) {
    std::string source_path;
    std::string dest_path;
    if(!(iss >> source_path >> dest_path)) {
        print(protocol::codes::BAD_REQUEST, "Missing source or destination path argument.");
        return;
    }
    protocol::Request req{
        protocol::commands::MOVE,
        source_path,
        dest_path,
        0,
        ""
    };
    req.chunks.clear();
    json j;
    protocol::to_json(j, req);
    send_json(j);
    state_ = ClientState::PROCESSING;
}

void Client::cmd_copy(std::istringstream& iss) {
    std::string source_path;
    std::string dest_path;
    if(!(iss >> source_path >> dest_path)) {
        print(protocol::codes::BAD_REQUEST, "Missing source or destination path argument.");
        return;
    }
    protocol::Request req{
        protocol::commands::COPY,
        source_path,
        dest_path,
        0,
        ""
    };
    req.chunks.clear();
    json j;
    protocol::to_json(j, req);
    send_json(j);
    state_ = ClientState::PROCESSING;
}

void Client::cmd_sync(std::istringstream& iss) {
    std::string source_path;
    std::string dest_path;
    if(!(iss >> source_path >> dest_path)) {
        print(protocol::codes::BAD_REQUEST, "Missing source or destination path argument.");
        return;
    }
    //TODO check if paths exist
    //TODO get directory contents
    protocol::Request req{
        protocol::commands::SYNC,
        source_path,
        dest_path,
        0,
        ""
    };
    req.chunks.clear();
    json j;
    protocol::to_json(j, req);
    send_json(j);
    state_ = ClientState::PROCESSING;
}

void Client::upload_init() {
    transfer_.transfer_id = partmeta_->add_partial_metadata(TransferType::UPLOAD, transfer_.fmeta, transfer_.chunks, UINT32_MAX);
    uploading();
}

void Client::uploading() {
    uint32_t index;

    for(uint32_t i = 0; i < transfer_.chunk_state.size(); ++i) { // Finf first chunk that has not been sent yet
        if(!transfer_.chunk_state[i]) {
            index = i;
            break;
        }
    }
    protocol::ChunkInfo chunk = transfer_.chunks[index];
    uint8_t flag = protocol::flags::SEND;

    if(chunk.index == (transfer_.chunks.size() - 1)) { // If it is last chunk
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

void Client::upload_done() {
    partmeta_->delete_partial_metadata(transfer_.transfer_id);

    transfer_.transfer_id = UINT32_MAX;
    transfer_.fmeta = fsutils::FileMetadata{};
    transfer_.chunk_state.clear();
    transfer_.chunks.clear();

    state_ = ClientState::READY;
}

void Client::upload_abort(bool save, bool notify, uint8_t flag) {
    if(save) {
        partmeta_->save();
    } else {
        partmeta_->delete_partial_metadata(transfer_.transfer_id);

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
    state_ = ClientState::READY;
}

void Client::upload_abort_exit(bool save, bool notify, uint8_t flag) {
    if(save) {
        partmeta_->save();
    } else {
        partmeta_->delete_partial_metadata(transfer_.transfer_id);

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

bool Client::valid_file(const std::filesystem::path& partial_file, const std::array<uint8_t, crypto_generichash_BYTES>& expected) {
    std::array<uint8_t, crypto_generichash_BYTES> hash = fsutils::hash_file(partial_file);

    return hash == expected && !fsutils::is_hash_error(hash);
}

bool Client::valid_chunk(const uint32_t& index, const uint32_t& size, const std::vector<uint8_t>& data) {
    protocol::ChunkInfo chunk = transfer_.chunks[index];
    if(chunk.size != size) return false;
    if(chunk.index != index) return false;
    std::array<uint8_t, crypto_generichash_BYTES> hash = fsutils::hash_chunk(data);
    return hash == fsutils::hex_to_hash(chunk.chunk_hash) && !fsutils::is_hash_error(hash);
}

void Client::download_init(const std::vector<protocol::ChunkInfo>& chunks, const std::string& file_hash) {
    uint64_t file_size = 0;
    for(const protocol::ChunkInfo& chunk : chunks) {
        file_size += chunk.size;
    }

    if(file_size > UINT32_MAX) {
        print(protocol::codes::INTERNAL_SERVER_ERROR, "Requested file is too large.");
        download_abort(false, true, protocol::flags::ERROR);
        return;
    }

    transfer_.transfer_id = UINT32_MAX; // unitialized
    transfer_.fmeta.size = static_cast<uint32_t>(file_size);
    transfer_.fmeta.hash = fsutils::hex_to_hash(file_hash);
    transfer_.chunks = chunks;
    transfer_.chunk_state = std::vector<bool>(chunks.size(), false);
    state_ = ClientState::DOWNLOADING;
}

void Client::download_prepare_partmeta() {
    partmeta_->add_partial_metadata(TransferType::DOWNLOAD, transfer_.fmeta, transfer_.chunks, transfer_.transfer_id);

    transfer_.partial_path = partmeta_->get_partial_path(transfer_.transfer_id);

    if(transfer_.partial_path.empty()) {
        print(protocol::codes::INTERNAL_SERVER_ERROR, "Cannot get partial file");
        upload_abort(false, true, protocol::flags::ERROR);
        return;
    } 
    if(!fsutils::is_file(transfer_.partial_path)) {
        fsutils::create_empty_file(transfer_.partial_path);
    }
    return;
}

void Client::downloading(const uint32_t& index, const uint32_t& size, const std::vector<uint8_t>& data, uint8_t flag) {
    transfer_.chunk_state[index] = true;
    partmeta_->mark_chunk_received(transfer_.transfer_id, index); // Set received chunks to true

    uint32_t offset = fsutils::CHUNK_SIZE * index;
    if(!fsutils::write_chunk(transfer_.partial_path, offset, data)) {
        print(protocol::codes::INTERNAL_SERVER_ERROR, "Cannot write to file");
        flag = protocol::flags::ERROR;
        download_abort(false, true, flag);
        return;
    }
    if(flag == protocol::flags::DONE) {
        if(!valid_file(transfer_.partial_path, transfer_.fmeta.hash)) {
            print(protocol::codes::INTERNAL_SERVER_ERROR, "Downloaded file is corrupted");
            flag = protocol::flags::ERROR;
            download_abort(false, true, flag);
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
        download_done();
    }
}

void Client::download_done() {
    fsutils::move_path(transfer_.partial_path, transfer_.fmeta.absolute_path, true); // Move downloaded file to destination
    partmeta_->delete_partial_metadata(transfer_.transfer_id);

    transfer_.transfer_id = UINT32_MAX;
    transfer_.fmeta = fsutils::FileMetadata{};
    transfer_.chunk_state.clear();
    transfer_.chunks.clear();
    
    print(protocol::codes::OK, "Download successful");
    state_ = ClientState::READY;
}

void Client::download_abort(bool save, bool notify, uint8_t flag) {
    if(save) {
        partmeta_->save();
    } else {
        fsutils::remove_file(transfer_.partial_path); // Delete partial file
        partmeta_->delete_partial_metadata(transfer_.transfer_id);

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
    state_ = ClientState::READY;
}

void Client::download_abort_exit(bool save, bool notify, uint8_t flag)  {
    if(save) {
        partmeta_->save();
    } else {
        fsutils::remove_file(transfer_.partial_path); // Delete partial file
        partmeta_->delete_partial_metadata(transfer_.transfer_id);

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