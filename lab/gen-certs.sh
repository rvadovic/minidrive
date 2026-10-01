#!/usr/bin/env bash
#
# Creates and renews the certificates MiniDrive needs to run at rung 3.5.
#
# This is deployment tooling. A server certificate and the CA that signed it are required to run
# the server with --rung 3.5 at all, and the client's --ca-file / --pin need something to point at.
# The adversary material (--lab) is the only lab part, and it is written to a separate directory.
#
# These are private certificates for a private service. Clients are expected to trust exactly this
# CA (--ca-file) or exactly this public key (--pin), never a public trust store.
#
# Usage:
#   SERVER_SANS="DNS:nas.example.com,IP:192.168.1.11" lab/gen-certs.sh init [options] [dir]
#   lab/gen-certs.sh renew [options] [dir]
#
#   dir                     output directory (default: lab/certs next to this script)
#
#   init                    create the CA and the first server certificate. Refuses to run if the
#                           directory already holds a PKI, because that would destroy it: every
#                           client's ca.crt and pin would stop matching.
#   renew                   issue a new server certificate with the EXISTING CA key and server key.
#                           ca.crt and the pin stay the same, so no client needs updating.
#
#   --force                 init only: replace an existing PKI. The old one is moved aside to
#                           <dir>/replaced-<timestamp>/, not deleted.
#   --lab                   init only: also write adversary material to <dir>/adversary/, and default
#                           SERVER_SANS to localhost names. Never use the output in production.
#   --pass-file <file>      read the CA key passphrase from the first line of <file>
#   --if-expiring <days>    renew only: do nothing unless the certificate expires within <days>
#                           (for running renew from a timer)
#
# Environment:
#   SERVER_SANS             names/addresses clients use to reach the server, e.g.
#                           "DNS:nas.example.com,IP:192.168.1.11,IP:10.10.10.1". Required by init
#                           unless --lab. On renew, defaults to the SANs recorded at init.
#   CA_PERMITTED            the only names the CA may ever sign for (X.509 name constraints), e.g.
#                           "DNS:minidrive.local,IP:192.168.1.0/24,IP:10.10.10.0/24". Defaults to
#                           exactly the SERVER_SANS. Fixed for the CA's lifetime: a later SAN outside
#                           it needs a new CA (init --force).
#   CA_KEY_ALG              EC (default, P-256), ML-DSA-65 or ML-DSA-87. See "Post-quantum" below.
#   CA_DAYS                 CA lifetime, default 3650
#   LEAF_DAYS               server certificate lifetime, default 90. Short on purpose; see renew.
#   MINIDRIVE_CA_PASSPHRASE CA key passphrase, if --pass-file is not given. Otherwise it is asked on
#                           the terminal.
#
# Output layout. Each directory is exactly what one party gets:
#   ca/       ca.key (passphrase-encrypted), ca.crt, server.sans. STAYS ON THIS MACHINE. Anyone holding
#             an unlocked ca.key can mint a certificate every --ca-file client accepts.
#   server/   server.crt, server.key, server.pin. Copy this directory, and only this one, to the server.
#   client/   ca.crt, server.pin. Give these to clients (--ca-file, --pin).
#   adversary/ (--lab only) rogue-ca.{crt,key}, rogue-server.{crt,key,pin}: an impostor CA with the
#             real CA's exact name, and a certificate for the real server's names signed by it.
#
# Revocation: there is none. No client checks a CRL, so the CA does not claim cRLSign. A stolen
# server.key is limited by the certificate's short lifetime instead: renew regularly (e.g. from a
# timer with --if-expiring 30), and a leaked key stops being accepted by --ca-file clients within
# LEAF_DAYS. Pinned clients keep trusting the key until they are given a new pin, so a stolen key
# means a new server key (init --force) and new pins on every client.
#
# Post-quantum: TLS key exchange is already hybrid (X25519MLKEM768), so recorded traffic is safe.
# Signatures are not exposed to harvest-now-decrypt-later: a future quantum computer cannot forge a
# handshake that already happened. The long-lived CA is the one artifact whose lifetime could
# overlap such a computer, so CA_KEY_ALG=ML-DSA-65 signs with ML-DSA. That needs OpenSSL 3.5+ here
# and on every client that verifies the chain (the static release clients embed 3.5; a client built
# against a distribution's OpenSSL 3.0 will refuse the chain). The server key stays P-256.

set -euo pipefail
umask 077

PROG="$(basename "$0")"
DEFAULT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/certs"
LAB_SANS="DNS:localhost,DNS:minidrive.local,IP:127.0.0.1,IP:::1"
CA_CN="MiniDrive Root CA"
SERVER_CN="MiniDrive Server"

die() { printf '%s: error: %s\n' "$PROG" "$*" >&2; exit 1; }
warn() { printf '%s: warning: %s\n' "$PROG" "$*" >&2; }

usage() {
    sed -n '/^# Usage:/,/^# Output layout/p' "$0" | sed '$d; s/^# \{0,1\}//' >&2
    exit 2
}

# Runs a command quietly. On failure its whole output is shown, so nothing fails silently.
run() {
    local out
    if ! out="$("$@" 2>&1)"; then
        printf '%s\n' "$out" >&2
        die "'$1 ${2:-}' failed (output above)"
    fi
}

# ---------------------------------------------------------------------------------------------
# Arguments
# ---------------------------------------------------------------------------------------------

[[ $# -ge 1 ]] || usage
MODE="$1"; shift
case "$MODE" in
    init|renew) ;;
    -h|--help|help) usage ;;
    *) die "unknown command '$MODE' (expected init or renew; see --help)" ;;
esac

FORCE=0 LAB=0 PASS_FILE="" IF_EXPIRING="" OUT_DIR=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --force) FORCE=1 ;;
        --lab) LAB=1 ;;
        --pass-file) [[ $# -ge 2 ]] || die "--pass-file needs a file"; PASS_FILE="$2"; shift ;;
        --if-expiring) [[ $# -ge 2 ]] || die "--if-expiring needs a number of days"; IF_EXPIRING="$2"; shift ;;
        -h|--help) usage ;;
        -*) die "unknown option '$1'" ;;
        *) [[ -z $OUT_DIR ]] || die "more than one output directory given"; OUT_DIR="$1" ;;
    esac
    shift
done
OUT_DIR="${OUT_DIR:-$DEFAULT_DIR}"

if [[ $MODE == renew ]]; then
    (( FORCE == 0 && LAB == 0 )) || die "--force and --lab only apply to init"
else
    [[ -z $IF_EXPIRING ]] || die "--if-expiring only applies to renew"
fi
[[ -z $IF_EXPIRING || $IF_EXPIRING =~ ^[0-9]{1,5}$ ]] || die "--if-expiring expects whole days"

CA_DAYS="${CA_DAYS:-3650}"
LEAF_DAYS="${LEAF_DAYS:-90}"
CA_KEY_ALG="${CA_KEY_ALG:-EC}"
[[ $CA_DAYS =~ ^[1-9][0-9]{0,4}$ ]] || die "CA_DAYS must be a positive number of days"
[[ $LEAF_DAYS =~ ^[1-9][0-9]{0,4}$ ]] || die "LEAF_DAYS must be a positive number of days"
(( LEAF_DAYS <= 397 )) || warn "LEAF_DAYS=$LEAF_DAYS: a stolen server key stays valid that long for --ca-file clients"

command -v openssl >/dev/null || die "openssl is not installed"

# ---------------------------------------------------------------------------------------------
# Input validation. Values end up in an OpenSSL config file, where a newline would add arbitrary
# extensions (e.g. CA:TRUE on the server certificate) and '$' is expanded, so every entry has to
# match a strict pattern before anything is written.
# ---------------------------------------------------------------------------------------------

DNS_RE='^([A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)(\.[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*$'
IPV4_RE='^(([0-9]{1,3})\.){3}[0-9]{1,3}$'
IPV6_RE='^[0-9A-Fa-f:]*:[0-9A-Fa-f:]*:[0-9A-Fa-f:]*$'

valid_ipv4() {
    [[ $1 =~ $IPV4_RE ]] || return 1
    local IFS=. o
    for o in $1; do (( 10#$o <= 255 )) || return 1; done
}
valid_ipv6() { [[ $1 =~ $IPV6_RE && ${#1} -le 39 ]]; }
valid_dns() { [[ $1 =~ $DNS_RE && ${#1} -le 253 ]]; }

# Splits a comma-separated list into the array named by $2, rejecting empty items.
split_list() {
    local list="$1" item
    local -n into="$2"
    into=()
    [[ -n $list ]] || return 1
    # Checked before splitting: `read` stops at the first newline, which would silently drop the
    # rest of the value instead of rejecting it.
    [[ $list != *[[:cntrl:][:space:]]* ]] || return 1
    IFS=',' read -r -a into <<< "$list"
    for item in "${into[@]}"; do [[ -n $item ]] || return 1; done
    (( ${#into[@]} > 0 ))
}

# Parses SERVER_SANS into SAN_DNS / SAN_IP.
parse_sans() {
    local -a items
    SAN_DNS=() SAN_IP=()
    split_list "$1" items || die "SERVER_SANS is empty, has an empty entry, or contains whitespace: '$1'"
    local item v
    for item in "${items[@]}"; do
        case "$item" in
            DNS:*) v="${item#DNS:}"; valid_dns "$v" || die "invalid DNS name in SERVER_SANS: '$v'"; SAN_DNS+=("$v") ;;
            IP:*)  v="${item#IP:}"; valid_ipv4 "$v" || valid_ipv6 "$v" || die "invalid IP address in SERVER_SANS: '$v'"; SAN_IP+=("$v") ;;
            *) die "SERVER_SANS entries must be DNS:<name> or IP:<address>, got: '$item'" ;;
        esac
    done
}

ipv4_mask() {   # prefix length -> dotted mask
    local p=$1 i out=() bits
    for i in 0 1 2 3; do
        bits=$(( p - 8 * i )); (( bits < 0 )) && bits=0; (( bits > 8 )) && bits=8
        out+=( $(( (0xff << (8 - bits)) & 0xff )) )
    done
    local IFS=.; printf '%s' "${out[*]}"
}

ipv6_mask() {   # prefix length -> full-form mask
    local p=$1 i out=() bits
    for i in 0 1 2 3 4 5 6 7; do
        bits=$(( p - 16 * i )); (( bits < 0 )) && bits=0; (( bits > 16 )) && bits=16
        out+=( "$(printf '%x' $(( (0xffff << (16 - bits)) & 0xffff )))" )
    done
    local IFS=:; printf '%s' "${out[*]}"
}

# Parses CA_PERMITTED (or the SANs) into NC_DNS / NC_IP, the latter as OpenSSL's addr/mask form.
parse_permitted() {
    local -a items
    NC_DNS=() NC_IP=()
    split_list "$1" items || die "CA_PERMITTED is empty, has an empty entry, or contains whitespace: '$1'"
    local item v addr prefix
    for item in "${items[@]}"; do
        case "$item" in
            DNS:*)
                v="${item#DNS:}"
                valid_dns "$v" || die "invalid DNS name in CA_PERMITTED: '$v'"
                NC_DNS+=("$v") ;;
            IP:*)
                v="${item#IP:}"; addr="${v%%/*}"; prefix=""
                [[ $v == */* ]] && prefix="${v#*/}"
                [[ -z $prefix || $prefix =~ ^[0-9]{1,3}$ ]] || die "invalid prefix length in CA_PERMITTED: '$v'"
                if valid_ipv4 "$addr"; then
                    prefix="${prefix:-32}"; (( prefix <= 32 )) || die "IPv4 prefix over 32: '$v'"
                    # OpenSSL compares (name & mask) against the base as written, so a base with
                    # host bits set would silently match nothing.
                    local -a o m; local mask i; mask="$(ipv4_mask "$prefix")"
                    IFS=. read -r -a o <<< "$addr"
                    IFS=. read -r -a m <<< "$mask"
                    for i in 0 1 2 3; do
                        (( (10#${o[i]} & ~10#${m[i]} & 0xff) == 0 )) \
                            || die "CA_PERMITTED '$v' has host bits set; use the network address"
                    done
                    NC_IP+=("$addr/$mask")
                elif valid_ipv6 "$addr"; then
                    prefix="${prefix:-128}"; (( prefix <= 128 )) || die "IPv6 prefix over 128: '$v'"
                    NC_IP+=("$addr/$(ipv6_mask "$prefix")")
                else
                    die "invalid IP address in CA_PERMITTED: '$addr'"
                fi ;;
            *) die "CA_PERMITTED entries must be DNS:<name> or IP:<address>[/<prefix>], got: '$item'" ;;
        esac
    done
}

# ---------------------------------------------------------------------------------------------
# Passphrase for the CA key. Passed to OpenSSL by reference (env:/file:), never on a command line,
# where any local user could read it from the process list.
# ---------------------------------------------------------------------------------------------

PASS_ARG=""
setup_passphrase() {
    local confirm="$1"
    if [[ -n $PASS_FILE ]]; then
        [[ -r $PASS_FILE ]] || die "cannot read --pass-file '$PASS_FILE'"
        local first; IFS= read -r first < "$PASS_FILE" || true
        [[ -n $first ]] || die "--pass-file '$PASS_FILE' has an empty first line"
        PASS_ARG="file:$PASS_FILE"
    elif [[ -n ${MINIDRIVE_CA_PASSPHRASE:-} ]]; then
        export MINIDRIVE_CA_PASSPHRASE
        PASS_ARG="env:MINIDRIVE_CA_PASSPHRASE"
    elif (exec </dev/tty) 2>/dev/null; then
        local p1 p2
        IFS= read -rsp "CA key passphrase: " p1 </dev/tty; printf '\n' >/dev/tty
        [[ ${#p1} -ge 8 ]] || die "the CA passphrase must be at least 8 characters"
        if (( confirm )); then
            IFS= read -rsp "Repeat passphrase: " p2 </dev/tty; printf '\n' >/dev/tty
            [[ $p1 == "$p2" ]] || die "passphrases do not match"
        fi
        export _MINIDRIVE_CA_PASS="$p1"
        PASS_ARG="env:_MINIDRIVE_CA_PASS"
    else
        die "the CA key is encrypted: give --pass-file, set MINIDRIVE_CA_PASSPHRASE, or run on a terminal"
    fi
}

# ---------------------------------------------------------------------------------------------
# Certificate building blocks. Everything is written into $STAGE and moved into place only after
# it has all been generated and verified, so a failure leaves the output directory untouched.
# ---------------------------------------------------------------------------------------------

STAGE=""
cleanup() { [[ -n $STAGE ]] && rm -rf "$STAGE"; return 0; }
trap cleanup EXIT

digest_args() {   # ML-DSA has its own fixed hashing; passing a digest is an error
    case "$1" in EC) printf '%s\n' -sha256 ;; esac
}

check_ca_alg() {
    case "$CA_KEY_ALG" in
        EC) ;;
        ML-DSA-65|ML-DSA-87)
            openssl list -public-key-algorithms 2>/dev/null | grep -q "$CA_KEY_ALG" \
                || die "CA_KEY_ALG=$CA_KEY_ALG needs OpenSSL 3.5+ ($(openssl version) found)"
            warn "ML-DSA CA: clients verifying the chain need OpenSSL 3.5+ (pin-only clients do not)" ;;
        *) die "CA_KEY_ALG must be EC, ML-DSA-65 or ML-DSA-87, got '$CA_KEY_ALG'" ;;
    esac
}

# new_key <out> <alg> [encrypt]
new_key() {
    local -a args=(genpkey -out "$1")
    case "$2" in
        EC) args+=(-algorithm EC -pkeyopt ec_paramgen_curve:P-256) ;;
        *)  args+=(-algorithm "$2") ;;
    esac
    [[ ${3:-} == encrypt ]] && args+=(-aes-256-cbc -pass "$PASS_ARG")
    run openssl "${args[@]}"
    chmod 600 "$1"
}

# A minimal config so the system openssl.cnf cannot add anything, plus the CA profile.
write_ca_config() {
    local cfg="$1" i
    {
        printf '[req]\ndistinguished_name = dn\n[dn]\n'
        printf '[v3_ca]\n'
        printf 'basicConstraints = critical,CA:TRUE,pathlen:0\n'
        printf 'keyUsage = critical,keyCertSign\n'
        printf 'subjectKeyIdentifier = hash\n'
        printf 'nameConstraints = critical,@nc\n'
        printf '[nc]\n'
        for i in "${!NC_DNS[@]}"; do printf 'permitted;DNS.%d = %s\n' "$i" "${NC_DNS[i]}"; done
        for i in "${!NC_IP[@]}"; do printf 'permitted;IP.%d = %s\n' "$i" "${NC_IP[i]}"; done
        # A name type with no permitted subtree is unconstrained (RFC 5280), so an empty type is
        # pinned to something no real server can be: the reserved .invalid TLD, the unspecified
        # address.
        (( ${#NC_DNS[@]} )) || printf 'permitted;DNS.0 = invalid\n'
        (( ${#NC_IP[@]} )) || printf 'permitted;IP.0 = 0.0.0.0/255.255.255.255\n'
    } > "$cfg"
}

# write_leaf_config <cfg> <dns...> -- <ip...>
write_leaf_config() {
    local cfg="$1"; shift
    local -a dns=() ip=()
    while [[ $# -gt 0 && $1 != -- ]]; do dns+=("$1"); shift; done
    [[ ${1:-} == -- ]] && shift
    ip=("$@")
    local i
    {
        printf '[req]\ndistinguished_name = dn\n[dn]\n'
        printf '[v3_leaf]\n'
        printf 'basicConstraints = critical,CA:FALSE\n'
        # No keyEncipherment: EC keys do not do key transport (RFC 5480, RFC 8813).
        printf 'keyUsage = critical,digitalSignature\n'
        printf 'extendedKeyUsage = serverAuth\n'
        printf 'subjectKeyIdentifier = hash\n'
        printf 'authorityKeyIdentifier = keyid:always\n'
        printf 'subjectAltName = @alt\n[alt]\n'
        for i in "${!dns[@]}"; do printf 'DNS.%d = %s\n' "$i" "${dns[i]}"; done
        for i in "${!ip[@]}"; do printf 'IP.%d = %s\n' "$i" "${ip[i]}"; done
    } > "$cfg"
}

# make_ca <key> <crt> <alg> <encrypt|plain>
make_ca() {
    local key="$1" crt="$2" alg="$3" enc="$4" cfg="$STAGE/tmp/ca.cnf"
    local -a pass=()
    write_ca_config "$cfg"
    if [[ $enc == encrypt ]]; then new_key "$key" "$alg" encrypt; pass=(-passin "$PASS_ARG")
    else new_key "$key" "$alg"; fi
    # shellcheck disable=SC2046
    run openssl req -new -x509 -config "$cfg" -extensions v3_ca -key "$key" "${pass[@]}" \
        $(digest_args "$alg") -days "$CA_DAYS" -subj "/CN=$CA_CN" -out "$crt"
    chmod 644 "$crt"
}

# sign_leaf <leaf-key> <out-crt> <ca-key> <ca-crt> <ca-alg> <encrypted|plain> <dns...> -- <ip...>
sign_leaf() {
    local key="$1" crt="$2" ca_key="$3" ca_crt="$4" ca_alg="$5" enc="$6"; shift 6
    local cfg="$STAGE/tmp/leaf.cnf" csr="$STAGE/tmp/leaf.csr"
    local -a pass=()
    [[ $enc == encrypted ]] && pass=(-passin "$PASS_ARG")
    write_leaf_config "$cfg" "$@"
    run openssl req -new -config "$cfg" -key "$key" -subj "/CN=$SERVER_CN" -out "$csr"
    # shellcheck disable=SC2046
    run openssl x509 -req -in "$csr" -CA "$ca_crt" -CAkey "$ca_key" "${pass[@]}" \
        -set_serial "0x$(openssl rand -hex 16)" $(digest_args "$ca_alg") -days "$LEAF_DAYS" \
        -extfile "$cfg" -extensions v3_leaf -out "$crt"
    chmod 644 "$crt"
}

# SHA-256 of the DER SubjectPublicKeyInfo: what the server prints at startup and --pin compares.
# Over the public key, not the certificate, so renewing with the same key keeps every pin valid.
pin_of_cert() {
    openssl x509 -in "$1" -pubkey -noout | openssl pkey -pubin -outform der | openssl dgst -sha256 -r \
        | cut -d' ' -f1
}
pin_of_key() {
    openssl pkey -in "$1" -pubout -outform der | openssl dgst -sha256 -r | cut -d' ' -f1
}

ca_alg_of() {   # the CA key's algorithm, read back from the certificate (renew does not trust env)
    case "$(openssl x509 -in "$1" -noout -text | sed -n 's/^ *Public Key Algorithm: *//p' | head -1)" in
        id-ecPublicKey) echo EC ;;
        ML-DSA-65) echo ML-DSA-65 ;;
        ML-DSA-87) echo ML-DSA-87 ;;
        *) die "unrecognised CA key algorithm in $1" ;;
    esac
}

# ---------------------------------------------------------------------------------------------
# Self-check: a certificate that is wrong is a mistake in this script, and the place to find that
# out is here, not at the first failed handshake. Includes negative checks, because a verifier that
# accepts everything also passes every positive check.
# ---------------------------------------------------------------------------------------------

verify_server_cert() {
    local ca_crt="$1" crt="$2" key="$3" n
    run openssl verify -x509_strict -purpose sslserver -CAfile "$ca_crt" "$crt"
    for n in "${SAN_DNS[@]}"; do
        run openssl verify -x509_strict -purpose sslserver -verify_hostname "$n" -CAfile "$ca_crt" "$crt"
    done
    for n in "${SAN_IP[@]}"; do
        run openssl verify -x509_strict -purpose sslserver -verify_ip "$n" -CAfile "$ca_crt" "$crt"
    done
    openssl x509 -in "$crt" -noout -ext basicConstraints | grep -q 'CA:FALSE' \
        || die "self-check: $crt is not marked CA:FALSE"
    [[ "$(pin_of_cert "$crt")" == "$(pin_of_key "$key")" ]] \
        || die "self-check: $crt does not belong to $key"
}

must_not_verify() {   # <what> <ca> <crt>
    if openssl verify -x509_strict -purpose sslserver -CAfile "$2" "$3" >/dev/null 2>&1; then
        die "self-check: $1 verified, but must not"
    fi
}

# Signs throwaway certificates for names outside the constraints and requires them to be refused.
# This is what proves a stolen CA key cannot impersonate anything beyond CA_PERMITTED.
verify_constraints() {
    local ca_key="$1" ca_crt="$2" ca_alg="$3" probe_key="$STAGE/tmp/probe.key" probe="$STAGE/tmp/probe.crt"
    new_key "$probe_key" EC
    sign_leaf "$probe_key" "$probe" "$ca_key" "$ca_crt" "$ca_alg" encrypted minidrive-nc-probe.test --
    must_not_verify "a certificate for DNS:minidrive-nc-probe.test (outside CA_PERMITTED)" "$ca_crt" "$probe"
    sign_leaf "$probe_key" "$probe" "$ca_key" "$ca_crt" "$ca_alg" encrypted -- 203.0.113.77
    must_not_verify "a certificate for IP:203.0.113.77 (outside CA_PERMITTED)" "$ca_crt" "$probe"
}

# ---------------------------------------------------------------------------------------------
# init
# ---------------------------------------------------------------------------------------------

do_init() {
    if (( LAB )); then
        SERVER_SANS="${SERVER_SANS:-$LAB_SANS}"
    else
        [[ -n ${SERVER_SANS:-} ]] || die "set SERVER_SANS to every name and address clients will dial, e.g.
  SERVER_SANS=\"DNS:nas.example.com,IP:192.168.1.11\" $PROG init
(a name missing from the certificate is a refused connection). --lab defaults to localhost names."
    fi
    parse_sans "$SERVER_SANS"
    parse_permitted "${CA_PERMITTED:-$SERVER_SANS}"
    check_ca_alg

    mkdir -p "$OUT_DIR"
    local existing=() p
    for p in ca server client adversary ca.key ca.crt server.key server.crt; do
        [[ -e $OUT_DIR/$p ]] && existing+=("$p")
    done
    if (( ${#existing[@]} )); then
        (( FORCE )) || die "$OUT_DIR already holds a PKI (${existing[*]}). Replacing it invalidates every
client's ca.crt and pin. Use '$PROG renew' to re-issue the server certificate with the same keys, or
'init --force' to replace everything (the old files are moved aside, not deleted)."
    fi

    setup_passphrase 1
    STAGE="$(mktemp -d "$OUT_DIR/.gen-certs.XXXXXX")"
    mkdir -p "$STAGE"/{ca,server,client,tmp}

    echo "Creating a new PKI in $OUT_DIR"
    echo "  server SANs:    $SERVER_SANS"
    echo "  CA may sign:    ${CA_PERMITTED:-$SERVER_SANS}"
    echo "  CA key:         $CA_KEY_ALG, ${CA_DAYS} days; server certificate ${LEAF_DAYS} days"

    make_ca "$STAGE/ca/ca.key" "$STAGE/ca/ca.crt" "$CA_KEY_ALG" encrypt
    printf '%s\n' "$SERVER_SANS" > "$STAGE/ca/server.sans"
    new_key "$STAGE/server/server.key" EC
    sign_leaf "$STAGE/server/server.key" "$STAGE/server/server.crt" "$STAGE/ca/ca.key" "$STAGE/ca/ca.crt" \
        "$CA_KEY_ALG" encrypted "${SAN_DNS[@]}" -- "${SAN_IP[@]}"

    local pin; pin="$(pin_of_cert "$STAGE/server/server.crt")"
    printf 'sha256:%s\n' "$pin" > "$STAGE/server/server.pin"
    cp "$STAGE/ca/ca.crt" "$STAGE/server/server.pin" "$STAGE/client/"

    verify_server_cert "$STAGE/ca/ca.crt" "$STAGE/server/server.crt" "$STAGE/server/server.key"
    verify_constraints "$STAGE/ca/ca.key" "$STAGE/ca/ca.crt" "$CA_KEY_ALG"

    local rogue_pin=""
    if (( LAB )); then
        # The impostor: a CA with the real CA's exact name and constraints - what an attacker would
        # copy - and a certificate for the real server's names signed by it. Only the keys differ,
        # so anything that matches an issuer by name instead of by signature accepts it.
        mkdir -p "$STAGE/adversary"
        make_ca "$STAGE/adversary/rogue-ca.key" "$STAGE/adversary/rogue-ca.crt" EC plain
        new_key "$STAGE/adversary/rogue-server.key" EC
        sign_leaf "$STAGE/adversary/rogue-server.key" "$STAGE/adversary/rogue-server.crt" \
            "$STAGE/adversary/rogue-ca.key" "$STAGE/adversary/rogue-ca.crt" EC plain \
            "${SAN_DNS[@]}" -- "${SAN_IP[@]}"
        rogue_pin="$(pin_of_cert "$STAGE/adversary/rogue-server.crt")"
        printf 'sha256:%s\n' "$rogue_pin" > "$STAGE/adversary/rogue-server.pin"
        verify_server_cert "$STAGE/adversary/rogue-ca.crt" "$STAGE/adversary/rogue-server.crt" \
            "$STAGE/adversary/rogue-server.key"
        must_not_verify "the rogue server certificate against the real CA" \
            "$STAGE/ca/ca.crt" "$STAGE/adversary/rogue-server.crt"
        must_not_verify "the real server certificate against the rogue CA" \
            "$STAGE/adversary/rogue-ca.crt" "$STAGE/server/server.crt"
    fi

    rm -rf "$STAGE/tmp"
    chmod 755 "$STAGE/client"; chmod 644 "$STAGE/client/"* "$STAGE/server/server.pin"
    (( LAB )) && chmod 644 "$STAGE/adversary/rogue-server.pin"

    # Commit. Only now is anything already in $OUT_DIR touched.
    if (( ${#existing[@]} )); then
        local aside; aside="$OUT_DIR/replaced-$(date +%Y%m%d-%H%M%S)"
        mkdir -p "$aside"
        for p in "${existing[@]}" ca.srl server.pin rogue-ca.crt rogue-ca.key rogue-server.crt \
                 rogue-server.key rogue-server.pin; do
            [[ -e $OUT_DIR/$p ]] && mv "$OUT_DIR/$p" "$aside/"
        done
        echo "  previous PKI moved to $aside"
    fi
    for p in adversary client server ca; do
        [[ -d $STAGE/$p ]] && mv "$STAGE/$p" "$OUT_DIR/$p"
    done

    cat <<SUMMARY

Done. Verified: chain, purpose, every SAN, CA:FALSE leaf, name constraints enforced.

  ca/       KEEP HERE. ca.key is passphrase-encrypted; it is needed only to renew.
  server/   copy to the server, e.g.  scp -r $OUT_DIR/server nas:/etc/minidrive/certs
            server --port 9000 --root <root> --rung 3.5 \\
                   --tls-cert <dir>/server.crt --tls-key <dir>/server.key
  client/   give to clients:
            client user@<host>:9000 --rung 3.5 --ca-file ca.crt --pin \$(cat server.pin)

  Server public key pin: sha256:$pin
  The certificate expires in $LEAF_DAYS days. Renew before then with the same keys (pin and ca.crt
  stay valid):  $PROG renew $OUT_DIR
SUMMARY
    if (( LAB )); then
        cat <<LAB

  adversary/  LAB ONLY - an impostor CA named exactly like the real one. Never bundle or deploy it.
  Rogue public key pin:  sha256:$rogue_pin
LAB
    fi
}

# ---------------------------------------------------------------------------------------------
# renew
# ---------------------------------------------------------------------------------------------

do_renew() {
    local ca_key="$OUT_DIR/ca/ca.key" ca_crt="$OUT_DIR/ca/ca.crt"
    local key="$OUT_DIR/server/server.key" crt="$OUT_DIR/server/server.crt"
    [[ -f $ca_key && -f $ca_crt ]] || die "no CA in $OUT_DIR/ca/ (renew runs where the CA key lives; create one with init)"
    [[ -f $key ]] || die "no server key at $key (renew reuses it so the pin stays valid)"

    if [[ -n $IF_EXPIRING && -f $crt ]] \
        && openssl x509 -in "$crt" -noout -checkend $(( IF_EXPIRING * 86400 )) >/dev/null; then
        echo "$crt is valid for more than $IF_EXPIRING days; nothing to do."
        return 0
    fi

    if [[ -z ${SERVER_SANS:-} ]]; then
        [[ -f $OUT_DIR/ca/server.sans ]] || die "set SERVER_SANS (no recorded SANs in $OUT_DIR/ca/server.sans)"
        SERVER_SANS="$(<"$OUT_DIR/ca/server.sans")"
    fi
    parse_sans "$SERVER_SANS"
    local ca_alg; ca_alg="$(ca_alg_of "$ca_crt")"
    openssl x509 -in "$ca_crt" -noout -checkend $(( LEAF_DAYS * 86400 )) >/dev/null \
        || warn "the CA certificate expires within $LEAF_DAYS days; the new certificate cannot outlive it"

    local old_pin; old_pin="$(pin_of_key "$key")"
    setup_passphrase 0
    STAGE="$(mktemp -d "$OUT_DIR/.gen-certs.XXXXXX")"
    mkdir -p "$STAGE/tmp"

    echo "Renewing $crt (same key, ${LEAF_DAYS} days)"
    echo "  server SANs: $SERVER_SANS"
    sign_leaf "$key" "$STAGE/server.crt" "$ca_key" "$ca_crt" "$ca_alg" encrypted \
        "${SAN_DNS[@]}" -- "${SAN_IP[@]}"
    verify_server_cert "$ca_crt" "$STAGE/server.crt" "$key"
    [[ "$(pin_of_cert "$STAGE/server.crt")" == "$old_pin" ]] || die "self-check: the pin changed on renewal"

    mv "$STAGE/server.crt" "$crt"
    printf '%s\n' "$SERVER_SANS" > "$STAGE/server.sans" && mv "$STAGE/server.sans" "$OUT_DIR/ca/server.sans"

    cat <<SUMMARY

Done. Pin unchanged: sha256:$old_pin
New expiry: $(openssl x509 -in "$crt" -noout -enddate | cut -d= -f2)
Copy $crt to the server and restart it; clients need nothing.
SUMMARY
}

case "$MODE" in
    init) do_init ;;
    renew) do_renew ;;
esac
