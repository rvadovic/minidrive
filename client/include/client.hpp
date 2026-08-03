#pragma once

#include <atomic>
#include <deque>
#include <map>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>
#include "terminalNoEcho.hpp"
#include "terminalRaw.hpp"
#include "protocol/message.hpp"
#include "filesystem/utils.hpp"
#include "filesystem/partmeta.hpp"
#include "sync_manifest.hpp"
#include "sync_diff.hpp"

// Stores data of currently active file transfer
struct ActiveTransfer{
    uint32_t transfer_id; // ID used in partmeta_ database
    fsutils::FileMetadata fmeta; // File metadata
    std::filesystem::path partial_path; // Path of .part file (user/.partial/id.part)
    std::vector<protocol::ChunkInfo> chunks; // Sizes, indexes and hashes of chunks
    std::vector<bool> chunk_state; // Represents received/sent chunks
};

enum class ClientState {
    LOGIN, // Logging in with username
    READY, // Ready to process commands
    NEED_INPUT, // Needs input: (Y/n)
    AUTH, // Authentification - password 
    PROCESSING, // Client is waiting for server rsponse
    EXIT, // Exit has been called
    UPLOAD_INIT, // Upload request sent, waiting for response
    UPLOADING,  // Uploading chunks of data
    DOWNLOAD_INIT, // Download request sent, waiting for response
    DOWNLOADING, // Downloading chunks of data
    NEED_INPUT_RESUME_TRANSFER, // Resuming transfer
    SYNC_LISTING // SYNC listing request sent, waiting for the server's recursive file listing
};

// Which command filled batch_queue_ - decides how the SYNC listing response is turned into ops and
// what the closing summary line is called.
enum class BatchMode {
    NONE,
    SYNC,
    DOWNLOAD_DIR,
    PLAIN // UPLOAD_DIR and multi-argument DELETE/MOVE/COPY: the queue is built without a listing
};

class Client {
public:
    Client(const std::string& username, asio::io_context& io_context, std::shared_ptr<asio::executor_work_guard<asio::io_context::executor_type>> guard);
    ~Client();

    // Connect and start listening loop
    void connect(const std::string& host, uint16_t port);

    // Exit command
    void exit(); // Triggered by user, signals, async read/write error, session exit, sends notification to server, shutdown and close socket
private:
    std::string username_;
    asio::io_context& io_context_;
    asio::ip::tcp::socket socket_;
    asio::posix::stream_descriptor input_;
    std::shared_ptr<asio::executor_work_guard<asio::io_context::executor_type>> guard_;
    std::unordered_map<std::string, std::function<void(std::istringstream&)>> commands_; // Map of commands and their functions
    std::thread input_thread_; // Input loop runs in this thread
    uint32_t msg_len_; // Message lenght for json read loop
    std::string input_buffer_;
    std::vector<char> buffer_; // Buffer for json read loop
    protocol::ChunkHeader ch_; // Chunk header for binary read loop
    std::atomic<ClientState> state_ = ClientState::LOGIN; // State of client
    std::unique_ptr<TerminalNoEcho> password_guard_; // Turns off echo in cmd (non-tty / piped input path)
    std::atomic<bool> exiting_{false}; // Indicates exit has been called
    std::atomic<bool> reading_line_{false}; // The input is being read from the terminal (only one read at a time)

    // Interactive line editor (only used when stdin is a real terminal; piped input, e.g. in
    // integration tests, keeps using the plain async_read_until('\n') path unchanged below)
    static constexpr const char* PROMPT = "> ";
    bool is_tty_{false}; // Whether stdin is an interactive terminal
    std::unique_ptr<TerminalRaw> raw_guard_; // Puts the terminal in raw mode for the whole session (tty only)
    bool masked_{false}; // True while the current line shouldn't be echoed/recalled (password entry)
    std::string current_prompt_; // Prompt text refresh_line() redraws ("> ", or "Password for X: " during auth)
    std::string line_buffer_; // Current in-progress line (tty mode)
    size_t cursor_{0}; // Cursor position within line_buffer_
    char char_buf_{}; // Scratch buffer for one-byte-at-a-time async reads (tty mode)
    std::vector<std::string> history_; // Previously submitted commands (tty mode, in-memory only)
    size_t history_pos_{0}; // Position while browsing history_ (== history_.size() means "not browsing")
    std::string history_saved_; // In-progress line stashed while browsing, restored by History-down past the end

    enum class EscState { NONE, ESC, CSI };
    EscState esc_state_{EscState::NONE}; // Parser state for ANSI escape sequences (arrow/home/end/delete keys)
    std::string csi_params_; // Accumulated parameter bytes of the CSI sequence currently being parsed
    ActiveTransfer transfer_{UINT32_MAX, fsutils::FileMetadata{}, std::filesystem::path(""), {}, {}}; // Current active transfer info
    std::filesystem::path root_; // Client root
    std::optional<PartialMetadata> partmeta_; // Database for partial file metadata

    // Batch engine. One queue-draining mechanism shared by SYNC, UPLOAD_DIR/DOWNLOAD_DIR and the
    // multi-argument DELETE/MOVE/COPY forms: each op is driven through the ordinary single-item
    // request/response cycle, and stdin is only re-armed once the whole queue has drained.
    std::deque<SyncOp> batch_queue_; // Remaining ops
    bool batch_active_ = false; // True while the queue is being drained
    bool advancing_ = false; // Guards advance_batch_queue() against re-entry (see advance_batch_queue)
    bool advance_pending_ = false; // An op finished synchronously while dispatching, keep draining
    BatchMode batch_mode_ = BatchMode::NONE;
    std::string batch_label_; // Name of the batch in the summary line, e.g. "Sync"
    std::optional<SyncOp> current_op_; // Op currently in flight, needed for baseline accounting
    std::optional<SyncManifest> manifest_; // Baseline of the sync currently running
    std::filesystem::path sync_local_dir_; // Local root of the sync/directory transfer in progress
    std::string sync_remote_dir_; // Remote root of the sync/directory transfer in progress
    size_t batch_uploaded_ = 0;
    size_t batch_downloaded_ = 0;
    size_t batch_deleted_ = 0;
    size_t batch_moved_ = 0;
    size_t batch_copied_ = 0;
    size_t batch_dirs_ = 0;
    size_t batch_skipped_ = 0;
    size_t batch_failed_ = 0;
    std::vector<std::string> batch_conflicts_;

    // Print to stdout
    void print(int code, const std::string& message, bool prompt = true);

    // Prepare the client root directory (./data/client_root)
    void setup();

    // Input loop runs in input_thread, calls handle_request()
    void read_line();
    void input_loop();

    // Interactive line editor (tty mode only, see members above)
    void read_char(); // Reads a single byte from stdin, feeds it to process_char()
    void process_char(char c); // Line-editing state machine: printable chars, backspace, escape sequences
    void handle_csi_final(const std::string& params, char final_byte); // Dispatches a completed CSI escape sequence
    void refresh_line(); // Redraws current_prompt_ + line_buffer_ in place
    void history_prev(); // Up arrow: recall older history entry
    void history_next(); // Down arrow: recall newer history entry / return to in-progress line

    // Read loop and write using json protocol for communication
    void read_header_json(); // read header of json message using async_read, call read_body_json()
    void read_body_json(); // read message of json message using async_read, call handle_response(), then read_next()
    void send_json(const nlohmann::json& j); // send json message using async_write

    // Special write which calls finish_exit()
    void send_json_exit(const nlohmann::json& j);

    // Read loop and write using binary protocol for chunk transfer
    void read_header_chunk(); // Read header of binary data using async_read, call read_body_chunk()
    void read_body_chunk(); // Read data of binary data using async_read, call handle_
    void send_chunk(const protocol::ChunkHeader& ch, const std::vector<uint8_t>& data); // Send data of binary data using async_write

    // Special write which calls finish_exit();
    void send_chunk_exit(const protocol::ChunkHeader& ch, const std::vector<uint8_t>& data);


    // Protocol switch between binary data and json message based on state_
    void read_next();

    // Handlers
    void handle_error(const std::error_code& ec); // Handles error codes of async operations
    void handle_response(const nlohmann::json& j); // Handles json response message from server
    void handle_request(const std::string& line); // Handles string request command from user
    void handle_chunk(const protocol::ChunkHeader& ch, const std::vector<uint8_t>& data); // Handles recieved binary data from server, calls transfer functions

    // Closes socket and on exit removes itself from sessions_ list in server
    void finish_exit();

    // Automatic operations
    void login(); // Send login request to server with username triggered by connect()
    void auth(const std::string password); // Handle authentifiction of user triggered by server response
    void need_input(const std::string input); // Handle (Y/n) question triggered by server response

    // Commands
    void cmd_help(std::istringstream& iss); // List command-function map keys
    void cmd_list(std::istringstream& iss); // Send request to server
    void cmd_upload(std::istringstream& iss); // Handle command args, verify local path, compute chunks and file metadata, create transfer_, send request to server 
    void cmd_delete(std::istringstream& iss); // Send request to server
    void cmd_download(std::istringstream& iss); // Prepare local destination path, send request to server
    void cmd_cd(std::istringstream& iss); // Send request to server
    void cmd_mkdir(std::istringstream& iss); // Send request to server
    void cmd_rmdir(std::istringstream& iss); // Send request to server
    void cmd_move(std::istringstream& iss); // Send request to server
    void cmd_copy(std::istringstream& iss); // Send request to server
    void cmd_sync(std::istringstream& iss); // Ask the server for a recursive listing, then diff and drive a batch
    void cmd_upload_dir(std::istringstream& iss); // Walk a local directory and queue mkdir/upload ops
    void cmd_download_dir(std::istringstream& iss); // Fetch a recursive listing and queue download ops

    // Command bodies, split from argument parsing so the batch engine can drive the very same
    // single-item request/response cycle the interactive commands use
    void do_list(const std::string& remote);
    void do_upload(const std::filesystem::path& local, const std::string& remote);
    void do_download(const std::string& remote, const std::filesystem::path& local, bool allow_overwrite);
    void do_delete(const std::string& remote);
    void do_mkdir(const std::string& remote);
    void do_rmdir(const std::string& remote);
    void do_move(const std::string& remote_from, const std::string& remote_to);
    void do_copy(const std::string& remote_from, const std::string& remote_to);
    void send_command(const std::string& cmd, const std::string& first, const std::string& second); // One-shot request, state_ = PROCESSING

    void batch_move_or_copy(std::istringstream& iss, bool is_move); // Shared MOVE/COPY parser, single or batched

    // Batch engine
    void begin_batch(BatchMode mode, const std::string& label, std::vector<SyncOp> ops); // Load the queue, reset counters
    void advance_batch_queue(); // Dispatch the next op, or finish the batch when the queue is empty
    void finish_batch(); // Persist the baseline, print the summary, re-arm stdin
    void command_finished(bool success); // Terminal point of any command: advance the batch or read the next line
    void record_op_result(); // Count the finished op and update the baseline
    void handle_sync_listing(const protocol::Response& res); // Turn a recursive listing into a queue of ops
    bool scan_local_tree(const std::filesystem::path& dir, std::map<std::string, SyncEntry>& out); // Recursive local scan, relative-path keyed

    // Upload
    void upload_init(); // Create partial metadata  entry in partmeta_, call uploading()
    void uploading(); // Pick chunk to send, read chunk of file and send it, call send_chunk()
    void upload_done(); // Delete partial file metadata, nullify transfer_, state_ = READY
    void upload_abort(bool save, bool notify, uint8_t flag); // Delete or save partial file metadta, notify server with protocol::flag
    void upload_abort_exit(bool save, bool notify, uint8_t flag); // calls finish_exit()

    // Download
    bool valid_file(const std::filesystem::path& partial_file, const std::array<uint8_t, crypto_generichash_BYTES>& expected); // Compute hash of full file after download and compare with expected
    bool valid_chunk(const uint32_t& index, const uint32_t& size, const std::vector<uint8_t>& data); // Compute hash of chunk data and compare with expected
    void download_init(const std::vector<protocol::ChunkInfo>& chunks, const std::string& file_hash); // Prepare transfer_ data, call downloading()
    void download_prepare_partmeta(); // Create partial metadata entry in database partmeta_ (client_root/.partial/partmeta.json) and create .part file
    void downloading(const uint32_t& index, const uint32_t& size, const std::vector<uint8_t>& data, uint8_t flag); // Systemtically sends one chunk, is called by handle_chunk()
    void download_done(); // Delete partial metadata from database partmeta_ (client_root/.partial/partmeta.json)
    void download_abort(bool save, bool notify, uint8_t flag); // Delete or save partial file metadata, delete .part file, notify server with protocol::flag
    void download_abort_exit(bool save, bool notify, uint8_t flag); // calls finish_exit()

    // Set cmd for password entry
    void on_password_required();
};