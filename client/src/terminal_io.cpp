#include "client_io.hpp"

#ifdef MINIDRIVE_INTERACTIVE_CLI

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/posix/stream_descriptor.hpp>

#include "protocol/codes.hpp"
#include "terminalNoEcho.hpp"
#include "terminalRaw.hpp"

// The interactive CLI, lifted out of Client behind the IClientIo seam. Nothing here changed in the
// move: the tty line editor (history, CSI escape parsing, in-place redraw) and the piped-stdin
// async_read_until path are the same code they were when they lived in client.cpp, so the CLI's
// output - which the integration suites parse - is byte-for-byte what it was.
//
// This file is the *entire* POSIX-only surface of the client. A headless build leaves it out and
// needs no platform abstraction, because the unportable code simply isn't compiled.
namespace clientio {
namespace {

constexpr const char* PROMPT = "> ";

class TerminalIo : public IClientIo {
public:
    explicit TerminalIo(asio::io_context& io_context)
        : io_context_(io_context),
          input_(io_context, ::dup(STDIN_FILENO)) {
        is_tty_ = ::isatty(STDIN_FILENO) != 0;
        current_prompt_ = PROMPT;
    }

    bool start(std::string& /*error*/) override {
        // Raw mode is held for the whole session on a real terminal, so the editor sees individual
        // keystrokes rather than the kernel's line discipline.
        if(is_tty_) {
            raw_guard_ = std::make_unique<TerminalRaw>();
        }
        return true;
    }

    void set_handlers(LineHandler on_line, CloseHandler on_close) override {
        on_line_ = std::move(on_line);
        on_close_ = std::move(on_close);
    }

    void read_line(PromptKind /*kind*/) override {
        // A terminal shows what it wants by the prompt it already drew; the kind is only of use to
        // a GUI, which is why it is ignored here rather than changing any output.
        if(is_tty_) {
            read_char();
            return;
        }

        asio::async_read_until(input_, asio::dynamic_buffer(input_buffer_), '\n',
            [this](std::error_code ec, std::size_t length) {
                if(!ec) {
                    std::string line = input_buffer_.substr(0, length - 1); // remove '\n'
                    input_buffer_.erase(0, length);
                    deliver(std::move(line));
                } else if(ec != asio::error::operation_aborted) {
                    std::cerr << "Input error: " << ec.message() << "\n";
                    if(on_close_) on_close_();
                }
            });
    }

    void cancel() override {
        std::error_code ec;
        input_.cancel(ec);
    }

    void result(int code, const std::string& message, bool prompt) override {
        if(code == protocol::codes::OK) {
            std::cout << "OK" << ": " << message << std::endl;
        } else {
            std::cout << "ERROR" << ": " << "<" << code << ">" << " " << message << std::endl;
        }
        if(prompt) {
            std::cout << current_prompt_ << std::flush;
        }
    }

    void info(const std::string& text) override {
        std::cout << text << std::endl;
    }

    void event(const nlohmann::json& /*payload*/) override {
        // Dropped on purpose: a progress line printed into a half-typed command would corrupt both
        // the display and the OK/ERROR transcript the test harness reads.
    }

    void begin_password(const std::string& prompt) override {
        if(is_tty_) {
            masked_ = true;
            current_prompt_ = prompt;
            line_buffer_.clear();
            cursor_ = 0;
            std::cout << current_prompt_ << std::flush;
        } else {
            password_guard_ = std::make_unique<TerminalNoEcho>();
            std::cout << prompt << std::flush;
        }
    }

    void end_password() override {
        if(is_tty_) {
            masked_ = false;
            current_prompt_ = PROMPT;
        } else {
            password_guard_.reset();
            std::cout << std::endl;
        }
    }

    std::string describe() const override {
        return is_tty_ ? "terminal (interactive)" : "terminal (piped stdin)";
    }

private:
    // Hands a finished line to the client on the io_context, never from inside the read handler -
    // the client re-arms input from within the handler chain, and dispatching directly would nest
    // a whole command's worth of work inside the read that produced it.
    void deliver(std::string line) {
        if(!on_line_) return;
        asio::post(io_context_, [this, line = std::move(line)]() mutable {
            on_line_(std::move(line));
        });
    }

    void read_char() {
        asio::async_read(input_, asio::buffer(&char_buf_, 1),
            [this](std::error_code ec, std::size_t /*length*/) {
                if(ec) {
                    if(ec != asio::error::operation_aborted) {
                        std::cerr << "Input error: " << ec.message() << "\n";
                        if(on_close_) on_close_();
                    }
                    return;
                }
                process_char(char_buf_);
            });
    }

    void refresh_line() {
        std::string out = "\r\x1b[K" + current_prompt_;
        if(!masked_) {
            out += line_buffer_;
            size_t back = line_buffer_.size() - cursor_;
            if(back > 0) {
                out += "\x1b[" + std::to_string(back) + "D";
            }
        }
        std::cout << out << std::flush;
    }

    void history_prev() {
        if(masked_ || history_.empty()) return;
        if(history_pos_ == history_.size()) {
            history_saved_ = line_buffer_;
        }
        if(history_pos_ > 0) {
            history_pos_--;
            line_buffer_ = history_[history_pos_];
            cursor_ = line_buffer_.size();
            refresh_line();
        }
    }

    void history_next() {
        if(masked_ || history_pos_ >= history_.size()) return;
        history_pos_++;
        line_buffer_ = (history_pos_ == history_.size()) ? history_saved_ : history_[history_pos_];
        cursor_ = line_buffer_.size();
        refresh_line();
    }

    void handle_csi_final(const std::string& params, char final_byte) {
        if(!params.empty() && params.front() == 'O') { // ESC O <letter> (Home/End on some terminals)
            if(final_byte == 'H') { cursor_ = 0; refresh_line(); }
            else if(final_byte == 'F') { cursor_ = line_buffer_.size(); refresh_line(); }
            return;
        }

        switch(final_byte) {
            case 'A': history_prev(); break;
            case 'B': history_next(); break;
            case 'C': if(cursor_ < line_buffer_.size()) { cursor_++; refresh_line(); } break;
            case 'D': if(cursor_ > 0) { cursor_--; refresh_line(); } break;
            case 'H': cursor_ = 0; refresh_line(); break;
            case 'F': cursor_ = line_buffer_.size(); refresh_line(); break;
            case '~':
                if(params == "3" && cursor_ < line_buffer_.size()) { // Delete
                    line_buffer_.erase(cursor_, 1);
                    refresh_line();
                } else if(params == "1") { // Home
                    cursor_ = 0;
                    refresh_line();
                } else if(params == "4") { // End
                    cursor_ = line_buffer_.size();
                    refresh_line();
                }
                break;
            default: break;
        }
    }

    void process_char(char c) {
        unsigned char uc = static_cast<unsigned char>(c);

        if(esc_state_ == EscState::ESC) {
            if(uc == '[') {
                esc_state_ = EscState::CSI;
                csi_params_.clear();
            } else if(uc == 'O') {
                esc_state_ = EscState::CSI;
                csi_params_ = "O";
            } else {
                esc_state_ = EscState::NONE; // unrecognized escape, drop
            }
            read_char();
            return;
        }

        if(esc_state_ == EscState::CSI) {
            if(((uc >= '0' && uc <= '9') || uc == ';') && csi_params_.size() < 8) {
                csi_params_ += static_cast<char>(uc);
                read_char();
                return;
            }
            handle_csi_final(csi_params_, static_cast<char>(uc));
            esc_state_ = EscState::NONE;
            csi_params_.clear();
            read_char();
            return;
        }

        if(uc == '\x1b') {
            esc_state_ = EscState::ESC;
            read_char();
            return;
        }

        if(uc == '\r' || uc == '\n') {
            std::cout << "\r\n";
            std::string line = line_buffer_;
            if(!masked_ && !line.empty() && (history_.empty() || history_.back() != line)) {
                history_.push_back(line);
            }
            history_pos_ = history_.size();
            history_saved_.clear();
            line_buffer_.clear();
            cursor_ = 0;
            deliver(std::move(line));
            return;
        }

        if(uc == 127 || uc == 8) { // Backspace
            if(cursor_ > 0) {
                line_buffer_.erase(cursor_ - 1, 1);
                cursor_--;
                refresh_line();
            }
            read_char();
            return;
        }

        if(uc == 4) { // Ctrl-D
            if(line_buffer_.empty()) {
                std::cout << "\r\n";
                if(on_close_) on_close_();
                return;
            }
            read_char();
            return;
        }

        if(uc >= 32 && uc < 127) { // Printable
            line_buffer_.insert(line_buffer_.begin() + static_cast<std::string::difference_type>(cursor_),
                                static_cast<char>(uc));
            cursor_++;
            refresh_line();
        }

        read_char();
    }

    enum class EscState { NONE, ESC, CSI };

    asio::io_context& io_context_;
    asio::posix::stream_descriptor input_;
    LineHandler on_line_;
    CloseHandler on_close_;

    std::string input_buffer_;                        // Piped-stdin accumulator
    bool is_tty_{false};                              // Whether stdin is an interactive terminal
    std::unique_ptr<TerminalRaw> raw_guard_;          // Raw mode for the whole session (tty only)
    std::unique_ptr<TerminalNoEcho> password_guard_;  // Echo off during password entry (non-tty)
    bool masked_{false};                              // Current line is a password: don't echo or recall it
    std::string current_prompt_;                      // What refresh_line() redraws
    std::string line_buffer_;                         // Current in-progress line (tty mode)
    size_t cursor_{0};                                // Cursor position within line_buffer_
    char char_buf_{};                                 // One-byte-at-a-time read scratch (tty mode)
    std::vector<std::string> history_;                // Previously submitted commands (in-memory only)
    size_t history_pos_{0};                           // Browsing position (== size() means "not browsing")
    std::string history_saved_;                       // In-progress line stashed while browsing

    EscState esc_state_{EscState::NONE};              // ANSI escape parser state
    std::string csi_params_;                          // Parameter bytes of the CSI sequence in progress
};

} // namespace

std::unique_ptr<IClientIo> make_terminal_io(asio::io_context& io_context) {
    return std::make_unique<TerminalIo>(io_context);
}

} // namespace clientio

#endif // MINIDRIVE_INTERACTIVE_CLI
