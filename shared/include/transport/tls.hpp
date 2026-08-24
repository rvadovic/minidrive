#pragma once

#include <memory>
#include <string>
#include <vector>

#include <asio/ip/tcp.hpp>

#include "transport/stream.hpp"

// Rung selection and TLS context construction. Everything that decides *which* security posture a
// process runs at lives here, so neither state machine, nor Server::accept(), nor Client::connect()
// contains a TLS branch: main() builds one StreamFactory at startup and the rest of the code just
// creates streams from it.
namespace transport {

// Where on the security ladder this process runs. Only the two ends of the ladder are built: rung 0
// (plaintext TCP - the regression baseline, and what every integration suite still runs on) and
// rung 3.5 (the production posture: TLS 1.3, chain validation, certificate pinning, and the hybrid
// post-quantum group X25519MLKEM768). The deliberately broken rungs between them, and mTLS above
// them, are backlog - parse_rung() names them explicitly rather than quietly handing back a weaker
// posture than the operator asked for.
enum class Rung {
    Plain, // "0"
    Tls35  // "3.5"
};

bool parse_rung(const std::string& text, Rung& out, std::string& error);
const char* rung_name(Rung rung);

// Whether this binary was built against OpenSSL (MINIDRIVE_ENABLE_TLS). False means --rung 3.5 is
// unavailable at runtime no matter how it is configured.
bool tls_available();

// The hybrid group first, classical groups behind it: a peer that cannot do ML-KEM still completes
// a (classical) handshake instead of failing outright. X25519MLKEM768 itself needs OpenSSL 3.5+;
// against an older library it is dropped from the list at startup, loudly - see --tls-require-pq
// for the deployment that would rather not start than run without it.
inline constexpr const char* DEFAULT_GROUPS = "X25519MLKEM768:X25519:P-256";

struct TlsServerConfig {
    std::string cert_file;            // PEM certificate chain (leaf first)
    std::string key_file;             // PEM private key for the leaf
    std::string ca_file;              // rung 4 only: CA that client certificates must chain to
    bool require_client_cert = false; // rung 4 only
    std::string min_version = "1.3";  // "1.2" exists for the backlogged downgrade rungs
    std::string ciphers;              // empty = OpenSSL's own defaults
    std::string groups = DEFAULT_GROUPS;
    bool require_pq = false;          // fail startup rather than run without the hybrid group
};

enum class PeerVerify {
    None,   // rung 2a: accept whatever certificate shows up. The ladder's counter-example.
    Ca,     // validate the chain to a trusted CA, and check the hostname/IP against the SANs
    Pinned  // Ca plus a leaf SPKI pin - or, with no CA configured, the pin alone
};

struct TlsClientConfig {
    PeerVerify verify = PeerVerify::Ca;
    std::string ca_file;             // empty with verify=Ca means the system trust store
    std::string pin;                 // hex SHA-256 of the server's SubjectPublicKeyInfo
    std::string server_name;         // name/IP to verify and to send as SNI; set from the endpoint
    std::string min_version = "1.3";
    std::string ciphers;
    std::string groups = DEFAULT_GROUPS;
    bool require_pq = false;
};

// Builds the IStream for one connection. Created once at startup, so the rung is decided in exactly
// one place and every call site is posture-agnostic.
class StreamFactory {
public:
    virtual ~StreamFactory() = default;

    virtual std::shared_ptr<IStream> create(asio::ip::tcp::socket socket) = 0;
    virtual bool is_tls() const = 0;
    virtual std::string describe() const = 0; // one-line posture summary for the startup banner
};

std::shared_ptr<StreamFactory> make_plain_factory();

// Both return nullptr and fill `error` on any misconfiguration - missing certificate, unreadable
// key, unusable group list, PQ required but unavailable - so main() fails loudly at startup instead
// of at the first handshake. `warnings` collects non-fatal notes worth logging (e.g. the hybrid
// group being dropped on an older OpenSSL, or a pin trusted without a chain behind it).
std::shared_ptr<StreamFactory> make_tls_server_factory(const TlsServerConfig& config,
                                                       std::string& error,
                                                       std::vector<std::string>& warnings);
std::shared_ptr<StreamFactory> make_tls_client_factory(const TlsClientConfig& config,
                                                       std::string& error,
                                                       std::vector<std::string>& warnings);

// Hex SHA-256 of a PEM certificate's SubjectPublicKeyInfo: the value --pin compares against, and
// what the server prints at startup so a client can be configured from it. Identical to
//   openssl x509 -pubkey -noout -in cert.pem | openssl pkey -pubin -outform der | openssl dgst -sha256
bool certificate_pin(const std::string& cert_file, std::string& pin_hex, std::string& error);

// Accepts "sha256:<hex>" or a bare hex digest, case-insensitively; normalizes to lowercase hex.
bool normalize_pin(const std::string& text, std::string& pin_hex, std::string& error);

// Whether an error came from the TLS layer rather than from the network underneath it - the
// difference between "the certificate was rejected" and "nothing was listening". Always false in a
// build without OpenSSL, where no such error can occur.
bool is_tls_error(const std::error_code& ec);

#ifdef MINIDRIVE_ENABLE_TLS
// Applies SNI, hostname/IP verification and certificate pinning to one SSL object, immediately
// before its client handshake. Lives here rather than in TlsStream because the stream knows how to
// run a handshake, not what makes one trustworthy.
void apply_client_handshake_options(asio::ssl::stream<asio::ip::tcp::socket>& stream,
                                    const ClientHandshakeOptions& opts);
#endif

} // namespace transport
