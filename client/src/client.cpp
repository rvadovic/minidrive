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
#include <sstream>

using asio::ip::tcp;
using nlohmann::json;

Client::Client(const std::string& username, asio::io_context& io_context, std::shared_ptr<asio::executor_work_guard<asio::io_context::executor_type>> guard)
    : username_(username), 
      io_context_(io_context),
      socket_(io_context_),
      input_(io_context_, ::dup(STDIN_FILENO)),
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
          {"UPLOAD_DIR",   [this](auto& iss){ cmd_upload_dir(iss); }},
          {"DOWNLOAD_DIR", [this](auto& iss){ cmd_download_dir(iss); }},
      } {
        is_tty_ = ::isatty(STDIN_FILENO) != 0;
        current_prompt_ = PROMPT;
        if(is_tty_) {
            raw_guard_ = std::make_unique<TerminalRaw>();
        }
        setup();
    }

Client::~Client() {
    exiting_ = true;
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
        std::cout << current_prompt_ << std::flush;
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

void Client::read_line() {
    if(exiting_ || reading_line_) return;

    reading_line_ = true;

    if(is_tty_) {
        read_char();
        return;
    }

    asio::async_read_until(input_, asio::dynamic_buffer(input_buffer_), '\n',[this](std::error_code ec, std::size_t length) {
        reading_line_ = false;
        if(!ec) {
            std::string line = input_buffer_.substr(0, length - 1); // remove '\n'
            input_buffer_.erase(0, length);

            asio::post(io_context_, [this, line = std::move(line)] {
                handle_request(line);
            });


        } else if(ec != asio::error::operation_aborted) {
            std::cerr << "Input error: " << ec.message() << "\n";
            exit();
        }
    });
}

void Client::read_char() {
    asio::async_read(input_, asio::buffer(&char_buf_, 1), [this](std::error_code ec, std::size_t /*length*/) {
        if(ec) {
            reading_line_ = false;
            if(ec != asio::error::operation_aborted) {
                std::cerr << "Input error: " << ec.message() << "\n";
                exit();
            }
            return;
        }
        process_char(char_buf_);
    });
}

void Client::refresh_line() {
    std::string out = "\r\x1b[K" + current_prompt_;
    if(!masked_) {
        out += line_buffer_;
        size_t back = line_buffer_.size() - cursor_;
        if(back > 0) {
            out += "\x1b[" + std::to_string(back) + "D";
        }
    }
    std::cout << out << std::flush;
}

void Client::history_prev() {
    if(masked_ || history_.empty()) return;
    if(history_pos_ == history_.size()) {
        history_saved_ = line_buffer_;
    }
    if(history_pos_ > 0) {
        history_pos_--;
        line_buffer_ = history_[history_pos_];
        cursor_ = line_buffer_.size();
        refresh_line();
    }
}

void Client::history_next() {
    if(masked_ || history_pos_ >= history_.size()) return;
    history_pos_++;
    line_buffer_ = (history_pos_ == history_.size()) ? history_saved_ : history_[history_pos_];
    cursor_ = line_buffer_.size();
    refresh_line();
}

void Client::handle_csi_final(const std::string& params, char final_byte) {
    if(!params.empty() && params.front() == 'O') { // ESC O <letter> (Home/End on some terminals)
        if(final_byte == 'H') { cursor_ = 0; refresh_line(); }
        else if(final_byte == 'F') { cursor_ = line_buffer_.size(); refresh_line(); }
        return;
    }

    switch(final_byte) {
        case 'A': history_prev(); break;
        case 'B': history_next(); break;
        case 'C': if(cursor_ < line_buffer_.size()) { cursor_++; refresh_line(); } break;
        case 'D': if(cursor_ > 0) { cursor_--; refresh_line(); } break;
        case 'H': cursor_ = 0; refresh_line(); break;
        case 'F': cursor_ = line_buffer_.size(); refresh_line(); break;
        case '~':
            if(params == "3" && cursor_ < line_buffer_.size()) { // Delete
                line_buffer_.erase(cursor_, 1);
                refresh_line();
            } else if(params == "1") { // Home
                cursor_ = 0;
                refresh_line();
            } else if(params == "4") { // End
                cursor_ = line_buffer_.size();
                refresh_line();
            }
            break;
        default: break;
    }
}

void Client::process_char(char c) {
    unsigned char uc = static_cast<unsigned char>(c);

    if(esc_state_ == EscState::ESC) {
        if(uc == '[') {
            esc_state_ = EscState::CSI;
            csi_params_.clear();
        } else if(uc == 'O') {
            esc_state_ = EscState::CSI;
            csi_params_ = "O";
        } else {
            esc_state_ = EscState::NONE; // unrecognized escape, drop
        }
        read_char();
        return;
    }

    if(esc_state_ == EscState::CSI) {
        if(((uc >= '0' && uc <= '9') || uc == ';') && csi_params_.size() < 8) {
            csi_params_ += static_cast<char>(uc);
            read_char();
            return;
        }
        handle_csi_final(csi_params_, static_cast<char>(uc));
        esc_state_ = EscState::NONE;
        csi_params_.clear();
        read_char();
        return;
    }

    if(uc == '\x1b') {
        esc_state_ = EscState::ESC;
        read_char();
        return;
    }

    if(uc == '\r' || uc == '\n') {
        std::cout << "\r\n";
        std::string line = line_buffer_;
        if(!masked_ && !line.empty() && (history_.empty() || history_.back() != line)) {
            history_.push_back(line);
        }
        history_pos_ = history_.size();
        history_saved_.clear();
        line_buffer_.clear();
        cursor_ = 0;
        reading_line_ = false;
        asio::post(io_context_, [this, line = std::move(line)] {
            handle_request(line);
        });
        return;
    }

    if(uc == 127 || uc == 8) { // Backspace
        if(cursor_ > 0) {
            line_buffer_.erase(cursor_ - 1, 1);
            cursor_--;
            refresh_line();
        }
        read_char();
        return;
    }

    if(uc == 4) { // Ctrl-D
        if(line_buffer_.empty()) {
            std::cout << "\r\n";
            reading_line_ = false;
            exit();
            return;
        }
        read_char();
        return;
    }

    if(uc >= 32 && uc < 127) { // Printable
        line_buffer_.insert(line_buffer_.begin() + static_cast<std::string::difference_type>(cursor_), static_cast<char>(uc));
        cursor_++;
        refresh_line();
    }

    read_char();
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

    asio::async_write(socket_, buffers, [this, len, write_buffer](std::error_code ec, std::size_t) {
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

    asio::async_write(socket_, buffers, [this, len, write_buffer](std::error_code ec, std::size_t) {
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
    if(is_tty_) {
        masked_ = true;
        current_prompt_ = "Password for " + username_ + ": ";
        line_buffer_.clear();
        cursor_ = 0;
        std::cout << current_prompt_ << std::flush;
    } else {
        password_guard_ = std::make_unique<TerminalNoEcho>();
        std::cout << "Password for " << username_ << ": " << std::flush;
    }
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
        read_line();
        return;
    } else if (res.status == protocol::statuses::EXIT) {
        exit();
        return;
    } else if(res.status == protocol::statuses::BUSY) {
        print(res.code, res.message, false);
        state_ = ClientState::PROCESSING;
    }

    print(res.code, res.message, !batch_active_);

    if(res.status == protocol::statuses::NEED_INPUT) {
        state_ = ClientState::NEED_INPUT;
        read_line();
    } else if(res.status == protocol::statuses::BUSY) {
        command_finished(false); // Counts as a failed item mid-batch, re-arms stdin otherwise
    } else if(res.status == protocol::statuses::ERROR) {
        bool listing_failed = (state_ == ClientState::SYNC_LISTING);
        state_ = ClientState::READY;
        if(listing_failed) { // The batch never started, nothing to drain
            batch_mode_ = BatchMode::NONE;
        }
        command_finished(false);
    } else if(res.status == protocol::statuses::OK) {
        if(state_ == ClientState::SYNC_LISTING) {
            state_ = ClientState::READY;
            handle_sync_listing(res);
            return;
        } else if(state_ == ClientState::UPLOAD_INIT) {
            state_ = ClientState::UPLOADING;
            upload_init();
            return;
        } else if(state_ == ClientState::DOWNLOAD_INIT) {
            download_init(res.chunks, res.file_hash);
            return;
        } else {
            state_ = ClientState::READY;
            command_finished(true);
        }
    } else if (res.status == protocol::statuses::RESUME) {
        if (res.file_hash.empty()) { // question: "Do you want to resume X of Y? (y/n)"
            state_ = ClientState::NEED_INPUT_RESUME_TRANSFER;
            read_line();
        } else { // kickoff: message is "UPLOAD <path>" / "DOWNLOAD <path>", file_hash carries the transfer id
            uint32_t id = static_cast<uint32_t>(std::stoul(res.file_hash));
            std::istringstream iss(res.message);
            std::string cmd;
            iss >> cmd;

            auto entry = partmeta_->get_entry(id);
            if (!entry) {
                print(protocol::codes::INTERNAL_SERVER_ERROR, "No local record of this transfer; cannot resume.");
                transfer_.transfer_id = id;
                if (cmd == protocol::commands::UPLOAD) {
                    upload_abort(false, true, protocol::flags::ERROR);
                } else {
                    download_abort(false, true, protocol::flags::ERROR);
                }
                return;
            }

            transfer_.fmeta.absolute_path = entry->absolute_path;
            transfer_.fmeta.size = entry->size;
            transfer_.fmeta.hash = entry->file_hash;
            transfer_.chunks = entry->chunks;
            transfer_.chunk_state = entry->chunk_state;

            if (cmd == protocol::commands::UPLOAD) {
                transfer_.transfer_id = id;
                state_ = ClientState::UPLOADING;
                uploading();
            } else if (cmd == protocol::commands::DOWNLOAD) {
                transfer_.partial_path = partmeta_->get_partial_path(id);
                transfer_.transfer_id = UINT32_MAX; // left uninitialized so handle_chunk() adopts it from the first chunk header, same as a fresh download
                state_ = ClientState::DOWNLOADING;
            }
        }
    }
}

void Client::handle_request(const std::string& line) {
    if(line == protocol::commands::EXIT) { // Priority over other commands
        exit();
        return;
    }

    if(state_ == ClientState::AUTH) {
        if(is_tty_) {
            masked_ = false;
            current_prompt_ = PROMPT;
        } else {
            password_guard_.reset();
            std::cout << std::endl;
        }
        auth(line);

    } else if(state_ == ClientState::READY) {
        std::istringstream iss(line); // For parsing
        std::string cmd;
        iss >> cmd;

        auto it = commands_.find(cmd); // Check map

        if(it == commands_.end()) {
            print(protocol::codes::BAD_REQUEST, "Invalid command \"" + cmd + "\".");
            read_line();
            return;
        }

        it->second(iss); // Call command

    } else if(state_ == ClientState::NEED_INPUT || state_ == ClientState::NEED_INPUT_RESUME_TRANSFER) {
        need_input(line);

    } else if(state_ == ClientState::LOGIN) {
        login();

    } else if(state_ == ClientState::PROCESSING || state_ == ClientState::UPLOAD_INIT || state_ == ClientState::DOWNLOAD_INIT) {
        print(protocol::codes::SERVICE_UNAVAILABLE, "Server is busy...");
    } else if (state_ == ClientState::EXIT) {
        return;

    } else if(state_ == ClientState::UPLOADING || state_ == ClientState::DOWNLOADING) { // Waiting for finished transfer and prompting for potentional exit
        print(protocol::codes::SERVICE_UNAVAILABLE, "Transfer in progress...");
    } else if(state_ == ClientState::NEED_INPUT_RESUME_TRANSFER) {
        need_input(line);
    }
}

void Client::handle_chunk(const protocol::ChunkHeader& ch, const std::vector<uint8_t>& data) {
    if(state_ == ClientState::UPLOADING) {
        if(ch.flags == protocol::flags::OK) {
            if(ch.transfer_id != transfer_.transfer_id) {
                print(protocol::codes::INTERNAL_SERVER_ERROR, "Transfer ID mismatch.", !batch_active_);
                upload_abort(false, true, protocol::flags::ERROR); // Last: may dispatch the next batch item
                return;
            }
            transfer_.chunk_state[ch.index] = true; // Set chunk at index to sent
            partmeta_->mark_chunk_received(transfer_.transfer_id, ch.index); // Set chunk at index to sent in partial metadata database
            uploading();
        } else if (ch.flags == protocol::flags::DONE) {
            transfer_.chunk_state[ch.index] = true;
            print(protocol::codes::OK, "Upload successful", !batch_active_);
            upload_done();
            return;
        } else if(ch.flags == protocol::flags::ERROR) {
            print(protocol::codes::INTERNAL_SERVER_ERROR, "Upload failed.", !batch_active_);
            upload_abort(false, false, protocol::flags::ERROR); // Last: may dispatch the next batch item
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
        read_line();
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

    input_.cancel();
    
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
    read_line();
}

void Client::send_command(const std::string& cmd, const std::string& first, const std::string& second) {
    protocol::Request req{
        cmd,
        first,
        second,
        0,
        ""
    };
    req.chunks.clear();
    json j;
    protocol::to_json(j, req);
    send_json(j);
    state_ = ClientState::PROCESSING;
}

void Client::do_list(const std::string& remote) {
    send_command(protocol::commands::LIST, remote, "");
}

void Client::do_delete(const std::string& remote) {
    send_command(protocol::commands::DELETE, remote, "");
}

void Client::do_mkdir(const std::string& remote) {
    send_command(protocol::commands::MKDIR, remote, "");
}

void Client::do_rmdir(const std::string& remote) {
    send_command(protocol::commands::RMDIR, remote, "");
}

void Client::do_move(const std::string& remote_from, const std::string& remote_to) {
    send_command(protocol::commands::MOVE, remote_from, remote_to);
}

void Client::do_copy(const std::string& remote_from, const std::string& remote_to) {
    send_command(protocol::commands::COPY, remote_from, remote_to);
}

void Client::do_upload(const std::filesystem::path& local, const std::string& remote) {
    if(!fsutils::exists(local)) {
        print(protocol::codes::BAD_REQUEST, "Local file does not exist: " + local.string(), !batch_active_);
        command_finished(false);
        return;
    }

    fsutils::FileMetadata fmeta = fsutils::scan_file(local);

    if(fsutils::is_scan_file_error(fmeta)) {
        print(protocol::codes::INTERNAL_SERVER_ERROR, "Gathering file metadata failed: " + local.string(), !batch_active_);
        command_finished(false);
        return;
    }

    if(fmeta.size == 0) {
        print(protocol::codes::BAD_REQUEST, "Local file is empty: " + local.string(), !batch_active_);
        command_finished(false);
        return;
    }

    std::vector<protocol::ChunkInfo> chunks = fsutils::compute_chunks(fmeta);

    if(fsutils::is_compute_chunks_error(chunks)) {
        print(protocol::codes::INTERNAL_SERVER_ERROR, "Generating file chunks failed: " + local.string(), !batch_active_);
        command_finished(false);
        return;
    }

    transfer_.fmeta = fmeta;
    transfer_.chunks = chunks;
    transfer_.chunk_state = std::vector<bool>(chunks.size(), false);

    protocol::Request req{
        protocol::commands::UPLOAD,
        remote,
        "",
        fmeta.size,
        fsutils::hash_to_hex(fmeta.hash),
        chunks
    };

    json j;
    protocol::to_json(j, req);
    send_json(j);
    state_ = ClientState::UPLOAD_INIT;
}

void Client::do_download(const std::string& remote, const std::filesystem::path& local, bool allow_overwrite) {
    if(!allow_overwrite && fsutils::exists(local)) {
        print(protocol::codes::BAD_REQUEST, "Local file already exists: " + local.string(), !batch_active_);
        command_finished(false);
        return;
    }

    // download_done() renames the .part file onto this path, so its parent has to be there already
    if(local.has_parent_path() && !local.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(local.parent_path(), ec);
    }

    transfer_.fmeta = fsutils::FileMetadata{
        local,
        0,
        0,
        {}
    };

    protocol::Request req{
        protocol::commands::DOWNLOAD,
        remote,
        "",
        0,
        ""
    };
    req.chunks.clear();
    json j;
    protocol::to_json(j, req);
    send_json(j);
    state_ = ClientState::DOWNLOAD_INIT;
}

void Client::cmd_list(std::istringstream& iss) {
    std::string path;
    if(!(iss >> path)) {
        path.clear();
    }
    do_list(path);
}

void Client::cmd_upload(std::istringstream& iss) {
    std::string local_path;
    std::string remote_path;
    if(!(iss >> local_path)) {
        print(protocol::codes::BAD_REQUEST, "Missing local path argument.");
        read_line();
        return;
    }

    std::filesystem::path local(local_path);

    if(!(iss >> remote_path)) {
        remote_path = local.filename().string();
    }

    do_upload(local, remote_path);
}

void Client::cmd_download(std::istringstream& iss) {
    std::string local_path;
    std::string remote_path;
    if(!(iss >> remote_path)) {
        print(protocol::codes::BAD_REQUEST, "Missing remote path argument.");
        read_line();
        return;
    }

    std::filesystem::path local;
    if(iss >> local_path) {
        local = std::filesystem::path(local_path);
    } else {
        local = std::filesystem::current_path() / std::filesystem::path(remote_path).filename();
    }

    do_download(remote_path, local, false);
}

void Client::cmd_delete(std::istringstream& iss) {
    std::vector<std::string> paths;
    std::string path;
    while(iss >> path) paths.push_back(path);

    if(paths.empty()) {
        print(protocol::codes::BAD_REQUEST, "Missing path argument.");
        read_line();
        return;
    }

    if(paths.size() == 1) { // Unchanged single-item behaviour, never queued
        do_delete(paths[0]);
        return;
    }

    std::vector<SyncOp> ops;
    for(const std::string& p : paths) {
        SyncOp op{};
        op.type = SyncOpType::DELETE_REMOTE;
        op.remote_path = p;
        ops.push_back(op);
    }
    begin_batch(BatchMode::PLAIN, "Delete", std::move(ops));
    advance_batch_queue();
}

void Client::cmd_cd(std::istringstream& iss) {
    std::string path;
    if(!(iss >> path)) {
        print(protocol::codes::BAD_REQUEST, "Missing path argument.");
        read_line();
        return;
    }
    send_command(protocol::commands::CD, path, "");
}

void Client::cmd_mkdir(std::istringstream& iss) {
    std::string path;
    if(!(iss >> path)) {
        print(protocol::codes::BAD_REQUEST, "Missing path argument.");
        read_line();
        return;
    }
    do_mkdir(path);
}

void Client::cmd_rmdir(std::istringstream& iss) {
    std::string path;
    if(!(iss >> path)) {
        print(protocol::codes::BAD_REQUEST, "Missing path argument.");
        read_line();
        return;
    }
    do_rmdir(path);
}

// Shared parser for MOVE and COPY: "<src>... <dest>". One source with a destination that is not
// explicitly a directory keeps the classic single-rename behaviour; several sources, or a
// destination written with a trailing '/', mean "into that directory" and are expanded client-side
// into one full (src, dst) pair per item - the server's move()/copy() handlers are untouched.
void Client::batch_move_or_copy(std::istringstream& iss, bool is_move) {
    const char* label = is_move ? "Move" : "Copy";

    std::vector<std::string> args;
    std::string arg;
    while(iss >> arg) args.push_back(arg);

    if(args.size() < 2) {
        print(protocol::codes::BAD_REQUEST, "Missing source or destination path argument.");
        read_line();
        return;
    }

    std::string dest = args.back();
    args.pop_back();

    bool directory_target = !dest.empty() && dest.back() == '/';

    if(args.size() == 1 && !directory_target) {
        if(is_move) do_move(args[0], dest);
        else do_copy(args[0], dest);
        return;
    }

    while(!dest.empty() && dest.back() == '/') dest.pop_back();

    std::vector<SyncOp> ops;
    for(const std::string& source : args) {
        SyncOp op{};
        op.type = is_move ? SyncOpType::MOVE_REMOTE : SyncOpType::COPY_REMOTE;
        op.remote_path_from = source;
        op.remote_path = join_remote(dest, std::filesystem::path(source).filename().string());
        ops.push_back(op);
    }
    begin_batch(BatchMode::PLAIN, label, std::move(ops));
    advance_batch_queue();
}

void Client::cmd_move(std::istringstream& iss) {
    batch_move_or_copy(iss, true);
}

void Client::cmd_copy(std::istringstream& iss) {
    batch_move_or_copy(iss, false);
}

void Client::cmd_sync(std::istringstream& iss) {
    std::string local_path;
    std::string remote_path;
    if(!(iss >> local_path >> remote_path)) {
        print(protocol::codes::BAD_REQUEST, "Missing source or destination path argument.");
        read_line();
        return;
    }

    std::filesystem::path local = fsutils::absolute(local_path);
    if(!fsutils::is_directory(local)) {
        print(protocol::codes::BAD_REQUEST, "Local sync directory does not exist.");
        read_line();
        return;
    }

    sync_local_dir_ = local;
    sync_remote_dir_ = remote_path;
    batch_mode_ = BatchMode::SYNC;

    // The listing is the only SYNC-specific thing the server does; everything the diff decides on
    // afterwards travels as ordinary single-item commands.
    protocol::Request req{
        protocol::commands::SYNC,
        remote_path,
        "",
        0,
        ""
    };
    req.chunks.clear();
    json j;
    protocol::to_json(j, req);
    send_json(j);
    state_ = ClientState::SYNC_LISTING;
}

void Client::cmd_upload_dir(std::istringstream& iss) {
    std::string local_path;
    std::string remote_path;
    if(!(iss >> local_path)) {
        print(protocol::codes::BAD_REQUEST, "Missing local directory argument.");
        read_line();
        return;
    }

    std::filesystem::path local = fsutils::absolute(local_path);
    if(!fsutils::is_directory(local)) {
        print(protocol::codes::BAD_REQUEST, "Local directory does not exist.");
        read_line();
        return;
    }

    if(!(iss >> remote_path)) {
        remote_path = local.filename().string();
    }

    std::map<std::string, SyncEntry> tree;
    if(!scan_local_tree(local, tree)) {
        print(protocol::codes::INTERNAL_SERVER_ERROR, "Failed to scan local directory.");
        read_line();
        return;
    }

    std::vector<SyncOp> ops;

    SyncOp root{};
    root.type = SyncOpType::MKDIR_REMOTE;
    root.remote_path = remote_path;
    ops.push_back(root);

    // Relative paths sort parent-before-child, so plain map order is already shallow to deep
    for(const auto& [relative_path, entry] : tree) {
        if(!entry.is_directory) continue;
        SyncOp op{};
        op.type = SyncOpType::MKDIR_REMOTE;
        op.remote_path = join_remote(remote_path, relative_path);
        ops.push_back(op);
    }
    for(const auto& [relative_path, entry] : tree) {
        if(entry.is_directory) continue;
        SyncOp op{};
        op.type = SyncOpType::UPLOAD;
        op.local_path = local / std::filesystem::path(relative_path);
        op.remote_path = join_remote(remote_path, relative_path);
        ops.push_back(op);
    }

    begin_batch(BatchMode::PLAIN, "Directory upload", std::move(ops));
    advance_batch_queue();
}

void Client::cmd_download_dir(std::istringstream& iss) {
    std::string remote_path;
    std::string local_path;
    if(!(iss >> remote_path)) {
        print(protocol::codes::BAD_REQUEST, "Missing remote directory argument.");
        read_line();
        return;
    }

    if(iss >> local_path) {
        sync_local_dir_ = fsutils::absolute(local_path);
    } else {
        sync_local_dir_ = fsutils::absolute(std::filesystem::current_path() / std::filesystem::path(remote_path).filename());
    }

    sync_remote_dir_ = remote_path;
    batch_mode_ = BatchMode::DOWNLOAD_DIR;

    protocol::Request req{ // Same listing request SYNC uses
        protocol::commands::SYNC,
        remote_path,
        "",
        0,
        ""
    };
    req.chunks.clear();
    json j;
    protocol::to_json(j, req);
    send_json(j);
    state_ = ClientState::SYNC_LISTING;
}

bool Client::scan_local_tree(const std::filesystem::path& dir, std::map<std::string, SyncEntry>& out) {
    std::filesystem::path base = fsutils::absolute(dir);
    std::vector<fsutils::FileMetadata> files = fsutils::scan_directory(base, true);
    if(fsutils::is_scan_dir_error(files)) return false;

    for(const fsutils::FileMetadata& file : files) {
        std::string relative_path = fsutils::relative(base, file.absolute_path).generic_string();
        if(relative_path.empty() || relative_path == ".") continue;
        // The baseline lives inside the synced folder, it is bookkeeping and never content
        if(relative_path == ".minidrive-sync" || relative_path.rfind(".minidrive-sync/", 0) == 0) continue;

        SyncEntry entry;
        entry.is_directory = fsutils::is_directory(file.absolute_path);
        entry.mtime = file.last_modified;
        entry.size = entry.is_directory ? 0u : file.size;
        entry.hash = entry.is_directory ? std::string() : fsutils::hash_to_hex(file.hash);
        out[relative_path] = entry;
    }
    return true;
}

void Client::handle_sync_listing(const protocol::Response& res) {
    std::map<std::string, SyncEntry> remote;
    for(const protocol::FileEntry& file : res.files) {
        SyncEntry entry;
        entry.hash = file.file_hash;
        entry.mtime = file.last_modified;
        entry.size = file.size;
        entry.is_directory = file.is_directory;
        remote[file.relative_path] = entry;
    }

    std::error_code ec;

    if(batch_mode_ == BatchMode::DOWNLOAD_DIR) {
        std::filesystem::create_directories(sync_local_dir_, ec);

        std::vector<SyncOp> ops;
        for(const auto& [relative_path, entry] : remote) {
            if(entry.is_directory) {
                std::filesystem::create_directories(sync_local_dir_ / std::filesystem::path(relative_path), ec);
                continue;
            }
            SyncOp op{};
            op.type = SyncOpType::DOWNLOAD;
            op.local_path = sync_local_dir_ / std::filesystem::path(relative_path);
            op.remote_path = join_remote(sync_remote_dir_, relative_path);
            ops.push_back(op);
        }

        begin_batch(BatchMode::DOWNLOAD_DIR, "Directory download", std::move(ops));
        advance_batch_queue();
        return;
    }

    std::map<std::string, SyncEntry> local;
    if(!scan_local_tree(sync_local_dir_, local)) {
        print(protocol::codes::INTERNAL_SERVER_ERROR, "Failed to scan local sync directory.");
        batch_mode_ = BatchMode::NONE;
        state_ = ClientState::READY;
        read_line();
        return;
    }

    manifest_.emplace(sync_local_dir_, sync_remote_dir_);

    std::map<std::string, SyncEntry> baseline;
    for(const auto& [relative_path, entry] : manifest_->get_entries()) {
        baseline[relative_path] = SyncEntry{entry.hash, entry.mtime, entry.size, entry.is_directory};
    }

    SyncPlan plan = compute_sync_plan(local, remote, baseline, sync_local_dir_, sync_remote_dir_);

    std::vector<SyncOp> ops = plan.ops;
    begin_batch(BatchMode::SYNC, "Sync", std::move(ops));

    // Purely local work needs no network op, so it is done here rather than queued
    for(const std::filesystem::path& new_dir : plan.local_mkdirs) {
        std::filesystem::create_directories(new_dir, ec);
        batch_dirs_++;
    }
    for(const std::filesystem::path& gone : plan.local_deletes) {
        if(fsutils::remove_file(gone)) batch_deleted_++;
        else batch_failed_++;
    }

    for(const auto& [relative_path, entry] : plan.baseline_set) {
        manifest_->put(relative_path, SyncManifestEntry{entry.hash, entry.mtime, entry.size, entry.is_directory});
    }
    for(const std::string& relative_path : plan.baseline_drop) {
        manifest_->remove(relative_path);
    }
    manifest_->save();

    batch_skipped_ = plan.skipped;
    batch_conflicts_ = plan.conflicts;

    advance_batch_queue();
}

void Client::begin_batch(BatchMode mode, const std::string& label, std::vector<SyncOp> ops) {
    batch_queue_.assign(ops.begin(), ops.end());
    batch_active_ = true;
    batch_mode_ = mode;
    batch_label_ = label;
    current_op_.reset();
    batch_uploaded_ = 0;
    batch_downloaded_ = 0;
    batch_deleted_ = 0;
    batch_moved_ = 0;
    batch_copied_ = 0;
    batch_dirs_ = 0;
    batch_skipped_ = 0;
    batch_failed_ = 0;
    batch_conflicts_.clear();
}

// Dispatches one op at a time. An op that fails before it ever reaches the wire (a missing local
// file, say) calls command_finished() synchronously from inside this very function, so the
// advancing_/advance_pending_ pair turns what would be recursion into a loop.
void Client::advance_batch_queue() {
    if(!batch_active_) return;
    if(advancing_) {
        advance_pending_ = true;
        return;
    }

    advancing_ = true;
    do {
        advance_pending_ = false;

        if(batch_queue_.empty()) {
            advancing_ = false;
            finish_batch();
            return;
        }

        current_op_ = batch_queue_.front();
        batch_queue_.pop_front();
        const SyncOp& op = *current_op_;

        switch(op.type) {
            case SyncOpType::MKDIR_REMOTE:      do_mkdir(op.remote_path); break;
            case SyncOpType::RMDIR_REMOTE:      do_rmdir(op.remote_path); break;
            case SyncOpType::DELETE_REMOTE:     do_delete(op.remote_path); break;
            case SyncOpType::MOVE_REMOTE:       do_move(op.remote_path_from, op.remote_path); break;
            case SyncOpType::COPY_REMOTE:       do_copy(op.remote_path_from, op.remote_path); break;
            case SyncOpType::UPLOAD:            do_upload(op.local_path, op.remote_path); break;
            case SyncOpType::DOWNLOAD:          do_download(op.remote_path, op.local_path, true); break;
            case SyncOpType::CONFLICT_DOWNLOAD: do_download(op.remote_path, op.local_path, false); break;
        }
    } while(advance_pending_);
    advancing_ = false;
}

void Client::record_op_result() {
    if(!current_op_) return;
    const SyncOp& op = *current_op_;

    switch(op.type) {
        case SyncOpType::MKDIR_REMOTE:      batch_dirs_++; break;
        case SyncOpType::RMDIR_REMOTE:      batch_deleted_++; break;
        case SyncOpType::DELETE_REMOTE:     batch_deleted_++; break;
        case SyncOpType::MOVE_REMOTE:       batch_moved_++; break;
        case SyncOpType::COPY_REMOTE:       batch_copied_++; break;
        case SyncOpType::UPLOAD:            batch_uploaded_++; break;
        case SyncOpType::DOWNLOAD:          batch_downloaded_++; break;
        case SyncOpType::CONFLICT_DOWNLOAD: batch_downloaded_++; break;
    }

    if(!manifest_) return;

    // The baseline is advanced one op at a time, so an interrupted batch simply leaves it
    // describing what actually completed and the next SYNC recomputes the correct remaining diff
    if(!op.relative_path_from.empty()) manifest_->remove(op.relative_path_from);
    if(!op.relative_path.empty()) {
        manifest_->put(op.relative_path, SyncManifestEntry{op.entry.hash, op.entry.mtime, op.entry.size, op.entry.is_directory});
    }
    manifest_->save();
}

// Terminal point of every command, batched or not. One failing item never aborts the queue -
// failures are counted and reported in the summary instead.
void Client::command_finished(bool success) {
    if(!batch_active_) {
        read_line();
        return;
    }

    if(success) {
        record_op_result();
    } else if(current_op_ && current_op_->type != SyncOpType::MKDIR_REMOTE) {
        batch_failed_++; // A directory that already exists is the normal case, not a failure
    }

    current_op_.reset();
    advance_batch_queue();
}

void Client::finish_batch() {
    batch_active_ = false;
    batch_mode_ = BatchMode::NONE;
    batch_queue_.clear();
    current_op_.reset();

    if(manifest_) {
        manifest_->save();
        manifest_.reset();
    }

    for(const std::string& conflict : batch_conflicts_) {
        std::cout << "CONFLICT: " << conflict << std::endl;
    }

    std::ostringstream summary;
    summary << batch_label_ << " complete."
            << " Uploaded: " << batch_uploaded_
            << ", Downloaded: " << batch_downloaded_
            << ", Deleted: " << batch_deleted_
            << ", Moved: " << batch_moved_
            << ", Copied: " << batch_copied_
            << ", Directories created: " << batch_dirs_
            << ", Skipped (unchanged): " << batch_skipped_
            << ", Failed: " << batch_failed_
            << ", Conflicts: " << batch_conflicts_.size() << ".";

    batch_conflicts_.clear();
    state_ = ClientState::READY;
    print(protocol::codes::OK, summary.str());
    read_line();
}

void Client::upload_init() {
    transfer_.transfer_id = partmeta_->add_partial_metadata(TransferType::UPLOAD, transfer_.fmeta, transfer_.chunks, UINT32_MAX);
    //read_line();
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
    command_finished(true);
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

    if(flag != protocol::flags::EXIT) {
        state_ = ClientState::READY;
        command_finished(false);
    }
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
    //read_line();
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
    
    print(protocol::codes::OK, "Download successful", !batch_active_);
    state_ = ClientState::READY;
    command_finished(true);
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

    if(flag != protocol::flags::EXIT) {
        state_ = ClientState::READY;
        command_finished(false);
    }
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