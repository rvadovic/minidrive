#pragma once

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <cstdint>
#include <unordered_set>
#include <mutex>
#include "storage.hpp"
#include "session.hpp"
#include "transport/tls.hpp"

// Manages sockets, aceppts new connecions
class Server {
public:
    // `streams` decides the security posture of every accepted connection (rung 0 or rung 3.5).
    // It is built and validated in main() before the port is bound, so a misconfigured certificate
    // is a startup failure rather than a per-connection one.
    Server(asio::io_context& io_context, std::uint16_t port, StorageConfig config,
           std::shared_ptr<transport::StreamFactory> streams);
    bool start(); // Prepare storage and start accepting; false if storage could not be set up
    void exit_all_sessions(); // Exit all sessions, triggered by signal

private:
    asio::ip::tcp::acceptor acceptor_;
    std::shared_ptr<transport::StreamFactory> streams_; // Wraps each accepted socket for the active rung
    std::shared_ptr<Storage> storage_; // Manages server storage, locks per user
    std::unordered_set<std::shared_ptr<Session>> sessions_; // Set of active sessions
    std::mutex sessions_mutex_; // Mutex for sessions_
    bool exit = false;
    void accept();

    void remove_session(std::shared_ptr<Session> session);
    void add_session(std::shared_ptr<Session> session);
};