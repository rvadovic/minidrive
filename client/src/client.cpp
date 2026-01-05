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

Client::Client(const std::string& username, asio::io_context& io_context)
    : username_(username), 
      io_context_(io_context),
      socket_(io_context_),
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

void Client::setup() {
    root_ = std::filesystem::path("./data/client_root");
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
        std::cout << std::endl << "File data was too large." << std::endl;
        return;
    }

    auto len = std::make_shared<uint32_t>(htonl(write_buffer->size())); // Host to network layer

    std::vector<asio::const_buffer> buffers;
    buffers.push_back(asio::buffer(len.get(), sizeof(uint32_t)));
    buffers.push_back(asio::buffer(*write_buffer));

    asio::async_write(socket_, buffers, [this](std::error_code ec, std::size_t) {
        if(ec) {
            handle_error(ec);
            return;
        }
    });
}

void Client::read_header_json() {
    asio::async_read(socket_, asio::buffer(&msg_len_, sizeof(msg_len_)),[this](std::error_code ec, std::size_t) {
        if(!ec) {
            msg_len_ = ntohl(msg_len_); // Network to host layer
            buffer_.resize(msg_len_);
            read_body_json(); // Read body right after header
        } else {
            handle_error(ec);
            return;
        }
    });
}

void Client::read_body_json() {
    asio::async_read(socket_, asio::buffer(buffer_),[this](std::error_code ec, std::size_t) {
        if(!ec) {
            std::string msg(buffer_.begin(), buffer_.end());
            json j = json::parse(msg);

            handle_response(j); // Handle message
            read_next(); // Decide which protocol to read next
        } else {
            handle_error(ec);
            return;
        }
    });
}

void Client::read_header_chunk() {
    auto header = std::make_shared<protocol::ChunkHeader>();

    asio::async_read(socket_, asio::buffer(header.get(), sizeof(protocol::ChunkHeader)), [this, header](std::error_code ec, std::size_t) {
        if(ec) {
            handle_error(ec);
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
        if(ec) {
            handle_error(ec);
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
        if(ec) {
            handle_error(ec);
            return;
        }
    });
}

void Client::read_next() {
    if(state_ == ClientState::DOWNLOADING || state_ == ClientState::UPLOADING) {
        read_header_chunk();
    } else {
        read_header_json();
    }
}

void Client::on_password_required() {
    password_guard_ = std::make_unique<TerminalNoEcho>();
    std::cout << "Password: " << std::flush;
}

void Client::handle_error(const std::error_code& ec) {
    if(ec.value() != 125) {
        std::cerr << "Network error: " << ec.message() << " (" << ec.value() << ")" << std::endl;
    }
    std::cout << "Closing client" << std::endl;
    socket_.close();
    exit();
}

// Mostly sets state_ of client
void Client::handle_response(const json& j) {
    protocol::Response res;
    protocol::from_json(j, res); // Parse
    
    if(res.status == protocol::statuses::AUTH) {
        state_ = ClientState::AUTH;
        std::cout << std::endl << res.code << ": " << res.message << std::endl;
        on_password_required();
    } 
    else if(res.status == protocol::statuses::NEED_INPUT) {
        state_ = ClientState::NEED_INPUT;
        std::cout << std::endl << res.code << ": " << res.message << std::endl;
        std::cout << "> " << std::flush;
    } 
    else if(res.status == protocol::statuses::ERROR) {
        state_ = ClientState::READY;
        std::cout << std::endl << res.code << ": " << res.message << std::endl;
        std::cout << "> " << std::flush;
    } 
    else if(res.status == protocol::statuses::OK) {
        if(state_ == ClientState::UPLOAD_INIT) {
            state_ = ClientState::UPLOADING;
            std::cout << std::endl << res.code << ": " << res.message << std::endl;
            std::cout << "> " << std::flush;
            upload_init();
            return;
        } 
        else if(state_ == ClientState::DOWNLOAD_INIT) {
            std::cout << std::endl << res.code << ": " << res.message << std::endl;
            std::cout << "> " << std::flush;
            download_init(res.chunks, res.file_hash);
            return;
        } 
        else {
            state_ = ClientState::READY;
        }
        std::cout << std::endl << res.code << ": " << res.message << std::endl;
        std::cout << "> " << std::flush;
    } 
    else if(res.status == protocol::statuses::CONFLICT) {
        std::cout << std::endl << res.code << ": " << res.message << std::endl;
    } 
    else if(res.status == protocol::statuses::BUSY) {
        state_ = ClientState::PROCESSING;
        std::cout << std::endl << "Server is busy." << std::endl;
    } else if (res.status == protocol::statuses::EXIT) {
        std::cout << std::endl << "Server unavailable" << std::endl;
        exit();
    }
}

void Client::handle_request(const std::string& line) {
    if(line == protocol::commands::EXIT) { // Priority over other commands
        exit();

    } else if(state_ == ClientState::AUTH) {
        password_guard_.reset();
        auth(line);

    } else if(state_ == ClientState::READY) {
        std::istringstream iss(line); // For parsing
        std::string cmd;
        iss >> cmd;

        auto it = commands_.find(cmd); // Check map

        if(it == commands_.end()) {
            std::cout << protocol::codes::BAD_REQUEST << ": Invalid command \""<< cmd << "\"." << std::endl;
            std::cout << "> " << std::flush;
            return;
        }

        it->second(iss); // Call command

    } else if(state_ == ClientState::NEED_INPUT) {
        need_input(line);

    } else if(state_ == ClientState::LOGIN) {
        login();

    } else if(state_ == ClientState::PROCESSING || state_ == ClientState::UPLOAD_INIT || state_ == ClientState::DOWNLOAD_INIT) {
        std::cout << std::endl << "Waiting for server..." << std::endl;
        std::cout << "> " << std::flush;

    } else if (state_ == ClientState::EXIT) {
        return;

    } else if(state_ == ClientState::UPLOADING || state_ == ClientState::DOWNLOADING) { // Waiting for finished transfer and prompting for potentional exit
        std::cout << std::endl << "Transeferring files..." << std::endl;
        std::cout << "> " << std::flush;
    }
}

void Client::handle_chunk(const protocol::ChunkHeader& ch, const std::vector<uint8_t>& data) {
    if(state_ == ClientState::UPLOADING) {
        if(ch.flags == protocol::flags::OK) {
            if(ch.transfer_id != transfer_.transfer_id) {
                upload_abort(false, true, protocol::flags::ERROR);
                std::cout << std::endl << protocol::codes::INTERNAL_SERVER_ERROR << ": Transfer ID mismatch." << std::endl;
                state_ = ClientState::READY;
                std::cout << "> " << std::flush;
                return;
            }
            transfer_.chunk_state[ch.index] = true; // Set chunk at index to sent
            partmeta_->mark_chunk_received(transfer_.transfer_id, ch.index); // Set chunk at index to sent in partial metadata database
            uploading();
        } else if (ch.flags == protocol::flags::DONE) {
            transfer_.chunk_state[ch.index] = true;
            std::cout << std::endl << protocol::codes::OK << ": Upload succesful" << std::endl;
            std::cout << "> " << std::flush;
            upload_done();
            return;
        } else if(ch.flags == protocol::flags::ERROR) {
            upload_abort(false, false, protocol::flags::ERROR);
            std::cout << std::endl << protocol::codes::INTERNAL_SERVER_ERROR << ": Upload failed." << std::endl;
            state_ = ClientState::READY;
            std::cout << "> " << std::flush;
            return;
        } else if(ch.flags == protocol::flags::EXIT) {
            upload_abort(true, false, protocol::flags::EXIT);
            std::cout << std::endl << protocol::codes::INTERNAL_SERVER_ERROR << ": Server unavailable" << std::endl;
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
            std::cout << std::endl << protocol::codes::INTERNAL_SERVER_ERROR << ": Chunk mismatch" << std::endl;
            std::cout << "> " << std::flush;
            download_abort(false, true, protocol::flags::CHUNK_MISMATCH);
            return;
        } else if(ch.flags == protocol::flags::LAST) {
            if(valid_chunk(ch.index, ch.size, data)) {
                downloading(ch.index, ch.size, data, protocol::flags::DONE);
                return;
            }
            std::cout << std::endl << protocol::codes::INTERNAL_SERVER_ERROR << ": Chunk mismatch" << std::endl;
            std::cout << "> " << std::flush;
            download_abort(false, true, protocol::flags::CHUNK_MISMATCH);
            return;
        } else if(ch.flags == protocol::flags::ERROR) {
            std::cout << std::endl << protocol::codes::INTERNAL_SERVER_ERROR << ": Download failed" << std::endl;
            std::cout << "> " << std::flush;
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
    if(!(input == "Y" || input == "n")) {
        std::cout << protocol::codes::BAD_REQUEST << ": Invalid input." << std::endl;
        std::cout << "> " << std::flush;
        return;
    }
    protocol::Request req{
            protocol::commands::NEED_INPUT,
            input,
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
        upload_abort(true, socket_opened, protocol::flags::EXIT);
    } else if (state_ == ClientState::DOWNLOADING) {
        download_abort(true, socket_opened, protocol::flags::EXIT);
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
        send_json(j);
    }
    
    asio::post(io_context_, [this]() {  // Post to io_context thread
        std::error_code ec;
        if(socket_.is_open()) {
            socket_.shutdown(tcp::socket::shutdown_both, ec);
            socket_.close(ec);
        }

        state_ = ClientState::EXIT;
    });
    io_context_.stop();

    /*if(on_exit) {
        std::cout << "on_exit" << std::endl;
        on_exit_();
    }*/
}

void Client::cmd_help(std::istringstream& iss) {
    for (const auto& [key, value] : commands_) {
        std:: cout << key << std::endl;
    }
    std::cout << "The syntax of filesystem commands is: \"Command\" \"what\" \"where\"." << std::endl;
    std::cout << "> " << std::flush;
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
        std::cout << std::endl << protocol::codes::BAD_REQUEST << ": Missing local path argument." << std::endl;
        std::cout << "> " << std::flush;
        return;
    }
    std::cout << local_path << std::endl;
    auto local = std::filesystem::path(local_path);

    if(!fsutils::exists(local)) {
        std::cout << std::endl << protocol::codes::BAD_REQUEST << ": Local file does not exist." << std::endl;
        std::cout << "> " << std::flush;
        return;
    }

    fsutils::FileMetadata fmeta = fsutils::scan_file(local);

    if(fsutils::is_scan_file_error(fmeta)) {
        std::cout << std::endl << protocol::codes::INTERNAL_SERVER_ERROR << ": Gathering file metadata failed." << std::endl;
        std::cout << "> " << std::flush;
        return;
    }

    if(fmeta.size == 0) {
        std::cout << std::endl << protocol::codes::BAD_REQUEST << ": Local file is empty." << std::endl;
        std::cout << "> " << std::flush;
        return;
    }

    std::vector<protocol::ChunkInfo> chunks = fsutils::compute_chunks(fmeta);

    if(fsutils::is_compute_chunks_error(chunks)) {
        std::cout << std::endl << protocol::codes::INTERNAL_SERVER_ERROR << ": Generating file chunks failed." << std::endl;
        std::cout << "> " << std::flush;
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
        std::cout << std::endl << protocol::codes::BAD_REQUEST << ": Missing remote path argument." << std::endl;
        std::cout << "> " << std::flush;
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
        if(!fsutils::exists(local)) {
            std::cout << protocol::codes::BAD_REQUEST << ": Local file does not exist." << std::endl;
            std::cout << "> " << std::flush;
        }
    } else {
        local = std::filesystem::current_path();
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
        std::cout << protocol::codes::BAD_REQUEST << ": Missing path argument." << std::endl;
        std::cout << "> " << std::flush;
        return;
    }
    protocol::Request req{
        protocol::commands::LIST,
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
        std::cout << protocol::codes::BAD_REQUEST << ": Missing path argument." << std::endl;
        std::cout << "> " << std::flush;
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
        std::cout << protocol::codes::BAD_REQUEST << ": Missing path argument." << std::endl;
        std::cout << "> " << std::flush;
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
        std::cout << protocol::codes::BAD_REQUEST << ": Missing path argument." << std::endl;
        std::cout << "> " << std::flush;
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
        std::cout << protocol::codes::BAD_REQUEST << ": Missing source or destination path argument." << std::endl;
        std::cout << "> " << std::flush;
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
        std::cout << protocol::codes::BAD_REQUEST << ": Missing source or destination path argument." << std::endl;
        std::cout << "> " << std::flush;
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
        std::cout << protocol::codes::BAD_REQUEST << ": Missing source or destination path argument." << std::endl;
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

bool Client::valid_file(const std::filesystem::path& partial_file, const std::array<uint8_t, crypto_generichash_BYTES>& expected) {
    std::array<uint8_t, crypto_generichash_BYTES> hash = fsutils::hash_file(partial_file);
    std::cout << fsutils::hash_to_hex(hash) << " =? " << fsutils::hash_to_hex(expected) << std::endl;

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
        std::cout << std::endl << protocol::codes::PRECONDITION_FAILED << ": Requested file is too large" << std::endl;
        std::cout << "> " << std::flush;
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
        std::cout << std::endl << protocol::codes::INTERNAL_SERVER_ERROR << ": Cannot get partial file" << std::endl;
        std::cout << "> " << std::flush;
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
        std::cout << std::endl << protocol::codes::INTERNAL_SERVER_ERROR << ": Cannot write to file" << std::endl;
        std::cout << "> " << std::flush;
        flag = protocol::flags::ERROR;
        download_abort(false, true, flag);
        return;
    }
    if(flag == protocol::flags::DONE) {
        if(!valid_file(transfer_.partial_path, transfer_.fmeta.hash)) {
            std::cout << std::endl << protocol::codes::INTERNAL_SERVER_ERROR << ": Invalid chunk" << std::endl;
            std::cout << "> " << std::flush;
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
    
    std::cout << std::endl << protocol::codes::OK << ": Download succesful" << std::endl;
    std::cout << "> " << std::flush;
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