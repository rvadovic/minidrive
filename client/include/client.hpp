#pragma once

#include <atomic>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>
#include "terminalNoEcho.hpp"

enum class ClientState {
    LOGIN,
    READY,
    NEED_INPUT,
    AUTH,
    PROCESSING,
    EXIT,
    TRANSFERRING
};

class Client {
public:
    Client(const std::string& username, asio::io_context& io_context);
    ~Client();
    void connect(const std::string& host, uint16_t port);
    void exit();
private:
    std::string username_;
    asio::io_context& io_context_;
    asio::ip::tcp::socket socket_;
    std::unordered_map<std::string, std::function<void(std::istringstream&)>> commands_;
    std::thread input_thread_;
    uint32_t msg_len_;
    std::vector<char> buffer_;
    std::atomic<ClientState> state_ = ClientState::LOGIN;
    std::unique_ptr<TerminalNoEcho> password_guard_;
    std::atomic<bool> exiting_{false};

    void input_loop();
    void read_header();
    void read_body();
    void send(const nlohmann::json& j);
    void handle_error(const std::error_code& ec);
    void handle_response(const nlohmann::json& j);
    void handle_request(const std::string& line);
    void login();
    void auth(const std::string password);
    void need_input(const std::string input);
    void cmd_help(std::istringstream& iss);
    void cmd_list(std::istringstream& iss);
    void cmd_upload(std::istringstream& iss);
    void cmd_download(std::istringstream& iss);
    void cmd_delete(std::istringstream& iss);
    void cmd_cd(std::istringstream& iss);
    void cmd_mkdir(std::istringstream& iss);
    void cmd_rmdir(std::istringstream& iss);
    void cmd_move(std::istringstream& iss);
    void cmd_copy(std::istringstream& iss);
    void cmd_sync(std::istringstream& iss);
    void on_password_required();
};