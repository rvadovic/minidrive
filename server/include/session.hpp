#pragma once

#include <asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>
#include "protocol/message.hpp"
#include "database.hpp"

enum class SessionSate {
    AUTH,
    OK,
    ERROR,
    CONFLICT,
    NEED_INPUT_REGISTER,
    NEED_INPUT_RESUME_UPDATE,
    BUSY,
    LOGIN,
    READY,
    EXIT,
    SETUP
};

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(asio::ip::tcp::socket socket, const std::filesystem::path& root, std::shared_ptr<Database> db);
    void start();
private:
    asio::ip::tcp::socket socket_;
    std::filesystem::path root_;
    std::shared_ptr<Database> db_;
    std::unordered_map<std::string, std::function<void(protocol::Request&)>> requests_;
    std::vector<char> buffer_;
    uint32_t msg_len_;
    SessionSate state_ = SessionSate::LOGIN;
    std::filesystem::path current_dir_;
    std::filesystem::path user_dir_;
    std::string username_;

    void read_header();
    void read_body();
    void handle_error(const std::error_code& ec);
    void handle_request(const nlohmann::json& j);
    void write_response(const nlohmann::json& j);
    void login(protocol::Request& req);
    void setup_dir();
    void auth(protocol::Request& req);
    void need_input(protocol::Request& req);
    void exit(protocol::Request& req);
    void list(protocol::Request& req);
    void delete_file(protocol::Request& req);
    void upload(protocol::Request& req);
    void download(protocol::Request& req);
    void cd(protocol::Request& req);
    void mkdir(protocol::Request& req);
    void rmdir(protocol::Request& req);
    void move(protocol::Request& req);
    void copy(protocol::Request& req);
    void sync(protocol::Request& req);

};