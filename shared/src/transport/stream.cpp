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
#include "transport/tls.hpp"

#ifdef MINIDRIVE_ENABLE_TLS
#include <openssl/objects.h>
#include <openssl/ssl.h>
// Direct call when the accessor is known to be linkable: OpenSSL is embedded (static build), or
// there is no dlsym to reach it with (Windows). Otherwise resolve it at runtime - see below.
#if OPENSSL_VERSION_NUMBER >= 0x30200000L && (defined(MINIDRIVE_OPENSSL_STATIC) || defined(_WIN32))
#define MINIDRIVE_DIRECT_GROUP_NAME 1
#elif !defined(_WIN32)
#include <dlfcn.h> // to resolve accessors newer than the OpenSSL headers this was built against
#endif
#endif

// The chunk bound is the protocol's own chunk size; if that ever changes, this must move with it
// or legitimate transfers start being rejected as oversized.
static_assert(transport::MAX_CHUNK_PAYLOAD == fsutils::CHUNK_SIZE,
              "MAX_CHUNK_PAYLOAD must match fsutils::CHUNK_SIZE");

namespace transport {

#ifdef MINIDRIVE_ENABLE_TLS
namespace {

// Resolves the negotiated group's name through the libssl that is actually loaded, rather than the
// one this was compiled against.
//
// That distinction is not academic here: libssl is linked dynamically on purpose, so a package
// built against OpenSSL 3.0 routinely runs on a machine with 3.5 - and in exactly that case the
// hybrid group is available and gets negotiated, because SSL_CTX_set1_groups_list resolves group
// names at runtime too. SSL_get0_group_name only exists from OpenSSL 3.2 on, so deciding with a
// compile-time #if would leave that deployment unable to name the group it just used, and
// X25519MLKEM768 has no NID for the older accessor to fall back on. Verified by running
// 3.0-built binaries against a 3.5 libssl.
//
// A statically linked OpenSSL inverts that: dlsym has no libssl to search and returns null, which
// would reintroduce the vanished-group bug by a different route. There the version is fixed at link
// time, so the accessor is called directly (MINIDRIVE_DIRECT_GROUP_NAME).
const char* negotiated_group_name(SSL* ssl) {
#if defined(MINIDRIVE_DIRECT_GROUP_NAME)
    return SSL_get0_group_name(ssl);
#else
#ifndef _WIN32
    using GroupNameFn = const char* (*)(SSL*);
    static GroupNameFn get0_group_name =
        reinterpret_cast<GroupNameFn>(::dlsym(RTLD_DEFAULT, "SSL_get0_group_name"));
    if(get0_group_name != nullptr) {
        return get0_group_name(ssl);
    }
#endif
    const int nid = SSL_get_negotiated_group(ssl);
    return nid == NID_undef ? nullptr : OBJ_nid2sn(nid);
#endif
}

} // namespace
#endif // MINIDRIVE_ENABLE_TLS

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

std::string PlainStream::describe_connection() {
    return "plaintext TCP (rung 0)";
}

#ifdef MINIDRIVE_ENABLE_TLS

TlsStream::TlsStream(asio::ip::tcp::socket socket, std::shared_ptr<asio::ssl::context> ctx,
                     ClientHandshakeOptions opts)
    : ctx_(std::move(ctx)), stream_(std::move(socket), *ctx_), opts_(std::move(opts)) {}

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
            // SNI, hostname/IP verification and pinning are per-connection, so they go on the SSL
            // object here rather than on the shared context - and before the handshake, because
            // that is the only point at which they can still influence it.
            apply_client_handshake_options(stream_, opts_);
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
    // Send close_notify, but never wait for the peer's.
    //
    // asio's ssl::stream::shutdown() is synchronous and, left alone, calls SSL_shutdown a second
    // time to read the peer's close_notify - blocking this thread until it arrives. An
    // unresponsive peer would hold an io thread hostage indefinitely, and on the client there is
    // only one io thread, so that is the whole process. Marking the peer's close_notify as already
    // received makes the first SSL_shutdown complete the shutdown from our side: ours is written,
    // and the call returns rather than waiting for a reply that may never come.
    if(SSL* ssl = stream_.native_handle()) {
        SSL_set_shutdown(ssl, SSL_RECEIVED_SHUTDOWN);
    }
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

std::string TlsStream::describe_connection() {
    const SSL* ssl = stream_.native_handle();
    if(!ssl) return "TLS (not established)";

    std::string out = SSL_get_version(ssl);
    if(const char* cipher = SSL_get_cipher_name(ssl)) {
        out += ", cipher ";
        out += cipher;
    }

    // The negotiated group is the whole point of rung 3.5's post-quantum half, so it is reported
    // by name.
    SSL* mutable_ssl = const_cast<SSL*>(ssl);
    if(const char* group = negotiated_group_name(mutable_ssl)) {
        out += ", group ";
        out += group;
    } else if(const int id = SSL_get_negotiated_group(mutable_ssl); id != NID_undef) {
        // A libssl too old to name this group, but new enough to have negotiated it. Reporting the
        // raw TLS group id beats dropping the field: it still answers "which one did I get".
        out += ", group id " + std::to_string(id);
    }
    return out;
}

#endif // MINIDRIVE_ENABLE_TLS

} // namespace transport
