#pragma once

#include <asio/any_io_executor.hpp>
#include <asio/buffer.hpp>
#include <asio/ip/tcp.hpp>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "protocol/message.hpp"

#ifdef MINIDRIVE_ENABLE_TLS
#include <asio/ssl.hpp>
#endif

// The transport seam. Session and Client speak to an IStream instead of a tcp::socket, so the
// security posture ("rung") is chosen at runtime by picking an implementation, and neither state
// machine has to know which one is underneath. Virtual rather than templated on purpose: --rung is
// a runtime value, and templating both state machines over the stream type would duplicate ~3,700
// lines of instantiation to save one std::function hop per 256 KB of payload.
namespace transport {

// A framed, ready-to-send message. Header and body are flattened into one owned buffer, so the
// stream can hold the bytes alive for the whole write itself - no call site can hand the kernel a
// pointer into a freed buffer (that is Known Bug #10's entire failure mode, now unreachable).
using Payload = std::shared_ptr<const std::vector<uint8_t>>;

// Upper bounds on what a peer may make us allocate from a length field it controls. Without these,
// four attacker-chosen bytes ask for a 4 GiB allocation before a single byte of body is read.
inline constexpr uint32_t MAX_MESSAGE_SIZE = 64u * 1024u * 1024u; // JSON control message body
inline constexpr uint32_t MAX_CHUNK_PAYLOAD = 256u * 1024u;       // one binary chunk == fsutils::CHUNK_SIZE

class IStream : public std::enable_shared_from_this<IStream> {
public:
    using ReadHandler = std::function<void(const std::error_code&, std::size_t)>;
    using WriteHandler = std::function<void(const std::error_code&, std::size_t)>;
    using ConnectHandler = std::function<void(const std::error_code&)>;

    virtual ~IStream() = default;

    // Reads exactly n bytes into dest. dest must outlive the operation; every call site keeps its
    // owning shared_ptr alive by capturing it in the handler, exactly as it did with async_read.
    virtual void async_read_exact(void* dest, std::size_t n, ReadHandler handler) = 0;

    // Queues one whole message. Writes never overlap: the next one is started from the previous
    // one's completion. asio::ssl::stream forbids overlapping writes outright, and even on plain
    // TCP two concurrent async_writes can interleave their buffers - which the server already does
    // today in `send_res(res); handle_resumes();`. Handlers run in submission order.
    void async_write_all(Payload payload, WriteHandler handler);

    // Client side: connect (and, for TLS, complete the client handshake) before any framing runs.
    virtual void async_connect(const asio::ip::tcp::resolver::results_type& endpoints,
                               ConnectHandler handler) = 0;

    // Server side: complete the server handshake for an accepted connection. Plain TCP has nothing
    // to negotiate and simply reports success, so Session::start() needs no rung-specific branch.
    virtual void async_server_handshake(ConnectHandler handler) = 0;

    virtual bool is_open() const = 0;
    virtual void shutdown(std::error_code& ec) = 0;
    virtual void close(std::error_code& ec) = 0;
    virtual void cancel(std::error_code& ec) = 0;
    virtual asio::any_io_executor get_executor() = 0;

protected:
    // Writes the whole buffer. The base class guarantees it is never called again until the
    // previous handler has run, which is what makes the TLS implementations legal.
    virtual void async_write_impl(const asio::const_buffer& buffer, WriteHandler handler) = 0;

private:
    struct QueuedWrite {
        Payload payload;
        WriteHandler handler;
    };

    void write_next();
    void fail_queue(std::deque<QueuedWrite> pending, const std::error_code& ec);

    std::deque<QueuedWrite> write_queue_;
    bool writing_ = false;
};

// Framing helpers: the wire formats themselves are unchanged, they are just built into one owned
// buffer instead of a scatter/gather list of caller-owned pieces.
Payload frame_json(const std::string& body);   // 4-byte big-endian length + JSON body
Payload frame_chunk(const protocol::ChunkHeader& ch, const std::vector<uint8_t>& data); // header + raw bytes

// Rung 0: plain TCP, byte for byte what the protocol has always sent. The baseline of the security
// ladder and the regression baseline for this seam.
class PlainStream : public IStream {
public:
    explicit PlainStream(asio::ip::tcp::socket socket);

    void async_read_exact(void* dest, std::size_t n, ReadHandler handler) override;
    void async_connect(const asio::ip::tcp::resolver::results_type& endpoints,
                       ConnectHandler handler) override;
    void async_server_handshake(ConnectHandler handler) override;
    bool is_open() const override;
    void shutdown(std::error_code& ec) override;
    void close(std::error_code& ec) override;
    void cancel(std::error_code& ec) override;
    asio::any_io_executor get_executor() override;

protected:
    void async_write_impl(const asio::const_buffer& buffer, WriteHandler handler) override;

private:
    asio::ip::tcp::socket socket_;
};

#ifdef MINIDRIVE_ENABLE_TLS
// Rungs 2-5: the same framing through asio::ssl::stream. Encryption happens inline on the existing
// io_context, inside the same completion handlers - no thread is added anywhere.
//
// NOT YET SELECTABLE. This is the stream adapter only; nothing constructs it yet because choosing
// and configuring an ssl::context (TLS version, chain validation, pinning, the X25519MLKEM768
// group) is step 7. It compiles and links here so step 7 is context configuration plus a --rung
// flag, with no further call-site churn.
class TlsStream : public IStream {
public:
    enum class Role { Client, Server };

    TlsStream(asio::ip::tcp::socket socket, asio::ssl::context& ctx);

    void async_read_exact(void* dest, std::size_t n, ReadHandler handler) override;
    void async_connect(const asio::ip::tcp::resolver::results_type& endpoints,
                       ConnectHandler handler) override;
    void async_server_handshake(ConnectHandler handler) override;
    bool is_open() const override;
    void shutdown(std::error_code& ec) override;
    void close(std::error_code& ec) override;
    void cancel(std::error_code& ec) override;
    asio::any_io_executor get_executor() override;

protected:
    void async_write_impl(const asio::const_buffer& buffer, WriteHandler handler) override;

private:
    asio::ssl::stream<asio::ip::tcp::socket> stream_;
};
#endif // MINIDRIVE_ENABLE_TLS

} // namespace transport
