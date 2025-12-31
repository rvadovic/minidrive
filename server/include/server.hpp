#pragma once

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <cstdint>
#include "database.hpp"

class Server {
public:
    Server(asio::io_context& io_context, std::uint16_t port, const std::filesystem::path& root);
    void start();

private:
    asio::ip::tcp::acceptor acceptor_;
    const std::filesystem::path root_;
    std::shared_ptr<Database> db_;
    void accept();
    void setup_root_directory();
};