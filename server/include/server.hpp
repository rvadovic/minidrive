#pragma once

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <cstdint>
#include <unordered_set>
#include <mutex>
#include "storage.hpp"
#include "session.hpp"

// Manages sockets, aceppts new connecions
class Server {
public:
    Server(asio::io_context& io_context, std::uint16_t port, StorageConfig config);
    void start(); // Start accepting
    void exit_all_sessions(); // Exit all sessions, triggered by signal

private:
    asio::ip::tcp::acceptor acceptor_;
    std::shared_ptr<Storage> storage_; // Manages server storage, locks per user
    std::unordered_set<std::shared_ptr<Session>> sessions_; // Set of active sessions
    std::mutex sessions_mutex_; // Mutex for sessions_
    bool exit = false;
    void accept();

    void remove_session(std::shared_ptr<Session> session);
    void add_session(std::shared_ptr<Session> session);
};