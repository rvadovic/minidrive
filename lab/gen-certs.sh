#!/usr/bin/env bash
#
# Generates the certificates MiniDrive needs to run at rung 3.5.
#
# This is deployment tooling, not lab material: a server certificate and the CA that signed it are
# required to run the server with --rung 3.5 at all, and the client's --ca-file/--pin have to point
# at something. Only the *rogue* CA and rogue server certificate at the end are lab material - they
# exist so the "MITM with a certificate from the wrong CA" exercise has something to MITM with, and
# they cost four extra lines in a script that has to exist anyway.
#
# Nothing here is a substitute for a real CA in a real deployment. These are private, self-issued
# certificates for a private service: the client is expected to trust exactly this CA (--ca-file),
# or exactly this public key (--pin), not a public trust store.
#
# Usage:
#   lab/gen-certs.sh [output_dir]              # default: lab/certs
#   SERVER_SANS="DNS:nas.example.com,IP:10.0.0.5" lab/gen-certs.sh
#
# Produces, in the output directory:
#   ca.crt / ca.key                     the root CA; ca.crt is what a client passes to --ca-file
#   server.crt / server.key             the server's certificate; --tls-cert / --tls-key
#   server.pin                          SHA-256 of the server's public key; --pin
#   rogue-ca.crt / rogue-ca.key         a second CA the real client must NOT trust
#   rogue-server.crt / rogue-server.key a certificate for the same names, signed by the rogue CA
#   rogue-server.pin                    so a pin-mismatch can be demonstrated deliberately
#
# (Client certificates belong to rung 4 - mutual TLS - which is not built yet; they would be
# generated here too, signed by the same CA, when it is.)

set -euo pipefail

OUT_DIR="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/certs}"

# The names and addresses a client may use to reach this server. Every one of them has to be in the
# certificate: at rung 3.5 the client checks the name it dialled against these SANs, so a name that
# is missing here is a connection that will be refused, not a warning.
SERVER_SANS="${SERVER_SANS:-DNS:localhost,DNS:minidrive.local,IP:127.0.0.1,IP:::1}"

CA_DAYS="${CA_DAYS:-3650}"
LEAF_DAYS="${LEAF_DAYS:-825}"

mkdir -p "$OUT_DIR"
cd "$OUT_DIR"

# ECDSA P-256: small, fast, and universally supported. The signature algorithm is independent of the
# key-exchange group, so this is unrelated to (and unaffected by) the hybrid post-quantum group the
# handshake negotiates - see --tls-groups.
new_key() {
    openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$1" 2>/dev/null
    chmod 600 "$1"
}

make_ca() {
    local key="$1" crt="$2" cn="$3"
    new_key "$key"
    openssl req -new -x509 -key "$key" -out "$crt" -days "$CA_DAYS" -subj "/CN=$cn" \
        -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
        -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
}

make_server_cert() {
    local key="$1" crt="$2" cn="$3" ca_key="$4" ca_crt="$5"
    new_key "$key"

    local csr ext
    csr="$(mktemp)"
    ext="$(mktemp)"
    cat > "$ext" <<EXT
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=$SERVER_SANS
EXT

    openssl req -new -key "$key" -out "$csr" -subj "/CN=$cn" 2>/dev/null
    openssl x509 -req -in "$csr" -CA "$ca_crt" -CAkey "$ca_key" -CAcreateserial \
        -out "$crt" -days "$LEAF_DAYS" -sha256 -extfile "$ext" 2>/dev/null
    rm -f "$csr" "$ext"
}

# SHA-256 of the DER-encoded SubjectPublicKeyInfo. The *public key*, not the whole certificate, so
# renewing the certificate with the same key leaves every deployed pin valid. This is exactly what
# the server computes and prints at startup, and what --pin compares against.
public_key_pin() {
    openssl x509 -in "$1" -pubkey -noout \
        | openssl pkey -pubin -outform der 2>/dev/null \
        | openssl dgst -sha256 \
        | awk '{print $NF}'
}

echo "Generating certificates in $OUT_DIR"
echo "  server SANs: $SERVER_SANS"

make_ca ca.key ca.crt "MiniDrive Root CA"
make_server_cert server.key server.crt "MiniDrive Server" ca.key ca.crt

# The rogue pair: a technically valid certificate for the same names, signed by a CA nobody trusts.
# This is what a machine-in-the-middle can produce on its own, and the reason --ca-file/--pin exist.
make_ca rogue-ca.key rogue-ca.crt "Rogue CA"
make_server_cert rogue-server.key rogue-server.crt "MiniDrive Server" rogue-ca.key rogue-ca.crt

SERVER_PIN="$(public_key_pin server.crt)"
ROGUE_PIN="$(public_key_pin rogue-server.crt)"
printf 'sha256:%s\n' "$SERVER_PIN" > server.pin
printf 'sha256:%s\n' "$ROGUE_PIN" > rogue-server.pin

# Verifying here rather than at the first handshake: a chain that does not build is a mistake in
# this script, and finding that out from a failed connection is a bad trade.
openssl verify -CAfile ca.crt server.crt > /dev/null
openssl verify -CAfile rogue-ca.crt rogue-server.crt > /dev/null

cat <<SUMMARY

Done.

  Server:
    server --port 9000 --root <root> --rung 3.5 \\
           --tls-cert $OUT_DIR/server.crt --tls-key $OUT_DIR/server.key

  Client, trusting the CA:
    client user@localhost:9000 --rung 3.5 --ca-file $OUT_DIR/ca.crt

  Client, pinned to this exact public key (strongest; survives certificate renewal):
    client user@localhost:9000 --rung 3.5 --ca-file $OUT_DIR/ca.crt \\
           --pin sha256:$SERVER_PIN

  Server public key pin: sha256:$SERVER_PIN   (also in $OUT_DIR/server.pin)
  Rogue  public key pin: sha256:$ROGUE_PIN   (lab use only - the impostor)

  Keep *.key private: they are the server's identity. Only ca.crt (and the pin) are meant to be
  distributed to clients.
SUMMARY
