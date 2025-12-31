#include <asio.hpp>
#include <iostream>
#include <memory>
#include <vector>
#include <nlohmann/json.hpp>
#include "session.hpp"
#include "protocol/message.hpp"
#include "protocol/commands.hpp"
#include "protocol/statuses.hpp"
#include "protocol/codes.hpp"
#include "filesystem/utils.hpp"

using asio::ip::tcp;
using nlohmann::json;

Session::Session(tcp::socket socket, const std::filesystem::path& root, std::shared_ptr<Database> db) 
    : socket_(std::move(socket)),
      root_(root),
      db_(std::move(db)),
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
          {protocol::commands::EXIT,     [this](auto& req){ exit(req); }},
          {protocol::commands::LOGIN,     [this](auto& req){ login(req); }}
      } {
}

void Session::start() {
    read_header();
}

void Session::read_header() {
    auto self = shared_from_this();
    asio::async_read(socket_, asio::buffer(&msg_len_, sizeof(msg_len_)),[this, self](std::error_code ec, std::size_t) {
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

void Session::read_body() {
    auto self = shared_from_this();
    std::cout << "before reading" << std::endl;
    if(msg_len_ == 0) {
        handle_error(asio::error::invalid_argument);
        read_header();
    }
    asio::async_read(socket_, asio::buffer(buffer_),[this, self](std::error_code ec, std::size_t) {
        if(!ec) {
            std::string msg(buffer_.begin(), buffer_.end());
            json j = json::parse(msg);

            std::cout << "after reading" << std::endl;
            handle_request(j);
            read_header();
        } else {
            handle_error(ec);
            return;
        }
    });
}

void Session::handle_error(const std::error_code& ec) {
    std::cerr << "Network error: " << ec.message() << " (" << ec.value() << ")" << std::endl;
}

void Session::handle_request(const json& j) {
    protocol::Request req;
    protocol::from_json(j, req);
    std::cout << req.cmd << std::endl;
    auto it = requests_.find(req.cmd);
    if (it != requests_.end()) {
        it->second(req);
    } else {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::BAD_REQUEST,
            "Unknown request",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
    }
}

void Session::write_response(const json& j) {
    auto self = shared_from_this();

    auto write_buffer = std::make_shared<std::string>(j.dump());
    auto response_len = std::make_shared<uint32_t>(htonl(write_buffer->size()));

    std::vector<asio::const_buffer> buffers;
    buffers.push_back(asio::buffer(response_len.get(), sizeof(uint32_t)));
    buffers.push_back(asio::buffer(*write_buffer));
    
    asio::async_write(socket_, buffers, [this, self](std::error_code ec, std::size_t) {
        if(ec) {
            handle_error(ec);
            return;
        }
    });
}

void Session::login(protocol::Request& req) {
    if(state_ != SessionSate::LOGIN) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Already logged in",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
        return;
    }

    if(req.first_argument.empty()) {
        user_dir_ = relative(std::filesystem::path("/public/files"), root_);
        current_dir_ = user_dir_;
        username_ = "";
        protocol::Response res {
            protocol::statuses::OK,
            protocol::codes::OK,
            "No username provided. Operating in public mode.",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
        state_ = SessionSate::READY;
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
        json j;
        protocol::to_json(j, res);
        write_response(j);
        state_ = SessionSate::NEED_INPUT_REGISTER;
        return;
    } else {
        protocol::Response res {
            protocol::statuses::AUTH,
            protocol::codes::UNAUTHORIZED,
            "Please provide your password.",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
        state_ = SessionSate::AUTH;
        return;
    }
}

void Session::setup_dir() {
    if(fsutils::is_directory(std::filesystem::path(root_ / "private" / username_))) {
        if(!fsutils::is_directory(std::filesystem::path(root_ / "private" / username_ / "files"))) {
            fsutils::mkdir(std::filesystem::path(root_ / "private" / username_ / "files"));
        }
        if(!fsutils::is_directory(std::filesystem::path(root_ / "private" / username_ / ".partial"))) {
            fsutils::mkdir(std::filesystem::path(root_ / "private" / username_ / ".partial"));
        }
    } else {
        fsutils::mkdir(std::filesystem::path(root_ / "private" / username_));
        fsutils::mkdir(std::filesystem::path(root_ / "private" / username_ / "files"));
        fsutils::mkdir(std::filesystem::path(root_ / "private" / username_ / ".partial"));
    }
    user_dir_ = fsutils::relative(std::filesystem::path("/private/" + username_ + "/files"), root_);
    current_dir_ = user_dir_;
}
void Session::auth(protocol::Request& req) {
    if(state_ != SessionSate::AUTH) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Not in authentication state",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
        return;
    }

    if(req.first_argument.empty()) {
        protocol::Response res {
            protocol::statuses::AUTH,
            protocol::codes::BAD_REQUEST,
            "Password cannot be empty. Try again.",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
        return;
    } else if(db_->user_exists(username_)) {
        if(db_->validate_user(username_, req.first_argument)) {
            setup_dir();
            protocol::Response res {
                protocol::statuses::OK,
                protocol::codes::OK,
                "Authentication successful.",
                ""
            };
            json j;
            protocol::to_json(j, res);
            write_response(j);
            state_ = SessionSate::READY;
            return;
        } else {
            protocol::Response res {
                protocol::statuses::AUTH,
                protocol::codes::UNAUTHORIZED,
                "Invalid password. Try again.",
                ""
            };
            json j;
            protocol::to_json(j, res);
            write_response(j);
            return;
        }
    } else if(!db_->user_exists(username_)) {
        db_->add_user(username_, req.first_argument);
        setup_dir();
        protocol::Response res {
            protocol::statuses::OK,
            protocol::codes::OK,
            "Registration successful.",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
        state_ = SessionSate::READY;
        return;
    }
}
void Session::need_input(protocol::Request& req) {
    switch (state_) {
        case SessionSate::NEED_INPUT_REGISTER:
            if(req.first_argument == "Y") {
                protocol::Response res {
                    protocol::statuses::AUTH,
                    protocol::codes::UNAUTHORIZED,
                    "For registration, please provide a password.",
                    ""
                };
                json j;
                protocol::to_json(j, res);
                write_response(j);
                state_ = SessionSate::AUTH;
                return; 
            } else if(req.first_argument == "n") {
                user_dir_ = relative(std::filesystem::path("/public/files"), root_);
                current_dir_ = user_dir_;

                protocol::Response res {
                    protocol::statuses::OK,
                    protocol::codes::OK,
                    "No registrartion. Operating in public mode.",
                    ""
                };
                json j;
                protocol::to_json(j, res);
                write_response(j);
                state_ = SessionSate::READY;
                return; 
            } else {
                protocol::Response res {
                    protocol::statuses::ERROR,
                    protocol::codes::BAD_REQUEST,
                    "Invalid input for registration. Y/n expected.",
                    ""
                };
                json j;
                protocol::to_json(j, res);
                write_response(j);
                return; 
            }
            break;
        case SessionSate::NEED_INPUT_RESUME_UPDATE:
            // Handle resume update input
            break;
        default:
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::SERVICE_UNAVAILABLE,
                "No input required at this time.",
                ""
            };
            json j;
            protocol::to_json(j, res);
            write_response(j);
            return; 
    }
    // Implementation of need_input
}
void Session::exit(protocol::Request& req) {
    // Implementation of exit
}
void Session::list(protocol::Request& req) {
    if(state_ != SessionSate::READY) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Session not ready.",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
        return;
    }
    if(req.first_argument.empty()) {
        std::vector<fsutils::FileMetadata> files = fsutils::scan_directory(current_dir_, false);
        std::string file_list("Current directory: " + current_dir_.string() + "\n");
        for (const auto& file : files) {
            file_list += file.relative_path.string() + "\n";
        }
        protocol::Response res {
            protocol::statuses::OK,
            protocol::codes::OK,
            file_list,
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
        return;
    }else {
        if(!fsutils::is_directory(std::filesystem::path(req.first_argument))) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::BAD_REQUEST,
                "Directory does not exist.",
                ""
            };
            json j;
            protocol::to_json(j, res);
            write_response(j);
            return;
        }
        if(!fsutils::is_subpath(user_dir_, std::filesystem::path(req.first_argument))) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::FORBIDDEN,
                "Access denied.",
                ""
            };
            json j;
            protocol::to_json(j, res);
            write_response(j);
            return;
        }
        std::vector<fsutils::FileMetadata> files = fsutils::scan_directory(std::filesystem::path(req.first_argument), false);
        std::string file_list;
        for (const auto& file : files) {
            file_list += file.relative_path.string() + "\n";
        }
        protocol::Response res {
            protocol::statuses::OK,
            protocol::codes::OK,
            file_list,
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
        return;
    }
}
void Session::delete_file(protocol::Request& req) {
    if(state_ != SessionSate::READY) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Session not ready.",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
        return;
    }
    if(!fsutils::is_file(std::filesystem::path(req.first_argument))) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::BAD_REQUEST,
                "File does not exist.",
                ""
            };
            json j;
            protocol::to_json(j, res);
            write_response(j);
            return;
        }
        if(!fsutils::is_subpath(user_dir_, std::filesystem::path(req.first_argument))) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::FORBIDDEN,
                "Access denied.",
                ""
            };
            json j;
            protocol::to_json(j, res);
            write_response(j);
            return;
        }
        if(!fsutils::remove_file(std::filesystem::path(req.first_argument))) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::INTERNAL_SERVER_ERROR,
                "Cannot remove file",
                ""
            };
            json j;
            protocol::to_json(j, res);
            write_response(j);
            return;
        }
        protocol::Response res {
            protocol::statuses::OK,
            protocol::codes::OK,
            "File deleted",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
}

void Session::upload(protocol::Request& req) {
    // Implementation of upload
}
void Session::download(protocol::Request& req) {
    // Implementation of download
}
void Session::cd(protocol::Request& req) {
    if(state_ != SessionSate::READY) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Session not ready.",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
        return;
    }
    if(!fsutils::is_directory(std::filesystem::path(req.first_argument))) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::BAD_REQUEST,
                "Directory does not exist.",
                ""
            };
            json j;
            protocol::to_json(j, res);
            write_response(j);
            return;
        }
        if(!fsutils::is_subpath(user_dir_, std::filesystem::path(req.first_argument))) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::FORBIDDEN,
                "Access denied.",
                ""
            };
            json j;
            protocol::to_json(j, res);
            write_response(j);
            return;
        }
        current_dir_ = std::filesystem::path(req.first_argument);
        protocol::Response res {
            protocol::statuses::OK,
            protocol::codes::OK,
            "Directory changed.",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
}
void Session::mkdir(protocol::Request& req) {
    if(state_ != SessionSate::READY) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Session not ready.",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
        return;
    }
    if(req.first_argument.empty()) {
        protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::BAD_REQUEST,
                "Directory path cannot be empty.",
                ""
            };
            json j;
            protocol::to_json(j, res);
            write_response(j);
            return;
    }
    if(fsutils::is_directory(std::filesystem::path(req.first_argument))) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::PRECONDITION_FAILED,
                "Directory already exists.",
                ""
            };
            json j;
            protocol::to_json(j, res);
            write_response(j);
            return;
        }
    if(!fsutils::is_subpath(user_dir_, std::filesystem::path(req.first_argument))) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::FORBIDDEN,
            "Access denied.",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
        return;
    }
    if(!fsutils::mkdir(std::filesystem::path(req.first_argument))) {
        protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::INTERNAL_SERVER_ERROR,
                "Cannot create directory.",
                ""
            };
            json j;
            protocol::to_json(j, res);
            write_response(j);
            return;
    }
    protocol::Response res {
            protocol::statuses::OK,
            protocol::codes::OK,
            "Directory created.",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
}
void Session::rmdir(protocol::Request& req) {
    if(state_ != SessionSate::READY) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Session not ready.",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
        return;
    }
    if(req.first_argument.empty()) {
        protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::BAD_REQUEST,
                "Directory path cannot be empty.",
                ""
            };
            json j;
            protocol::to_json(j, res);
            write_response(j);
            return;
    }
    if(!fsutils::is_directory(std::filesystem::path(req.first_argument))) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::BAD_REQUEST,
                "Directory does no  exist.",
                ""
            };
            json j;
            protocol::to_json(j, res);
            write_response(j);
            return;
        }
    if(!fsutils::is_subpath(user_dir_, std::filesystem::path(req.first_argument))) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::FORBIDDEN,
            "Access denied.",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
        return;
    }
    if(!fsutils::rmdir(std::filesystem::path(req.first_argument))) {
        protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::INTERNAL_SERVER_ERROR,
                "Cannot delete directory.",
                ""
            };
            json j;
            protocol::to_json(j, res);
            write_response(j);
            return;
    }
    protocol::Response res {
            protocol::statuses::OK,
            protocol::codes::OK,
            "Directory deleted.",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
}
void Session::move(protocol::Request& req) {
    if(state_ != SessionSate::READY) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Session not ready.",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
        return;
    }
    if(req.first_argument.empty() || req.second_argument.empty()) {
        protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::BAD_REQUEST,
                "File path cannot be empty.",
                ""
            };
            json j;
            protocol::to_json(j, res);
            write_response(j);
            return;
    }
    if((fsutils::is_directory(std::filesystem::path(req.second_argument)) &&
        fsutils::is_directory(std::filesystem::path(req.second_argument))) ||
        (fsutils::is_file(std::filesystem::path(req.second_argument)) &&
        fsutils::is_file(std::filesystem::path(req.second_argument)))) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::PRECONDITION_FAILED,
                "Incorrect paths.",
                ""
            };
            json j;
            protocol::to_json(j, res);
            write_response(j);
            return;
        }
    if(!fsutils::is_subpath(user_dir_, std::filesystem::path(req.first_argument)) 
        || !fsutils::is_subpath(user_dir_, std::filesystem::path(req.second_argument))) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::FORBIDDEN,
            "Access denied.",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
        return;
    }
    if(!(fsutils::move_file(std::filesystem::path(req.first_argument), std::filesystem::path(req.second_argument), false))) {
        protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::INTERNAL_SERVER_ERROR,
                "Cannot move files.",
                ""
            };
            json j;
            protocol::to_json(j, res);
            write_response(j);
            return;
    }
    protocol::Response res {
            protocol::statuses::OK,
            protocol::codes::OK,
            "File moved.",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
}
void Session::copy(protocol::Request& req) {
    if(state_ != SessionSate::READY) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::SERVICE_UNAVAILABLE,
            "Session not ready.",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
        return;
    }
    if(req.first_argument.empty() || req.second_argument.empty()) {
        protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::BAD_REQUEST,
                "File path cannot be empty.",
                ""
            };
            json j;
            protocol::to_json(j, res);
            write_response(j);
            return;
    }
    if((fsutils::is_directory(std::filesystem::path(req.second_argument)) &&
        fsutils::is_directory(std::filesystem::path(req.second_argument))) ||
        (fsutils::is_file(std::filesystem::path(req.second_argument)) &&
        fsutils::is_file(std::filesystem::path(req.second_argument)))) {
            protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::PRECONDITION_FAILED,
                "Incorrect paths.",
                ""
            };
            json j;
            protocol::to_json(j, res);
            write_response(j);
            return;
        }
    if(!fsutils::is_subpath(user_dir_, std::filesystem::path(req.first_argument)) 
        || !fsutils::is_subpath(user_dir_, std::filesystem::path(req.second_argument))) {
        protocol::Response res {
            protocol::statuses::ERROR,
            protocol::codes::FORBIDDEN,
            "Access denied.",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
        return;
    }
    if(!(fsutils::copy_file(std::filesystem::path(req.first_argument), std::filesystem::path(req.second_argument), false))) {
        protocol::Response res {
                protocol::statuses::ERROR,
                protocol::codes::INTERNAL_SERVER_ERROR,
                "Cannot copy files.",
                ""
            };
            json j;
            protocol::to_json(j, res);
            write_response(j);
            return;
    }
    protocol::Response res {
            protocol::statuses::OK,
            protocol::codes::OK,
            "File copied.",
            ""
        };
        json j;
        protocol::to_json(j, res);
        write_response(j);
}

void Session::sync(protocol::Request& req) {
    // Implementation of sync
}