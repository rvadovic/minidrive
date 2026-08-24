#include "transport/tls.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

#include <asio/ip/address.hpp>

#ifdef MINIDRIVE_ENABLE_TLS
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif

namespace transport {

namespace {

std::vector<std::string> split(const std::string& text, char sep) {
    std::vector<std::string> parts;
    std::string item;
    std::istringstream iss(text);
    while(std::getline(iss, item, sep)) {
        if(!item.empty()) parts.push_back(item);
    }
    return parts;
}

std::string join(const std::vector<std::string>& parts, char sep) {
    std::string out;
    for(const auto& part : parts) {
        if(!out.empty()) out += sep;
        out += part;
    }
    return out;
}

std::string to_lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return text;
}

// The hybrid groups are the ones that need OpenSSL 3.5+, so losing one of them is worth reporting
// differently from losing, say, a brainpool curve nobody asked about.
bool is_post_quantum_group(const std::string& name) {
    const std::string lowered = to_lower(name);
    return lowered.find("mlkem") != std::string::npos || lowered.find("kyber") != std::string::npos;
}

class PlainFactory : public StreamFactory {
public:
    std::shared_ptr<IStream> create(asio::ip::tcp::socket socket) override {
        return std::make_shared<PlainStream>(std::move(socket));
    }
    bool is_tls() const override { return false; }
    std::string describe() const override {
        return "rung 0: plaintext TCP - no transport security, credentials travel in the clear";
    }
};

} // namespace

std::shared_ptr<StreamFactory> make_plain_factory() {
    return std::make_shared<PlainFactory>();
}

bool parse_rung(const std::string& text, Rung& out, std::string& error) {
    if(text == "0") {
        out = Rung::Plain;
        return true;
    }
    if(text == "3.5") {
        out = Rung::Tls35;
        return true;
    }
    // Naming the backlogged rungs explicitly matters: silently treating "2a" as "whatever is
    // closest" would hand an operator a posture they did not choose, in either direction.
    if(text == "1" || text == "2a" || text == "2b" || text == "2c" || text == "2d" ||
       text == "3" || text == "4" || text == "5") {
        error = "rung " + text + " is not implemented (backlog). Built rungs: 0 (plaintext TCP) "
                "and 3.5 (TLS 1.3, chain validation, pinning, hybrid post-quantum key exchange).";
        return false;
    }
    error = "unknown rung '" + text + "'. Valid values: 0, 3.5";
    return false;
}

const char* rung_name(Rung rung) {
    switch(rung) {
        case Rung::Plain: return "0";
        case Rung::Tls35: return "3.5";
    }
    return "unknown";
}

bool normalize_pin(const std::string& text, std::string& pin_hex, std::string& error) {
    std::string value = text;
    const std::string prefix = "sha256:";
    if(to_lower(value).rfind(prefix, 0) == 0) {
        value = value.substr(prefix.size());
    }
    value = to_lower(value);

    if(value.size() != 64) {
        error = "a pin must be a SHA-256 digest: 64 hex characters, optionally prefixed 'sha256:'";
        return false;
    }
    for(char c : value) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if(!hex) {
            error = "a pin must be a SHA-256 digest: 64 hex characters, optionally prefixed 'sha256:'";
            return false;
        }
    }
    pin_hex = value;
    return true;
}

#ifndef MINIDRIVE_ENABLE_TLS

bool tls_available() { return false; }

bool is_tls_error(const std::error_code&) { return false; }

std::shared_ptr<StreamFactory> make_tls_server_factory(const TlsServerConfig&, std::string& error,
                                                       std::vector<std::string>&) {
    error = "this build has no TLS support (built without OpenSSL); only --rung 0 is available";
    return nullptr;
}

std::shared_ptr<StreamFactory> make_tls_client_factory(const TlsClientConfig&, std::string& error,
                                                       std::vector<std::string>&) {
    error = "this build has no TLS support (built without OpenSSL); only --rung 0 is available";
    return nullptr;
}

bool certificate_pin(const std::string&, std::string&, std::string& error) {
    error = "this build has no TLS support (built without OpenSSL)";
    return false;
}

#else // MINIDRIVE_ENABLE_TLS

bool tls_available() { return true; }

bool is_tls_error(const std::error_code& ec) {
    return ec.category() == asio::error::get_ssl_category();
}

namespace {

// Drains OpenSSL's error queue into one readable line. Without this, a failed context setup reports
// only "operation failed" and the actual reason (wrong passphrase, key/cert mismatch, bad PEM) is
// left sitting in the queue.
std::string openssl_errors() {
    std::string out;
    unsigned long code = 0;
    while((code = ERR_get_error()) != 0) {
        char buf[256];
        ERR_error_string_n(code, buf, sizeof(buf));
        if(!out.empty()) out += "; ";
        out += buf;
    }
    return out.empty() ? std::string("no further detail from OpenSSL") : out;
}

// SHA-256 over the DER-encoded SubjectPublicKeyInfo - the public key rather than the whole
// certificate, so re-issuing the same key (renewal) does not invalidate a deployed pin.
bool spki_pin(X509* cert, std::string& pin_hex) {
    if(cert == nullptr) return false;

    unsigned char* der = nullptr;
    const int len = i2d_X509_PUBKEY(X509_get_X509_PUBKEY(cert), &der);
    if(len <= 0 || der == nullptr) {
        if(der) OPENSSL_free(der);
        return false;
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    const int ok = EVP_Digest(der, static_cast<size_t>(len), digest, &digest_len, EVP_sha256(), nullptr);
    OPENSSL_free(der);
    if(ok != 1) return false;

    static const char* hex = "0123456789abcdef";
    pin_hex.clear();
    pin_hex.reserve(digest_len * 2);
    for(unsigned int i = 0; i < digest_len; ++i) {
        pin_hex.push_back(hex[digest[i] >> 4]);
        pin_hex.push_back(hex[digest[i] & 0x0f]);
    }
    return true;
}

// A connection closed without close_notify then reports the same terminal condition it does at
// rung 0 - a plain end of file - instead of a TLS-layer error. Both state machines were written
// against that, and truncation is already detectable by the protocol itself: every message is
// length-prefixed and every transfer is chunk-hashed and whole-file-hashed, so a cut-short
// connection fails the transfer rather than passing as a complete one.
void ignore_unexpected_eof(SSL_CTX* ctx) {
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF // OpenSSL 3.0+; older versions behave this way already
    SSL_CTX_set_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
#else
    (void)ctx;
#endif
}

bool min_version_value(const std::string& text, int& version, std::string& error) {
    if(text == "1.3") { version = TLS1_3_VERSION; return true; }
    if(text == "1.2") { version = TLS1_2_VERSION; return true; }
    error = "unsupported TLS minimum version '" + text + "'. Valid values: 1.2, 1.3";
    return false;
}

// Applies a group preference list, keeping only the groups this OpenSSL actually knows.
//
// set1_groups_list rejects the whole list if any single entry is unrecognized, so a build against
// OpenSSL < 3.5 would fail outright on a list containing X25519MLKEM768 - taking the classical
// groups down with it. Probing each name individually turns that into a reported degradation
// instead: the connection still happens, over a classical key exchange, and the operator is told.
// --tls-require-pq is for the deployment that would rather not start than lose the hybrid group.
bool apply_groups(SSL_CTX* ctx, const std::string& list, bool require_pq,
                  std::string& applied, std::vector<std::string>& warnings, std::string& error) {
    const std::vector<std::string> requested = split(list, ':');
    if(requested.empty()) {
        error = "empty TLS group list";
        return false;
    }

    std::vector<std::string> usable;
    std::vector<std::string> unusable;
    for(const auto& group : requested) {
        if(SSL_CTX_set1_groups_list(ctx, group.c_str()) == 1) {
            usable.push_back(group);
        } else {
            ERR_clear_error(); // a rejected probe is expected, not a failure worth reporting later
            unusable.push_back(group);
        }
    }

    for(const auto& group : unusable) {
        if(is_post_quantum_group(group)) {
            // The *runtime* version, not OPENSSL_VERSION_TEXT: libssl is linked dynamically, so
            // what decides whether this group exists is the library loaded now, not the headers
            // this was compiled against. Naming the wrong one would send someone to fix the wrong
            // machine.
            const std::string message = "post-quantum group " + group + " is unavailable in the "
                "OpenSSL in use (" + OpenSSL_version(OPENSSL_VERSION) + "); it needs OpenSSL 3.5 "
                "or newer. Key exchange falls back to the classical groups.";
            if(require_pq) {
                error = message + " --tls-require-pq was given, so startup fails instead.";
                return false;
            }
            warnings.push_back(message);
        } else {
            warnings.push_back("TLS group " + group + " is not supported by this OpenSSL build and was dropped");
        }
    }

    if(usable.empty()) {
        error = "none of the requested TLS groups (" + list + ") are supported by this OpenSSL build";
        return false;
    }

    applied = join(usable, ':');
    if(SSL_CTX_set1_groups_list(ctx, applied.c_str()) != 1) {
        error = "failed to apply TLS group list '" + applied + "': " + openssl_errors();
        return false;
    }
    return true;
}

// TLS 1.3 suites are configured through a different call than the pre-1.3 cipher list, and the two
// namespaces do not overlap: 1.3 suite names all start with "TLS_".
bool apply_ciphers(SSL_CTX* ctx, const std::string& ciphers, std::string& error) {
    if(ciphers.empty()) return true;

    const bool tls13_suites = ciphers.find("TLS_") != std::string::npos;
    const int ok = tls13_suites ? SSL_CTX_set_ciphersuites(ctx, ciphers.c_str())
                                : SSL_CTX_set_cipher_list(ctx, ciphers.c_str());
    if(ok != 1) {
        error = "failed to apply cipher list '" + ciphers + "': " + openssl_errors();
        return false;
    }
    return true;
}

class TlsFactory : public StreamFactory {
public:
    TlsFactory(std::shared_ptr<asio::ssl::context> ctx, ClientHandshakeOptions opts, std::string description)
        : ctx_(std::move(ctx)), opts_(std::move(opts)), description_(std::move(description)) {}

    std::shared_ptr<IStream> create(asio::ip::tcp::socket socket) override {
        return std::make_shared<TlsStream>(std::move(socket), ctx_, opts_);
    }
    bool is_tls() const override { return true; }
    std::string describe() const override { return description_; }

private:
    std::shared_ptr<asio::ssl::context> ctx_;
    ClientHandshakeOptions opts_;
    std::string description_;
};

} // namespace

void apply_client_handshake_options(asio::ssl::stream<asio::ip::tcp::socket>& stream,
                                    const ClientHandshakeOptions& opts) {
    SSL* ssl = stream.native_handle();
    if(ssl == nullptr) return;

    // SNI carries a name, never an address literal (RFC 6066), so connecting straight to an IP
    // sends none - which is correct, not a gap: the identity check below still runs on the SANs.
    if(!opts.sni_host.empty()) {
        SSL_set_tlsext_host_name(ssl, opts.sni_host.c_str());
    }

    // Hostname/IP verification is done by OpenSSL as part of chain verification rather than in the
    // callback below: X509_check_host/_ip handle wildcards, SAN types and embedded NULs correctly,
    // and getting those wrong by hand is exactly how "validated" certificates end up unvalidated.
    if(X509_VERIFY_PARAM* param = SSL_get0_param(ssl)) {
        if(!opts.verify_host.empty()) {
            X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
            X509_VERIFY_PARAM_set1_host(param, opts.verify_host.c_str(), 0);
        } else if(!opts.verify_ip.empty()) {
            X509_VERIFY_PARAM_set1_ip_asc(param, opts.verify_ip.c_str());
        }
    }

    if(opts.pin_sha256_hex.empty()) return;

    const std::string expected = opts.pin_sha256_hex;
    const bool pin_only = opts.pin_only;

    stream.set_verify_callback([expected, pin_only](bool preverified, asio::ssl::verify_context& ctx) {
        X509_STORE_CTX* store = ctx.native_handle();
        X509* cert = X509_STORE_CTX_get_current_cert(store);
        const int depth = X509_STORE_CTX_get_error_depth(store);

        // OpenSSL walks the chain from the root down, so returning false above depth 0 stops the
        // walk before the leaf is ever seen. In pin-only mode - where the pin *is* the identity and
        // the chain is deliberately untrusted - everything above the leaf is waved through so the
        // leaf actually gets examined.
        if(depth != 0) {
            return pin_only ? true : preverified;
        }

        if(!pin_only && !preverified) return false;
        if(cert == nullptr) return false;

        std::string actual;
        if(!spki_pin(cert, actual)) return false;
        return actual == expected;
    });
}

bool certificate_pin(const std::string& cert_file, std::string& pin_hex, std::string& error) {
    BIO* bio = BIO_new_file(cert_file.c_str(), "r");
    if(bio == nullptr) {
        error = "cannot read certificate " + cert_file + ": " + openssl_errors();
        return false;
    }

    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if(cert == nullptr) {
        error = "cannot parse certificate " + cert_file + ": " + openssl_errors();
        return false;
    }

    const bool ok = spki_pin(cert, pin_hex);
    X509_free(cert);
    if(!ok) {
        error = "cannot hash the public key of " + cert_file + ": " + openssl_errors();
        return false;
    }
    return true;
}

std::shared_ptr<StreamFactory> make_tls_server_factory(const TlsServerConfig& config,
                                                       std::string& error,
                                                       std::vector<std::string>& warnings) {
    if(config.cert_file.empty() || config.key_file.empty()) {
        error = "TLS needs both --tls-cert and --tls-key (see lab/gen-certs.sh)";
        return nullptr;
    }

    int min_version = 0;
    if(!min_version_value(config.min_version, min_version, error)) return nullptr;

    auto ctx = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_server);
    ctx->set_options(asio::ssl::context::default_workarounds |
                     asio::ssl::context::no_sslv2 |
                     asio::ssl::context::no_sslv3 |
                     asio::ssl::context::no_tlsv1 |
                     asio::ssl::context::no_tlsv1_1 |
                     asio::ssl::context::single_dh_use);

    SSL_CTX* raw = ctx->native_handle();
    ignore_unexpected_eof(raw);
    if(SSL_CTX_set_min_proto_version(raw, min_version) != 1) {
        error = "cannot require TLS " + config.min_version + ": " + openssl_errors();
        return nullptr;
    }

    std::error_code ec;
    ctx->use_certificate_chain_file(config.cert_file, ec);
    if(ec) {
        error = "cannot load certificate chain " + config.cert_file + ": " + openssl_errors();
        return nullptr;
    }
    ctx->use_private_key_file(config.key_file, asio::ssl::context::pem, ec);
    if(ec) {
        error = "cannot load private key " + config.key_file + ": " + openssl_errors();
        return nullptr;
    }
    if(SSL_CTX_check_private_key(raw) != 1) {
        error = "private key " + config.key_file + " does not match certificate " + config.cert_file;
        return nullptr;
    }

    // Rung 3.5 authenticates the server to the client and nothing in the other direction; the
    // password still establishes who the user is. Client certificates are rung 4 (backlog).
    ctx->set_verify_mode(asio::ssl::verify_none);

    if(!apply_ciphers(raw, config.ciphers, error)) return nullptr;

    std::string groups;
    if(!apply_groups(raw, config.groups, config.require_pq, groups, warnings, error)) return nullptr;

    std::string description = "rung 3.5: TLS " + config.min_version + "+ (server-authenticated), groups " + groups;
    return std::make_shared<TlsFactory>(std::move(ctx), ClientHandshakeOptions{}, std::move(description));
}

std::shared_ptr<StreamFactory> make_tls_client_factory(const TlsClientConfig& config,
                                                       std::string& error,
                                                       std::vector<std::string>& warnings) {
    int min_version = 0;
    if(!min_version_value(config.min_version, min_version, error)) return nullptr;

    std::string pin_hex;
    if(!config.pin.empty() && !normalize_pin(config.pin, pin_hex, error)) return nullptr;
    if(config.verify == PeerVerify::Pinned && pin_hex.empty()) {
        error = "--tls-verify pinned needs --pin <sha256:...> (the server prints its own pin at startup)";
        return nullptr;
    }
    // Contradictory: with verification off, nothing checks the certificate, so the pin would be
    // accepted and then quietly ignored - the worst of both, since it reads as protection.
    if(config.verify == PeerVerify::None && !pin_hex.empty()) {
        error = "--pin cannot be combined with --tls-verify none: with verification off the pin "
                "would not be checked at all. Drop one of the two.";
        return nullptr;
    }

    auto ctx = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_client);
    ctx->set_options(asio::ssl::context::default_workarounds |
                     asio::ssl::context::no_sslv2 |
                     asio::ssl::context::no_sslv3 |
                     asio::ssl::context::no_tlsv1 |
                     asio::ssl::context::no_tlsv1_1);

    SSL_CTX* raw = ctx->native_handle();
    ignore_unexpected_eof(raw);
    if(SSL_CTX_set_min_proto_version(raw, min_version) != 1) {
        error = "cannot require TLS " + config.min_version + ": " + openssl_errors();
        return nullptr;
    }

    if(!apply_ciphers(raw, config.ciphers, error)) return nullptr;

    std::string groups;
    if(!apply_groups(raw, config.groups, config.require_pq, groups, warnings, error)) return nullptr;

    ClientHandshakeOptions opts;
    opts.pin_sha256_hex = pin_hex;

    // A pin with no CA behind it is a deliberate posture, not an oversight: it is how a
    // self-signed or private deployment is trusted. It is not chain validation, so say so.
    const bool have_ca = !config.ca_file.empty();
    opts.pin_only = (config.verify == PeerVerify::Pinned) && !have_ca;

    std::string verify_description;
    switch(config.verify) {
        case PeerVerify::None:
            // Rung 2a's posture, and the reason the ladder has that rung: anything can impersonate
            // the server. Never a default, and never silent.
            ctx->set_verify_mode(asio::ssl::verify_none);
            warnings.push_back("--tls-verify none: the server's certificate is NOT checked. The "
                               "connection is encrypted but trivially machine-in-the-middled.");
            verify_description = "verify=none";
            break;

        case PeerVerify::Ca:
        case PeerVerify::Pinned: {
            ctx->set_verify_mode(asio::ssl::verify_peer);
            if(have_ca) {
                std::error_code ec;
                ctx->load_verify_file(config.ca_file, ec);
                if(ec) {
                    error = "cannot load CA file " + config.ca_file + ": " + openssl_errors();
                    return nullptr;
                }
                // The identity to check the certificate against: a name goes through SNI and the
                // DNS SANs, an address literal through the IP SANs.
                std::error_code addr_ec;
                asio::ip::make_address(config.server_name, addr_ec);
                if(addr_ec) {
                    opts.sni_host = config.server_name;
                    opts.verify_host = config.server_name;
                } else {
                    opts.verify_ip = config.server_name;
                }
                verify_description = opts.pin_sha256_hex.empty() ? "verify=ca" : "verify=ca+pinned";
            } else if(opts.pin_only) {
                warnings.push_back("--pin without --ca-file: the certificate chain is not validated, "
                                   "the pinned public key alone identifies the server.");
                verify_description = "verify=pin-only";
            } else {
                ctx->set_default_verify_paths();
                std::error_code addr_ec;
                asio::ip::make_address(config.server_name, addr_ec);
                if(addr_ec) {
                    opts.sni_host = config.server_name;
                    opts.verify_host = config.server_name;
                } else {
                    opts.verify_ip = config.server_name;
                }
                verify_description = "verify=ca(system)";
            }
            break;
        }
    }

    std::string description = "rung 3.5: TLS " + config.min_version + "+, " + verify_description +
                              ", groups " + groups;
    return std::make_shared<TlsFactory>(std::move(ctx), std::move(opts), std::move(description));
}

#endif // MINIDRIVE_ENABLE_TLS

} // namespace transport
