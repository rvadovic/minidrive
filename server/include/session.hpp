#pragma once

#include <asio/ip/tcp.hpp>
#include <atomic>
#include <nlohmann/json.hpp>
#include "protocol/message.hpp"
#include "filesystem/partmeta.hpp"
#include "storage.hpp"
#include "database.hpp"

enum class SessionState {
    AUTH, // Authentification of user - password
    OK, // Operation succesful
    ERROR, // Error occured
    CONFLICT,
    NEED_INPUT_REGISTER, // Need input: (Y/n) for registration
    NEED_INPUT_RESUME_TREANSFER, //Need input: (Y/n) for resuming uploads/downloads
    NEED_INPUT_SET_TIER, // Need input: (Y/n) for confirming a storage tier change
    BUSY,
    LOGIN, // Logging in with given username
    READY, // Ready to execute commands
    EXIT, // Exit has been called
    SETUP,
    UPLOAD_INIT, // Upload command registered, preparing for upload
    UPLOADING, // Recieving uploaded data
    DOWNLOAD_INIT, // Download command registered, preparing for download
    DOWNLOADING, // Sending data
};

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(asio::ip::tcp::socket socket, std::shared_ptr<Storage> storage, std::function<void(std::shared_ptr<Session>)> on_exit);
    void start(); // Start listening loop
    void exit(); // Triggered by signals, server, client, sends notification to client or calls finish_exit()
private:
    asio::ip::tcp::socket socket_;
    std::filesystem::path root_; // Root of filesystem
    std::shared_ptr<Storage> storage_; // Handles operations on filesystem using one mutex per user
    std::function<void(std::shared_ptr<Session>)> on_exit_; // Removes this session from server list of sessions on exit
    std::shared_ptr<Database> db_; // Handles file-based database of user data
    std::shared_ptr<PartialMetadata> partmeta_; // Handles file-based database of user's partial file data
    std::unordered_map<std::string, std::function<void(protocol::Request&)>> requests_; // Map of commands and their executors
    std::vector<char> buffer_; // Buffer for json read loop
    uint32_t msg_len_; // Message length for json read loop
    protocol::ChunkHeader ch_; // Chunk header for binary read loop
    SessionState state_ = SessionState::LOGIN; // State of session
    std::filesystem::path current_dir_; // Current directory of session
    std::filesystem::path user_dir_; // Users root directory
    std::string username_; // Logged user
    ActiveTransfer transfer_{UINT32_MAX, fsutils::FileMetadata{}, std::filesystem::path(""), {}, {}}; // Transfer currently active
    std::atomic<bool> exiting_{false}; // Indicates exit has been called
    std::queue<PartialMetadataEntry> files_to_be_resumed; // Files to be resumed
    bool resuming_ = false; // True while working through files_to_be_resumed (gates handle_resumes() re-population)
    std::string pending_tier_; // Tier the user asked to move to, held while waiting for their (Y/n)

    // Read loop and write using json protocol for communication
    void read_header_json(); // read header of json message using async_read, call read_body_json()
    void read_body_json(); // read message of json message using async_read, call handle_response(), then read_next()
    void write_response_json(const nlohmann::json& j); // send json message using async_write

    // Special write which calls finish_exit()
    void write_response_json_exit(const nlohmann::json& j);

    // Read loop and write using binary protocol for chunk transfer
    void read_header_chunk(); // Read header of binary data using async_read, call read_body_chunk()
    void read_body_chunk(); // Read data of binary data using async_read, call handle_
    void send_chunk(const protocol::ChunkHeader& ch, const std::vector<uint8_t>& data); // Send data of binary data using async_write

    // Special write which calls finish_exit();
    void send_chunk_exit(const protocol::ChunkHeader& ch, const std::vector<uint8_t>& data);

    // Sending helper
    void send_res(protocol::Response& res);

    // Protocol switch between binary data and json message based on state_
    void read_next();

    // Closes socket and on exit removes itself from sessions_ list in server
    void finish_exit();

    // Handlers
    void handle_error(const std::error_code& ec); // Handles error codes of async operations
    void handle_request(const nlohmann::json& j);   // Handles json request message from client
    void handle_chunk(const protocol::ChunkHeader& ch, const std::vector<uint8_t>& data); // Handles received binary data from server
    void handle_resumes(); // Handles resumable transfers
    // Automatic operations
    void login(protocol::Request& req); // Handle users existance in db_ (root/users.json), send NEED_INPUT or AUTH response to client, decide public/private mode
    bool setup_dir(); // Set up user directory on their storage tier after succesful login, false means it already sent an error response
    void auth(protocol::Request& req); // Handle password in private mode, store in db_ (root/users.json), send BAD_REQUEST reponse when needed
    void need_input(protocol::Request& req); // Handle registration and resume transfer questions (Y/n)

    // Executes commands
    void list(protocol::Request& req); // Check arguments, acquire per user lock, execute using fsutils, release per user lock
    void delete_file(protocol::Request& req); // Check arguments, acquire per user lock, execute using fsutils, release per user lock
    void upload(protocol::Request& req); // Check arguments, prepare for upload, gather data from client, acquire  per user lock
    void download(protocol::Request& req); // Check arguments, prepare for download, send data to client, acquire per user lock
    void cd(protocol::Request& req); // Check arguments, set current_dir_
    void mkdir(protocol::Request& req); // Check arguments, acquire per user lock, execute using fsutils, release per user lock
    void rmdir(protocol::Request& req); // Check arguments, acquire per user lock, execute using fsutils, release per user lock
    void move(protocol::Request& req); // Check arguments, acquire per user lock, execute using fsutils, release per user lock
    void copy(protocol::Request& req); // Check arguments, acquire per user lock, execute using fsutils, release per user lock
    void sync(protocol::Request& req);
    void tiers(protocol::Request& req); // List storage media configured on the server, no lock needed
    void set_tier(protocol::Request& req); // Check arguments, acquire per user lock, ask for confirmation
    void finish_set_tier(); // Runs the migration after the user confirmed, releases per user lock

    // Upload - simular to clients download
    bool valid_file(const std::filesystem::path& partial_file, const std::array<uint8_t, crypto_generichash_BYTES>& expected);
    bool valid_chunk(const uint32_t& index, const uint32_t& size, const std::vector<uint8_t>& data);
    void upload_init();
    void uploading(const uint32_t& index, const uint32_t& size, const std::vector<uint8_t>& data, uint8_t flag);
    void upload_done(); // release per user lock
    void upload_abort(bool save, bool notify, uint8_t flag); // release per user lock
    void upload_abort_exit(bool save, bool notify, uint8_t flag); // calls finish_exit()

    // Download - simular to clients upload
    void download_init();
    void downloading();
    void download_done(); // release per user lock
    void download_abort(bool save, bool notify, uint8_t flag); // release per user lock
    void download_abort_exit(bool save, bool notify, uint8_t flag); // calls finish_exit()
};