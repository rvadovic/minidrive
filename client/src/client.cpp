#include <asio.hpp>
#include <iostream>
#include <vector>
#include "client.hpp"
#include "terminalNoEcho.hpp"
#include "protocol/message.hpp"
#include "protocol/commands.hpp"
#include "protocol/codes.hpp"
#include "protocol/statuses.hpp"
#include <nlohmann/json.hpp>
#include "filesystem/utils.hpp"

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
                read_header();
            } else {
                handle_error(ec);
                return;
            }
        });
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

void Client::send(const json& j) {
    auto write_buffer = std::make_shared<std::string>(j.dump());
    auto len = std::make_shared<uint32_t>(htonl(write_buffer->size()));

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

void Client::read_header() {
    asio::async_read(socket_, asio::buffer(&msg_len_, sizeof(msg_len_)),[this](std::error_code ec, std::size_t) {
        if(!ec) {
            msg_len_ = ntohl(msg_len_);
            buffer_.resize(msg_len_);
            read_body();
        } else {
            handle_error(ec);
            return;
        }
    });
}

void Client::read_body() {
    asio::async_read(socket_, asio::buffer(buffer_),[this](std::error_code ec, std::size_t) {
        if(!ec) {
            std::string msg(buffer_.begin(), buffer_.end());
            json j = json::parse(msg);

            handle_response(j);
            read_header();
        } else {
            handle_error(ec);
            return;
        }
    });
}

void Client::on_password_required() {
    password_guard_ = std::make_unique<TerminalNoEcho>();
    std::cout << "Password: " << std::flush;
}

void Client::handle_error(const std::error_code& ec) {
    std::cerr << "Network error: " << ec.message() << " (" << ec.value() << ")" << std::endl;
    std::cout << "Closing client" << std::endl;
    exit();
}

void Client::handle_response(const json& j) { //TODO
    protocol::Response res;
    protocol::from_json(j, res);
    if(res.status == protocol::statuses::AUTH) {
        state_ = ClientState::AUTH;
        std::cout << res.code << ": " << res.message << std::endl;
        on_password_required();
    } else if(res.status == protocol::statuses::NEED_INPUT) {
        state_ = ClientState::NEED_INPUT;
        std::cout << res.code << ": " << res.message << std::endl;
        std::cout << "> " << std::flush;
    } else if(res.status == protocol::statuses::ERROR) {
        state_ = ClientState::READY;
        std::cout << res.code << ": " << res.message << std::endl;
        std::cout << "> " << std::flush;
    } else if(res.status == protocol::statuses::OK) {
        state_ = ClientState::READY;
        std::cout << res.code << ": " << res.message << std::endl;
        std::cout << "> " << std::flush;
    } else if(res.status == protocol::statuses::CONFLICT) {
        std::cout << res.code << ": " << res.message << std::endl;
    } else if(res.status == protocol::statuses::BUSY) {
        state_ = ClientState::PROCESSING;
        std::cout << "Server is busy." << std::endl;
    }
}

void Client::handle_request(const std::string& line) {
    if(line == protocol::commands::EXIT) {
        exit();
    } else if(state_ == ClientState::AUTH) {
        password_guard_.reset();
        auth(line);
    } else if(state_ == ClientState::READY) {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        auto it = commands_.find(cmd);

        if(it == commands_.end()) {
            std::cout << protocol::codes::BAD_REQUEST << ": Invalid command \""<< cmd << "\"." << std::endl;
            std::cout << "> " << std::flush;
            return;
        }

        it->second(iss);
    } else if(state_ == ClientState::NEED_INPUT) {

    } else if(state_ == ClientState::LOGIN) {
        login();
    } else if(state_ == ClientState::PROCESSING) {
        std::cout << "Waiting for server..." << std::endl;
    } else if (state_ == ClientState::EXIT) {
        return;
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
        json j;
        protocol::to_json(j, req);
        send(j);
}

void Client::auth(const std::string password) {
    protocol::Request req{
            protocol::commands::AUTH,
            password,
            "",
            0,
            ""
        };
        json j;
        protocol::to_json(j, req);
        send(j);
        state_ = ClientState::PROCESSING;
}

void Client::need_input(const std::string input) {
    if(!(input == "Y" || input == "n")) {
        std::cout << protocol::codes::BAD_REQUEST << ": Invalid input." << std::endl;
        return;
    }
    protocol::Request req{
            protocol::commands::NEED_INPUT,
            input,
            "",
            0,
            ""
        };
        json j;
        protocol::to_json(j, req);
        send(j);
        state_ = ClientState::PROCESSING;
}

void Client::exit() {
    bool expected = false;

    if(!exiting_.compare_exchange_strong(expected, true)) return;

    protocol::Request req{
        protocol::commands::EXIT,
        "",
        "",
        0,
        ""
    };
    json j;
    protocol::to_json(j, req);
    send(j);

    asio::post(io_context_), [this]() {
        std::error_code ec;
        if(socket_.is_open()) {
            socket_.shutdown(tcp::socket::shutdown_both, ec);
            socket_.close(ec);
        }
    };
    state_ = ClientState::EXIT;
}

void Client::cmd_help(std::istringstream& iss) {
    for (const auto& [key, value] : commands_) {
        std:: cout << key << std::endl;
    }
    std::cout << "The syntax of filesystem commands is: \"Command\" \"what\" \"where\"." << std::endl;
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

    json j;
    protocol::to_json(j, req);
    send(j);
    state_ = ClientState::PROCESSING;
}

void Client::cmd_upload(std::istringstream& iss) {
    std::string local_path;
    std::string remote_path;
    if(!(iss >> local_path)) {
        std::cout << protocol::codes::BAD_REQUEST << ": Missing local path argument." << std::endl;
        return;
    }
    //TODO check if file exists
    //TODO get file size and hash
    protocol::Request req{
            protocol::commands::UPLOAD,
            local_path,
            "",
            0,
            ""
        };

    if((iss >> remote_path)) {
        req.second_argument = remote_path;
    }

    json j;
    protocol::to_json(j, req);
    send(j);
    state_ = ClientState::PROCESSING;
}

void Client::cmd_download(std::istringstream& iss) {
    std::string local_path;
    std::string remote_path;
    if(!(iss >> remote_path)) {
        std::cout << protocol::codes::BAD_REQUEST << ": Missing remote path argument." << std::endl;
        return;
    }
    //TODO check if fiile exists or get current directory
    if(!(iss >> local_path)) {
        
    }

    protocol::Request req{
            protocol::commands::UPLOAD,
            remote_path,
            local_path,
            0,
            ""
        };

    json j;
    protocol::to_json(j, req);
    send(j);
    state_ = ClientState::PROCESSING;
}

void Client::cmd_delete(std::istringstream& iss) {
    std::string path;
    if(!(iss >> path)) {
        std::cout << protocol::codes::BAD_REQUEST << ": Missing path argument." << std::endl;
        return;
    }
    protocol::Request req{
        protocol::commands::LIST,
        path,
        "",
        0,
        ""
    };
    json j;
    protocol::to_json(j, req);
    send(j);
    state_ = ClientState::PROCESSING;
}

void Client::cmd_cd(std::istringstream& iss) {
    std::string path;
    if(!(iss >> path)) {
        std::cout << protocol::codes::BAD_REQUEST << ": Missing path argument." << std::endl;
        return;
    }
    protocol::Request req{
        protocol::commands::CD,
        path,
        "",
        0,
        ""
    };
    json j;
    protocol::to_json(j, req);
    send(j);
    state_ = ClientState::PROCESSING;
}

void Client::cmd_mkdir(std::istringstream& iss) {
    std::string path;
    if(!(iss >> path)) {
        std::cout << protocol::codes::BAD_REQUEST << ": Missing path argument." << std::endl;
        return;
    }
    protocol::Request req{
        protocol::commands::MKDIR,
        path,
        "",
        0,
        ""
    };
    json j;
    protocol::to_json(j, req);
    send(j);
    state_ = ClientState::PROCESSING;
}

void Client::cmd_rmdir(std::istringstream& iss) {
    std::string path;
    if(!(iss >> path)) {
        std::cout << protocol::codes::BAD_REQUEST << ": Missing path argument." << std::endl;
        return;
    }
    protocol::Request req{
        protocol::commands::RMDIR,
        path,
        "",
        0,
        ""
    };
    json j;
    protocol::to_json(j, req);
    send(j);
    state_ = ClientState::PROCESSING;
}

void Client::cmd_move(std::istringstream& iss) {
    std::string source_path;
    std::string dest_path;
    if(!(iss >> source_path >> dest_path)) {
        std::cout << protocol::codes::BAD_REQUEST << ": Missing source or destination path argument." << std::endl;
        return;
    }
    protocol::Request req{
        protocol::commands::MOVE,
        source_path,
        dest_path,
        0,
        ""
    };
    json j;
    protocol::to_json(j, req);
    send(j);
    state_ = ClientState::PROCESSING;
}

void Client::cmd_copy(std::istringstream& iss) {
    std::string source_path;
    std::string dest_path;
    if(!(iss >> source_path >> dest_path)) {
        std::cout << protocol::codes::BAD_REQUEST << ": Missing source or destination path argument." << std::endl;
        return;
    }
    protocol::Request req{
        protocol::commands::COPY,
        source_path,
        dest_path,
        0,
        ""
    };
    json j;
    protocol::to_json(j, req);
    send(j);
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
    json j;
    protocol::to_json(j, req);
    send(j);
    state_ = ClientState::PROCESSING;
}