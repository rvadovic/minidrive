#include <asio.hpp>
#include "session.hpp"
#include "server.hpp"
#include "filesystem/utils.hpp"
#include <vector>
#include <iostream>
#include "database.hpp"

using asio::ip::tcp;


Server::Server(asio::io_context& io_context, std::uint16_t port, const std::filesystem::path& root)
    : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)), root_(root) {
}
void Server::start(){
    setup_root_directory();
    accept();
}

void Server::accept() {
    acceptor_.async_accept([this](std::error_code ec, tcp::socket socket) {
        if (!ec) {
            auto session = std::make_shared<Session>(std::move(socket), root_, db_);
            session->start();
        }
        accept();
    });
}

void Server::setup_root_directory() {
    if(!fsutils::is_file(std::filesystem::path(root_ / "users.json"))) {
        fsutils::create_empty_file(std::filesystem::path(root_ / "users.json"));
        std::cout << "Created users.json file in root directory." << std::endl;
        db_ = std::make_shared<Database>(std::filesystem::path(root_ / "users.json"));
    }
    if(!fsutils::is_directory(std::filesystem::path(root_ / "public"))) {
        fsutils::mkdir(std::filesystem::path(root_ / "public"));
        std::cout << "Created public directory in root directory." <<  std::endl;
    }
    if(!fsutils::is_directory(std::filesystem::path(root_ / "private"))) {
        fsutils::mkdir(std::filesystem::path(root_ / "private"));
        std::cout << "Created private directory in root directory." << std::endl;
    }
    std::cout << "Root directory is set up" << std::endl;
}
