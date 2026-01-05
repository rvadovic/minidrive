#include <asio.hpp>
#include "session.hpp"
#include "server.hpp"
#include "filesystem/utils.hpp"
#include <vector>
#include <mutex>
#include <unordered_set>
#include <iostream>
#include "storage.hpp"

using asio::ip::tcp;


Server::Server(asio::io_context& io_context, std::uint16_t port, const std::filesystem::path& root)
    : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
    storage_ = std::make_shared<Storage>(root);
}
void Server::start(){
    storage_->setup();
    accept();
}

void Server::exit_all_sessions() {
    exit = true;
    acceptor_.close();
    std::vector<std::shared_ptr<Session>> sessions_copy;
    
    {
        std::lock_guard lock(sessions_mutex_);
        sessions_copy.assign(sessions_.begin(), sessions_.end());
    }

    for(std::shared_ptr session: sessions_copy) {
        session->exit();
    }
}

void Server::accept() {
    acceptor_.async_accept([this](std::error_code ec, tcp::socket socket) {
        if (!ec) {
            auto session = std::make_shared<Session>(std::move(socket), storage_, [this](std::shared_ptr<Session> s) {
                remove_session(s); // Session will remove itsefl from sessions_ on exit
            });
            add_session(session);
            session->start();

        }
        if(!exit) accept();
    });
}

void Server::add_session(std::shared_ptr<Session> session) {
    std::lock_guard lock(sessions_mutex_);
    sessions_.insert(session);
}

void Server::remove_session(std::shared_ptr<Session> session) {
    std::lock_guard lock(sessions_mutex_);
    sessions_.erase(session);
}
