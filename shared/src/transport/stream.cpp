#include "transport/stream.hpp"

#include <asio/connect.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <cstring>
#include <limits>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include "filesystem/utils.hpp"

// The chunk bound is the protocol's own chunk size; if that ever changes, this must move with it
// or legitimate transfers start being rejected as oversized.
static_assert(transport::MAX_CHUNK_PAYLOAD == fsutils::CHUNK_SIZE,
              "MAX_CHUNK_PAYLOAD must match fsutils::CHUNK_SIZE");

namespace transport {

void IStream::async_write_all(Payload payload, WriteHandler handler) {
    write_queue_.push_back(QueuedWrite{std::move(payload), std::move(handler)});
    if(!writing_) {
        write_next();
    }
}

void IStream::write_next() {
    if(write_queue_.empty()) {
        writing_ = false;
        return;
    }

    writing_ = true;

    // Keeping the stream alive for the duration of its own write means a Session that drops its
    // last reference mid-write cannot pull the buffer out from under the kernel.
    auto self = shared_from_this();
    auto payload = write_queue_.front().payload;

    async_write_impl(asio::buffer(*payload), [this, self, payload](const std::error_code& ec, std::size_t n) {
        // Only this lambda ever pops; handlers can only push_back. So the entry started above is
        // still the front, and a handler that enqueues while this one runs is simply picked up by
        // the write_next() below (writing_ is still true, so it cannot start a second write).
        auto handler = std::move(write_queue_.front().handler);
        write_queue_.pop_front();

        if(ec) {
            // Nothing queued behind a failed write can succeed, and issuing more writes on a dead
            // socket only produces more errors: report the failure to everyone still waiting.
            //
            // Detach the queue *before* running any handler. Handlers routinely write again on
            // failure (handle_error -> exit() -> send the goodbye), and with writing_ cleared such
            // a write starts immediately - if it were still sitting in write_queue_ when this
            // drained it, its handler would be called here while its write was still in flight,
            // and that in-flight completion would then pop an empty deque.
            writing_ = false;
            std::deque<QueuedWrite> pending;
            pending.swap(write_queue_);

            if(handler) handler(ec, n);
            fail_queue(std::move(pending), ec);
            return;
        }

        if(handler) handler(ec, n);
        write_next();
    });
}

void IStream::fail_queue(std::deque<QueuedWrite> pending, const std::error_code& ec) {
    for(auto& queued : pending) {
        if(queued.handler) queued.handler(ec, 0);
    }
}

Payload frame_json(const std::string& body) {
    auto payload = std::make_shared<std::vector<uint8_t>>();
    payload->resize(sizeof(uint32_t) + body.size());

    const uint32_t len = htonl(static_cast<uint32_t>(body.size()));
    std::memcpy(payload->data(), &len, sizeof(uint32_t));
    if(!body.empty()) {
        std::memcpy(payload->data() + sizeof(uint32_t), body.data(), body.size());
    }
    return payload;
}

Payload frame_chunk(const protocol::ChunkHeader& ch, const std::vector<uint8_t>& data) {
    protocol::ChunkHeader header{};
    header.transfer_id = htonl(ch.transfer_id);
    header.index = htonl(ch.index);
    header.size = htonl(ch.size);
    header.flags = ch.flags;

    auto payload = std::make_shared<std::vector<uint8_t>>();
    payload->resize(sizeof(protocol::ChunkHeader) + data.size());

    std::memcpy(payload->data(), &header, sizeof(protocol::ChunkHeader));
    if(!data.empty()) {
        std::memcpy(payload->data() + sizeof(protocol::ChunkHeader), data.data(), data.size());
    }
    return payload;
}

PlainStream::PlainStream(asio::ip::tcp::socket socket)
    : socket_(std::move(socket)) {}

void PlainStream::async_read_exact(void* dest, std::size_t n, ReadHandler handler) {
    asio::async_read(socket_, asio::buffer(dest, n), std::move(handler));
}

void PlainStream::async_write_impl(const asio::const_buffer& buffer, WriteHandler handler) {
    asio::async_write(socket_, buffer, std::move(handler));
}

void PlainStream::async_connect(const asio::ip::tcp::resolver::results_type& endpoints,
                                ConnectHandler handler) {
    asio::async_connect(socket_, endpoints,
        [handler = std::move(handler)](const std::error_code& ec, const asio::ip::tcp::endpoint&) {
            handler(ec);
        });
}

void PlainStream::async_server_handshake(ConnectHandler handler) {
    // Plain TCP negotiates nothing. Posting rather than calling straight through keeps the
    // callback's context identical to the TLS case.
    asio::post(socket_.get_executor(), [handler = std::move(handler)] {
        handler(std::error_code{});
    });
}

bool PlainStream::is_open() const {
    return socket_.is_open();
}

void PlainStream::shutdown(std::error_code& ec) {
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
}

void PlainStream::close(std::error_code& ec) {
    socket_.close(ec);
}

void PlainStream::cancel(std::error_code& ec) {
    socket_.cancel(ec);
}

asio::any_io_executor PlainStream::get_executor() {
    return socket_.get_executor();
}

#ifdef MINIDRIVE_ENABLE_TLS

TlsStream::TlsStream(asio::ip::tcp::socket socket, asio::ssl::context& ctx)
    : stream_(std::move(socket), ctx) {}

void TlsStream::async_read_exact(void* dest, std::size_t n, ReadHandler handler) {
    asio::async_read(stream_, asio::buffer(dest, n), std::move(handler));
}

void TlsStream::async_write_impl(const asio::const_buffer& buffer, WriteHandler handler) {
    asio::async_write(stream_, buffer, std::move(handler));
}

void TlsStream::async_connect(const asio::ip::tcp::resolver::results_type& endpoints,
                              ConnectHandler handler) {
    auto self = shared_from_this();
    asio::async_connect(stream_.lowest_layer(), endpoints,
        [this, self, handler = std::move(handler)](const std::error_code& ec, const asio::ip::tcp::endpoint&) mutable {
            if(ec) {
                handler(ec);
                return;
            }
            stream_.async_handshake(asio::ssl::stream_base::client,
                [self, handler = std::move(handler)](const std::error_code& hs_ec) {
                    handler(hs_ec);
                });
        });
}

void TlsStream::async_server_handshake(ConnectHandler handler) {
    auto self = shared_from_this();
    stream_.async_handshake(asio::ssl::stream_base::server,
        [self, handler = std::move(handler)](const std::error_code& ec) {
            handler(ec);
        });
}

bool TlsStream::is_open() const {
    return stream_.lowest_layer().is_open();
}

void TlsStream::shutdown(std::error_code& ec) {
    // Best effort close_notify; the TCP shutdown below is what actually ends the connection.
    stream_.shutdown(ec);
    std::error_code tcp_ec;
    stream_.lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, tcp_ec);
    if(!ec) ec = tcp_ec;
}

void TlsStream::close(std::error_code& ec) {
    stream_.lowest_layer().close(ec);
}

void TlsStream::cancel(std::error_code& ec) {
    stream_.lowest_layer().cancel(ec);
}

asio::any_io_executor TlsStream::get_executor() {
    return stream_.lowest_layer().get_executor();
}

#endif // MINIDRIVE_ENABLE_TLS

} // namespace transport
