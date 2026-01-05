#pragma once

#include <atomic>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>
#include "terminalNoEcho.hpp"
#include "protocol/message.hpp"
#include "filesystem/utils.hpp"
#include "filesystem/partmeta.hpp"

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
};

class Client {
public:
    Client(const std::string& username, asio::io_context& io_context);
    ~Client();

    // Connect and start listening loop
    void connect(const std::string& host, uint16_t port);

    // Exit command
    void exit(); // Triggered by user, signals, async read/write error, session exit, sends notification to server, shutdown and close socket
private:
    std::string username_;
    asio::io_context& io_context_;
    asio::ip::tcp::socket socket_;
    std::unordered_map<std::string, std::function<void(std::istringstream&)>> commands_; // Map of commands and their functions
    std::thread input_thread_; // Input loop runs in this thread
    uint32_t msg_len_; // Message lenght for json read loop
    std::vector<char> buffer_; // Buffer for json read loop
    protocol::ChunkHeader ch_; // Chunk header for binary read loop
    std::atomic<ClientState> state_ = ClientState::LOGIN; // State of client
    std::unique_ptr<TerminalNoEcho> password_guard_; // Turns off echo in cmd
    std::atomic<bool> exiting_{false}; // Indicates exit has been called
    ActiveTransfer transfer_{UINT32_MAX, fsutils::FileMetadata{}, std::filesystem::path(""), {}, {}}; // Current active transfer info
    std::filesystem::path root_; // Client root
    std::optional<PartialMetadata> partmeta_; // Database for partial file metadata

    // Prepare the client root directory (./data/client_root)
    void setup();

    // Input loop runs in input_thread, calls handle_request()
    void input_loop();

    // Read loop and write using json protocol for communication
    void read_header_json(); // read header of json message using async_read, call read_body_json()
    void read_body_json(); // read message of json message using async_read, call handle_response(), then read_next()
    void send_json(const nlohmann::json& j); // send json message using async_write

    // Read loop and write using binary protocol for chunk transfer
    void read_header_chunk(); // Read header of binary data using async_read, call read_body_chunk()
    void read_body_chunk(); // Read data of binary data using async_read, call handle_
    void send_chunk(const protocol::ChunkHeader& ch, const std::vector<uint8_t>& data); // Send data of binary data using async_write

    // Protocol switch between binary data and json message based on state_
    void read_next();

    // Handlers
    void handle_error(const std::error_code& ec); // Handles error codes of async operations
    void handle_response(const nlohmann::json& j); // Handles json response message from server
    void handle_request(const std::string& line); // Handles string request command from user
    void handle_chunk(const protocol::ChunkHeader& ch, const std::vector<uint8_t>& data); // Handles recieved binary data from server, calls transfer functions

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
    void cmd_sync(std::istringstream& iss);

    // Upload
    void upload_init(); // Create partial metadata  entry in partmeta_, call uploading()
    void uploading(); // Pick chunk to send, read chunk of file and send it, call send_chunk()
    void upload_done(); // Delete partial file metadata, nullify transfer_, state_ = READY
    void upload_abort(bool save, bool notify, uint8_t flag); // Delete or save partial file metadta, notify server with protocol::flag

    // Download
    bool valid_file(const std::filesystem::path& partial_file, const std::array<uint8_t, crypto_generichash_BYTES>& expected); // Compute hash of full file after download and compare with expected
    bool valid_chunk(const uint32_t& index, const uint32_t& size, const std::vector<uint8_t>& data); // Compute hash of chunk data and compare with expected
    void download_init(const std::vector<protocol::ChunkInfo>& chunks, const std::string& file_hash); // Prepare transfer_ data, call downloading()
    void download_prepare_partmeta(); // Create partial metadata entry in database partmeta_ (client_root/.partial/partmeta.json) and create .part file
    void downloading(const uint32_t& index, const uint32_t& size, const std::vector<uint8_t>& data, uint8_t flag); // Systemtically sends one chunk, is called by handle_chunk()
    void download_done(); // Delete partial metadata from database partmeta_ (client_root/.partial/partmeta.json)
    void download_abort(bool save, bool notify, uint8_t flag); // Delete or save partial file metadata, delete .part file, notify server with protocol::flag

    // Set cmd for password entry
    void on_password_required();
};