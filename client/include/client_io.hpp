#pragma once

#include <functional>
#include <memory>
#include <string>

#include <asio/io_context.hpp>
#include <nlohmann/json.hpp>

// The client's console seam, in the same spirit as transport::IStream: the state machine talks to
// an IClientIo instead of to stdin/stdout, so *where* commands come from and *where* results go is
// chosen once, in main(), and no command handler knows the difference.
//
// Two implementations:
//   - TerminalIo (POSIX only) - the interactive CLI and the piped-stdin path the integration
//     suites drive. Byte-for-byte the behaviour that shipped before this seam existed.
//   - IpcIo (portable) - the same conversation as length-prefixed JSON frames over a Unix domain
//     socket (named pipe on Windows), for the Tauri sidecar. This is the whole reason the client
//     builds on Windows and macOS at all: the unportable half is the terminal, and on a headless
//     build it simply isn't compiled.
namespace clientio {

// What the client is waiting for, so a GUI can render the right control. TerminalIo ignores this
// entirely - it prompts from result()/begin_password() exactly as it always has.
enum class PromptKind {
    Command,  // an ordinary command line
    Password, // the account password
    Confirm   // a y/n answer to a question the server asked
};

class IClientIo {
public:
    // One complete line of input, already stripped of its framing.
    using LineHandler = std::function<void(std::string)>;
    // The input channel ended (stdin EOF, or the GUI closed the socket).
    using CloseHandler = std::function<void()>;

    virtual ~IClientIo() = default;

    // Opens the channel. Returns false and fills `error` if it cannot be opened at all, which is a
    // startup failure rather than something to report through the channel itself.
    virtual bool start(std::string& error) = 0;

    virtual void set_handlers(LineHandler on_line, CloseHandler on_close) = 0;

    // Delivers exactly one line to the line handler. Callers guard against overlapping calls
    // (Client::reading_line_), so this is only ever armed once at a time.
    virtual void read_line(PromptKind kind) = 0;

    // Stops any pending input operation. Called from Client::exit().
    virtual void cancel() = 0;

    // The OK:/ERROR: protocol contract. `prompt` asks for the input prompt to be redrawn after it,
    // which only means anything on a terminal.
    virtual void result(int code, const std::string& message, bool prompt) = 0;

    // Free-form output that is not an OK/ERROR line: listings, vault status, conflict notices.
    virtual void info(const std::string& text) = 0;

    // A push notification with no request behind it - transfer progress, mostly. A terminal has
    // nowhere sensible to put these mid-line, so TerminalIo drops them; that keeps the CLI's
    // output identical to what the integration suites already parse.
    virtual void event(const nlohmann::json& payload) = 0;

    // Password entry brackets: echo off / masked input in between.
    virtual void begin_password(const std::string& prompt) = 0;
    virtual void end_password() = 0;

    // Name of this channel, for the startup log line.
    virtual std::string describe() const = 0;
};

// Terminal implementation. Only declared - and only compiled - where there is a terminal to drive.
#ifdef MINIDRIVE_INTERACTIVE_CLI
std::unique_ptr<IClientIo> make_terminal_io(asio::io_context& io_context);
#endif

// IPC implementation. `endpoint` is a Unix domain socket path, or a named pipe name on Windows
// (either "\\.\pipe\foo" or a bare "foo"). The endpoint must already exist: the host process owns
// it and the client connects, which keeps socket lifetime out of the client entirely.
std::unique_ptr<IClientIo> make_ipc_io(asio::io_context& io_context, const std::string& endpoint);

} // namespace clientio
