#include "client_io.hpp"

#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/connect.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include <spdlog/spdlog.h>

#include "protocol/codes.hpp"
#include "transport/stream.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <asio/windows/stream_handle.hpp>
#else
#include <asio/local/stream_protocol.hpp>
#endif

// The headless channel. Same conversation as the CLI - one command line in, OK/ERROR lines out -
// but carried as length-prefixed JSON frames over a Unix domain socket (named pipe on Windows)
// instead of a terminal.
//
// The framing is deliberately the one shared/src/transport/stream.cpp already implements for the
// wire protocol (4-byte big-endian length + JSON body), so the host has one framing to write, not
// two. This is IPC only: nothing here touches the client<->server protocol.
//
// The host owns the endpoint and the client connects to it. That keeps socket lifetime, permissions
// and cleanup entirely on the host side, where a supervising process can actually manage them, and
// means a dead client never leaves a stale socket behind.
namespace clientio {
namespace {

#if defined(_WIN32)
using IpcSocket = asio::windows::stream_handle;
#else
using IpcSocket = asio::local::stream_protocol::socket;
#endif

class IpcIo : public IClientIo {
public:
    IpcIo(asio::io_context& io_context, std::string endpoint)
        : io_context_(io_context), socket_(io_context), endpoint_(std::move(endpoint)) {}

    bool start(std::string& error) override {
        if(endpoint_.empty()) {
            error = "--ipc requires a socket path";
            return false;
        }

#if defined(_WIN32)
        // A bare name is taken as a pipe under the local pipe namespace, so a host can pass either
        // "minidrive-1234" or the fully qualified "\\.\pipe\minidrive-1234".
        std::string pipe_name = endpoint_;
        if(pipe_name.rfind("\\\\", 0) != 0) {
            pipe_name = "\\\\.\\pipe\\" + pipe_name;
        }
        // FILE_FLAG_OVERLAPPED is required: asio drives the handle through the IOCP completion
        // port, and a synchronous handle would silently never complete.
        HANDLE handle = ::CreateFileA(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if(handle == INVALID_HANDLE_VALUE) {
            error = "cannot open named pipe " + pipe_name + ": error " +
                    std::to_string(::GetLastError());
            return false;
        }
        std::error_code ec;
        socket_.assign(handle, ec);
        if(ec) {
            ::CloseHandle(handle);
            error = "cannot attach named pipe " + pipe_name + ": " + ec.message();
            return false;
        }
#else
        std::error_code ec;
        socket_.connect(asio::local::stream_protocol::endpoint(endpoint_), ec);
        if(ec) {
            error = "cannot connect to IPC socket " + endpoint_ + ": " + ec.message();
            return false;
        }
#endif
        open_ = true;
        read_frame(); // Read continuously from here on, so a host disconnect is noticed immediately
        return true;
    }

    void set_handlers(LineHandler on_line, CloseHandler on_close) override {
        on_line_ = std::move(on_line);
        on_close_ = std::move(on_close);
    }

    void read_line(PromptKind kind) override {
        prompt_kind_ = kind;
        if(!pending_lines_.empty()) {
            std::string line = std::move(pending_lines_.front());
            pending_lines_.pop_front();
            deliver(std::move(line));
            return;
        }
        // Nothing buffered, so say so: this frame is the host's cue that the client is idle and
        // what sort of answer it is waiting for.
        want_line_ = true;
        send(nlohmann::json{
            {"type", "PROMPT"},
            {"kind", kind_name(kind)},
            {"text", kind == PromptKind::Password ? password_prompt_ : std::string()}
        });
    }

    void cancel() override {
        want_line_ = false;
        if(!open_) return;
        std::error_code ec;
        socket_.cancel(ec);
    }

    void result(int code, const std::string& message, bool /*prompt*/) override {
        // The prompt is not derived from this flag: read_line() emits a PROMPT frame at the moment
        // input is actually armed, which is the only point where "waiting for you" is really true.
        send(nlohmann::json{
            {"type", "RESULT"},
            {"ok", code == protocol::codes::OK},
            {"code", code},
            {"message", message}
        });
    }

    void info(const std::string& text) override {
        send(nlohmann::json{{"type", "INFO"}, {"text", text}});
    }

    void event(const nlohmann::json& payload) override {
        nlohmann::json frame = payload;
        frame["type"] = "EVENT";
        send(frame);
    }

    void begin_password(const std::string& prompt) override {
        password_prompt_ = prompt;
    }

    void end_password() override {
        password_prompt_.clear();
    }

    std::string describe() const override {
        return "ipc (" + endpoint_ + ")";
    }

private:
    static const char* kind_name(PromptKind kind) {
        switch(kind) {
            case PromptKind::Password: return "password";
            case PromptKind::Confirm:  return "confirm";
            case PromptKind::Command:  break;
        }
        return "command";
    }

    void deliver(std::string line) {
        if(!on_line_) return;
        // Posted rather than called: the client re-arms input from inside its own handler chain,
        // and dispatching straight from a read would nest a command inside the read that fed it.
        asio::post(io_context_, [this, line = std::move(line)]() mutable {
            on_line_(std::move(line));
        });
    }

    void closed() {
        if(!open_) return;
        open_ = false;
        std::error_code ec;
        socket_.close(ec);
        if(on_close_) on_close_();
    }

    void read_frame() {
        if(!open_) return;
        asio::async_read(socket_, asio::buffer(&in_len_, sizeof(in_len_)),
            [this](std::error_code ec, std::size_t) {
                if(ec) {
                    if(ec != asio::error::operation_aborted) closed();
                    return;
                }
                uint32_t len = 0;
                std::memcpy(&len, &in_len_, sizeof(len));
                len = be32_to_host(len);
                // Same bound the wire protocol applies, for the same reason: four bytes the peer
                // chooses must not be able to ask for an arbitrary allocation.
                if(len == 0 || len > transport::MAX_MESSAGE_SIZE) {
                    spdlog::error("IPC frame length {} is out of range; closing the channel", len);
                    closed();
                    return;
                }
                in_body_.resize(len);
                asio::async_read(socket_, asio::buffer(in_body_.data(), in_body_.size()),
                    [this](std::error_code body_ec, std::size_t) {
                        if(body_ec) {
                            if(body_ec != asio::error::operation_aborted) closed();
                            return;
                        }
                        handle_frame(std::string(in_body_.begin(), in_body_.end()));
                        read_frame();
                    });
            });
    }

    void handle_frame(const std::string& body) {
        nlohmann::json j = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
        if(j.is_discarded() || !j.is_object()) {
            spdlog::warn("IPC: ignoring a frame that is not a JSON object");
            return;
        }

        const std::string type = j.value("type", std::string("COMMAND"));
        if(type != "COMMAND") {
            spdlog::warn("IPC: ignoring frame of unknown type '{}'", type);
            return;
        }

        std::string line = j.value("line", std::string());

        // Queued rather than dispatched. This is exactly how a terminal behaves with input piped
        // ahead of it, and the client depends on that: a queued EXIT must not be able to preempt a
        // transfer that is already running (see Known Bug #1). The line is only handed over when
        // the client asks for one.
        if(want_line_) {
            want_line_ = false;
            deliver(std::move(line));
            return;
        }
        pending_lines_.push_back(std::move(line));
    }

    static uint32_t be32_to_host(uint32_t value) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
        return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
               (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
    }

    void send(const nlohmann::json& frame) {
        if(!open_) return;
        write_queue_.push_back(transport::frame_json(frame.dump()));
        if(!writing_) write_next();
    }

    void write_next() {
        if(write_queue_.empty()) {
            writing_ = false;
            return;
        }
        writing_ = true;
        transport::Payload payload = write_queue_.front();
        asio::async_write(socket_, asio::buffer(payload->data(), payload->size()),
            [this, payload](std::error_code ec, std::size_t) {
                if(!write_queue_.empty()) write_queue_.pop_front();
                if(ec) {
                    writing_ = false;
                    write_queue_.clear();
                    if(ec != asio::error::operation_aborted) closed();
                    return;
                }
                write_next();
            });
    }

    asio::io_context& io_context_;
    IpcSocket socket_;
    std::string endpoint_;
    bool open_ = false;

    LineHandler on_line_;
    CloseHandler on_close_;

    uint32_t in_len_ = 0;
    std::vector<uint8_t> in_body_;
    std::deque<std::string> pending_lines_; // Commands received before the client asked for one
    bool want_line_ = false;                // The client is waiting and the queue was empty
    PromptKind prompt_kind_ = PromptKind::Command;
    std::string password_prompt_;

    // Writes are serialized the same way transport::IStream serializes them: one in flight, the
    // next started from the previous completion, with the payload owned for the whole write.
    std::deque<transport::Payload> write_queue_;
    bool writing_ = false;
};

} // namespace

std::unique_ptr<IClientIo> make_ipc_io(asio::io_context& io_context, const std::string& endpoint) {
    return std::make_unique<IpcIo>(io_context, endpoint);
}

} // namespace clientio
