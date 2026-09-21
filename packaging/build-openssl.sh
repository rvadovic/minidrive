#!/usr/bin/env bash
#
# Builds the pinned OpenSSL (packaging/openssl-version.env) as static libraries, for the fully static
# client (-DMINIDRIVE_FULLY_STATIC=ON -DOPENSSL_ROOT_DIR=<prefix>).
#
# Usage:
#   packaging/build-openssl.sh <prefix>                 # Linux, macOS: download, verify, build, install
#   packaging/build-openssl.sh --fetch-only <src_dir>   # download + verify + unpack only (Windows,
#                                                       # where the build itself needs nmake + MSVC)
#
# The tarball is checked against the pinned SHA-256 before anything in it is run.
#
# --openssldir=/etc/ssl: an embedded OpenSSL otherwise looks for its default trust store under the
# build prefix, which exists on no user's machine. /etc/ssl is where Debian, Ubuntu, Alpine and Arch
# keep their CA certificates, and macOS ships /etc/ssl/cert.pem. It only matters for the
# "--tls-verify ca" mode without --ca-file; a MiniDrive deployment normally passes --ca-file or
# --pin, which do not touch the default store at all. SSL_CERT_FILE / SSL_CERT_DIR override it.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=openssl-version.env
source "$here/openssl-version.env"

fetch_only=0
if [[ "${1:-}" == "--fetch-only" ]]; then
    fetch_only=1
    shift
fi
dest="${1:?usage: build-openssl.sh [--fetch-only] <dir>}"
mkdir -p "$dest"
dest="$(cd "$dest" && pwd)"

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

name="openssl-${OPENSSL_VERSION}"
url="https://github.com/openssl/openssl/releases/download/${name}/${name}.tar.gz"
echo "Fetching $url"
curl -fsSL --retry 3 -o "$work/$name.tar.gz" "$url"

actual="$(sha256 "$work/$name.tar.gz")"
if [[ "$actual" != "$OPENSSL_SHA256" ]]; then
    echo "SHA-256 mismatch for $name.tar.gz" >&2
    echo "  expected $OPENSSL_SHA256" >&2
    echo "  got      $actual" >&2
    exit 1
fi
echo "SHA-256 verified: $actual"

if [[ $fetch_only -eq 1 ]]; then
    tar -xzf "$work/$name.tar.gz" -C "$dest" --strip-components=1
    echo "Unpacked $name into $dest"
    exit 0
fi

tar -xzf "$work/$name.tar.gz" -C "$work"
cd "$work/$name"

jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"

# no-shared: static archives only, so find_package(OpenSSL) cannot pick a .so/.dylib by accident.
# no-module: providers are built into libcrypto rather than as loadable modules.
# --libdir=lib: otherwise 64-bit Linux installs into lib64, which FindOpenSSL may not search.
./Configure no-shared no-module no-tests \
    --prefix="$dest" --libdir=lib --openssldir=/etc/ssl

# build_libs + install_dev: libraries, headers and pkg-config files only - the openssl CLI and the
# man pages are not needed to link a client, and skipping them roughly halves the build.
make -j"$jobs" build_libs
make install_dev

echo "Installed static $name into $dest"
